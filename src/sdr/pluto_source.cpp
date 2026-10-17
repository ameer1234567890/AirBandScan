#include "sdr/pluto_source.h"

#include "util/log.h"

// libiio 1.x moved the header to <iio/iio.h> and changed the attribute,
// scan and buffer APIs; the small wrappers below hide the difference.
#if defined(IIO_V1)
#include <iio/iio.h>
#else
#include <iio.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
// The AD9361 needs its FIR decimator below ~2.08 MS/s; stay above that.
constexpr double kMinRate = 2.1e6;
constexpr double kMaxRate = 20.0e6;

std::string errStr(int e)
{
    char buf[128] = {0};
    iio_strerror(e < 0 ? -e : e, buf, sizeof(buf));
    return buf;
}

bool isPluto(const std::string& desc)
{
    return desc.find("Pluto") != std::string::npos || desc.find("PLUTO") != std::string::npos
        || desc.find("AD936") != std::string::npos || desc.find("ad936") != std::string::npos;
}

#if defined(IIO_V1)
const iio_attr* attr(const iio_channel* ch, const char* name)
{
    return ch ? iio_channel_find_attr(ch, name) : nullptr;
}
int writeStr(const iio_channel* ch, const char* name, const char* v)
{
    auto* a = attr(ch, name);
    return a ? (int)iio_attr_write_string(a, v) : -ENOENT;
}
int writeLL(const iio_channel* ch, const char* name, long long v)
{
    auto* a = attr(ch, name);
    return a ? iio_attr_write_longlong(a, v) : -ENOENT;
}
int readLL(const iio_channel* ch, const char* name, long long* v)
{
    auto* a = attr(ch, name);
    return a ? iio_attr_read_longlong(a, v) : -ENOENT;
}
int writeBool(const iio_channel* ch, const char* name, bool v)
{
    auto* a = attr(ch, name);
    return a ? iio_attr_write_bool(a, v) : -ENOENT;
}
#else
int writeStr(const iio_channel* ch, const char* name, const char* v)
{
    return (int)iio_channel_attr_write(ch, name, v);
}
int writeLL(const iio_channel* ch, const char* name, long long v)
{
    return iio_channel_attr_write_longlong(ch, name, v);
}
int readLL(const iio_channel* ch, const char* name, long long* v)
{
    return iio_channel_attr_read_longlong(ch, name, v);
}
int writeBool(const iio_channel* ch, const char* name, bool v)
{
    return iio_channel_attr_write_bool(ch, name, v);
}
#endif

std::vector<SdrDeviceInfo> scan()
{
    std::vector<SdrDeviceInfo> out;
    auto add = [&](const char* desc, const char* uri) {
        if (!desc || !uri || !isPluto(desc))
            return;
        SdrDeviceInfo d;
        d.index = (int)out.size();
        d.name = desc;
        d.serial = uri;
        out.push_back(d);
    };
#if defined(IIO_V1)
    // usb + ip (DNS-SD / mDNS, when libiio was built with it).
    struct iio_scan* sc = iio_scan(nullptr, "usb,ip");
    if (iio_err(sc))
        return out;
    for (size_t i = 0; i < iio_scan_get_results_count(sc); ++i)
        add(iio_scan_get_description(sc, i), iio_scan_get_uri(sc, i));
    iio_scan_destroy(sc);
#else
    // NULL = every backend libiio was built with (usb, and ip via DNS-SD / mDNS).
    iio_scan_context* sc = iio_create_scan_context(nullptr, 0);
    if (!sc)
        return out;
    iio_context_info** info = nullptr;
    ssize_t n = iio_scan_context_get_info_list(sc, &info);
    for (ssize_t i = 0; i < n; ++i)
        add(iio_context_info_get_description(info[i]), iio_context_info_get_uri(info[i]));
    if (n > 0)
        iio_context_info_list_free(info);
    iio_scan_context_destroy(sc);
#endif
    return out;
}
} // namespace

PlutoSource::~PlutoSource()
{
    stop();
}

std::string PlutoSource::normaliseUri(const std::string& in)
{
    std::string s = in;
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    if (s.empty())
        return s;
    // A bare IPv6 address contains ':' too, so check the scheme explicitly.
    for (const char* p : {"ip:", "usb:", "local:", "serial:"})
        if (s.rfind(p, 0) == 0)
            return s;
    return "ip:" + s;
}

std::vector<SdrDeviceInfo> PlutoSource::listDevices()
{
    return scan();
}

bool PlutoSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    std::string uri = normaliseUri(uri_);
    if (uri.empty())
    {
        auto found = scan();
        if (!found.empty())
            uri = found[std::clamp(deviceIndex, 0, (int)found.size() - 1)].serial;
        else
            uri = "ip:192.168.2.1"; // Pluto's default address (USB network or Ethernet)
    }
    logWrite("[pluto] connecting to %s", uri.c_str());

#if defined(IIO_V1)
    ctx_ = iio_create_context(nullptr, uri.c_str());
    int cerr = iio_err(ctx_);
    if (cerr)
        ctx_ = nullptr;
#else
    ctx_ = iio_create_context_from_uri(uri.c_str());
    int cerr = errno;
#endif
    if (!ctx_)
    {
        err = "PlutoSDR: cannot connect to " + uri + " (" + errStr(cerr) + ")";
        return false;
    }
    if (iio_context_set_timeout(ctx_, 5000) < 0)
        logWrite("[pluto] could not set the context timeout");
    phy_ = iio_context_find_device(ctx_, "ad9361-phy");
    rx_ = iio_context_find_device(ctx_, "cf-ad9361-lpc");
    if (phy_)
    {
        rxCfg_ = iio_device_find_channel(phy_, "voltage0", false);
        lo_ = iio_device_find_channel(phy_, "altvoltage0", true);
    }
    if (rx_)
    {
        chI_ = iio_device_find_channel(rx_, "voltage0", false);
        chQ_ = iio_device_find_channel(rx_, "voltage1", false);
    }
    if (!phy_ || !rx_ || !rxCfg_ || !lo_ || !chI_ || !chQ_)
    {
        err = "PlutoSDR: " + uri + " is not an AD9361/AD9363 radio";
        closeAll();
        return false;
    }

    sampleRate_ = std::clamp(sampleRate_, kMinRate, kMaxRate);
    writeStr(rxCfg_, "rf_port_select", "A_BALANCED");
    writeLL(rxCfg_, "sampling_frequency", (long long)std::llround(sampleRate_));
    writeLL(rxCfg_, "rf_bandwidth", (long long)std::llround(sampleRate_));
    long long actual = 0;
    if (readLL(rxCfg_, "sampling_frequency", &actual) == 0 && actual > 0)
        sampleRate_ = (double)actual;
    // The RX LO power-down attribute only exists on newer firmware; ignore errors.
    writeBool(lo_, "powerdown", false);
    if (!applyTune())
    {
        char mhz[32];
        std::snprintf(mhz, sizeof(mhz), "%.3f", centerFreq_ / 1e6);
        err = std::string("PlutoSDR rejected ") + mhz +
              " MHz: a stock Pluto tunes 325-3800 MHz only. Enable the AD9364 range (70 MHz-6 GHz):"
              " ssh root@pluto, then fw_setenv attr_name compatible; fw_setenv attr_val ad9364; reboot";
        closeAll();
        return false;
    }
    applyGain();

    // ~20 ms per buffer: short enough that hopping does not waste dwell time.
    size_t samples = (size_t)std::max(4096.0, std::ceil(sampleRate_ * 0.02 / 1024.0) * 1024.0);
#if defined(IIO_V1)
    mask_ = iio_create_channels_mask(iio_device_get_channels_count(rx_));
    if (!mask_)
    {
        err = "PlutoSDR: out of memory";
        closeAll();
        return false;
    }
    iio_channel_enable(chI_, mask_);
    iio_channel_enable(chQ_, mask_);
    buf_ = iio_device_create_buffer(rx_, 0, mask_);
    int berr = iio_err(buf_);
    if (berr)
        buf_ = nullptr;
    else
    {
        stream_ = iio_buffer_create_stream(buf_, 4, samples);
        berr = iio_err(stream_);
        if (berr)
            stream_ = nullptr;
    }
    sampleSize_ = berr ? 0 : iio_device_get_sample_size(rx_, mask_);
    if (!berr && sampleSize_ <= 0)
        berr = -EINVAL;
#else
    iio_channel_enable(chI_);
    iio_channel_enable(chQ_);
    iio_device_set_kernel_buffers_count(rx_, 4);
    buf_ = iio_device_create_buffer(rx_, samples, false);
    int berr = buf_ ? 0 : errno;
#endif
    if (berr)
    {
        err = "PlutoSDR: cannot create RX buffer (" + errStr(berr) + ")";
        closeAll();
        return false;
    }

    cb_ = std::move(cb);
    running_.store(true);
    thread_ = std::thread(&PlutoSource::readLoop, this);
    return true;
}

void PlutoSource::readLoop()
{
    std::vector<float> out;
    float offRe = 0.0f, offIm = 0.0f;
    const float rate = (float)(50.0 / sampleRate_);
    while (running_.load())
    {
#if defined(IIO_V1)
        const iio_block* blk = iio_stream_get_next_block(stream_);
        int rerr = iio_err(blk);
        const char* p = rerr ? nullptr : (const char*)iio_block_first(blk, chI_);
        const char* end = rerr ? nullptr : (const char*)iio_block_end(blk);
        const ptrdiff_t step = sampleSize_;
#else
        ssize_t r = iio_buffer_refill(buf_);
        int rerr = r < 0 ? (int)r : 0;
        const char* p = rerr ? nullptr : (const char*)iio_buffer_first(buf_, chI_);
        const char* end = rerr ? nullptr : (const char*)iio_buffer_end(buf_);
        const ptrdiff_t step = rerr ? 1 : iio_buffer_step(buf_);
#endif
        if (!running_.load())
            break;
        if (rerr)
        {
            logWrite("[pluto] read failed: %s", errStr(rerr).c_str());
            running_.store(false); // the scanner reports "SDR stopped delivering samples"
            break;
        }
        // Samples are interleaved little-endian int16 I/Q (12-bit ADC).
        size_t n = (size_t)((end - p) / step);
        out.resize(n * 2);
        const bool dc = dcBlock_.load();
        for (size_t i = 0; i < n; ++i, p += step)
        {
            int16_t s[2];
            std::memcpy(s, p, sizeof(s));
            float re = s[0] * (1.0f / 2048.0f);
            float im = s[1] * (1.0f / 2048.0f);
            if (dc)
            {
                re -= offRe; offRe += re * rate;
                im -= offIm; offIm += im * rate;
            }
            out[i * 2] = re;
            out[i * 2 + 1] = im;
        }
        if (cb_ && n)
            cb_(out.data(), (int)n);
    }
}

void PlutoSource::stop()
{
    bool wasRunning = running_.exchange(false);
    if (buf_ && wasRunning)
        iio_buffer_cancel(buf_); // wakes a blocked refill / get_next_block
    if (thread_.joinable())
        thread_.join();
    closeAll();
    cb_ = nullptr;
}

void PlutoSource::closeAll()
{
#if defined(IIO_V1)
    if (stream_)
        iio_stream_destroy(stream_);
    if (buf_)
        iio_buffer_destroy(buf_);
    if (mask_)
        iio_channels_mask_destroy(mask_);
    stream_ = nullptr;
    mask_ = nullptr;
    sampleSize_ = 0;
#else
    if (buf_)
        iio_buffer_destroy(buf_);
    if (chI_) iio_channel_disable(chI_);
    if (chQ_) iio_channel_disable(chQ_);
#endif
    if (ctx_)
        iio_context_destroy(ctx_);
    buf_ = nullptr;
    ctx_ = nullptr;
    phy_ = rx_ = nullptr;
    rxCfg_ = lo_ = chI_ = chQ_ = nullptr;
}

bool PlutoSource::applyTune()
{
    if (!lo_)
        return true;
    long long hz = (long long)std::llround(centerFreq_ * (1.0 + ppm_ / 1e6));
    if (writeLL(lo_, "frequency", hz) < 0)
        return false;
    long long back = 0; // out-of-range values may be clamped instead of rejected
    if (readLL(lo_, "frequency", &back) == 0 && std::llabs(back - hz) > 1000)
        return false;
    return true;
}

void PlutoSource::applyGain()
{
    if (!rxCfg_)
        return;
    if (gainDb_ < 0)
    {
        writeStr(rxCfg_, "gain_control_mode", "slow_attack");
    }
    else
    {
        writeStr(rxCfg_, "gain_control_mode", "manual");
        writeLL(rxCfg_, "hardwaregain", (long long)std::lround(std::clamp(gainDb_, 0.0, 71.0)));
    }
}

void PlutoSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (lo_ && !applyTune())
        logWrite("[pluto] LO %.3f MHz rejected", hz / 1e6);
}

void PlutoSource::setPpm(double ppm)
{
    ppm_ = ppm;
    applyTune();
}

void PlutoSource::setSampleRate(double hz)
{
    sampleRate_ = hz; // applied on start
}

void PlutoSource::setGain(double db)
{
    gainDb_ = db;
    applyGain();
}
