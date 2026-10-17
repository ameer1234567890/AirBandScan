#include "sdr/airspyhf_source.h"

#include <libairspyhf/airspyhf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
int rxTrampoline(airspyhf_transfer_t* transfer)
{
    auto* self = static_cast<AirspyHfSource*>(transfer->ctx);
    return self->handleRx((const float*)transfer->samples, transfer->sample_count);
}
} // namespace

AirspyHfSource::~AirspyHfSource()
{
    stop();
}

std::vector<SdrDeviceInfo> AirspyHfSource::listDevices()
{
    std::vector<SdrDeviceInfo> out;
    uint64_t serials[64];
    int n = airspyhf_list_devices(serials, 64);
    for (int i = 0; i < n; ++i)
    {
        SdrDeviceInfo info;
        info.index = i;
        info.name = "Airspy HF+";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)serials[i]);
        info.serial = buf;
        out.push_back(info);
    }
    return out;
}

bool AirspyHfSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    uint64_t serials[64];
    int n = airspyhf_list_devices(serials, 64);
    if (n <= 0)
    {
        err = "No Airspy HF+ device found";
        return false;
    }
    if (deviceIndex < 0 || deviceIndex >= n)
        deviceIndex = 0;
    if (airspyhf_open_sn(&dev_, serials[deviceIndex]) != AIRSPYHF_SUCCESS || !dev_)
    {
        err = "Airspy HF+ open failed (in use, or no USB permission?)";
        dev_ = nullptr;
        return false;
    }

    // Pick the supported rate closest to the requested one (HF+ Dual Port:
    // 768/384/256/192 kS/s; Discovery adds 912/456 kS/s and others).
    uint32_t count = 0;
    airspyhf_get_samplerates(dev_, &count, 0);
    std::vector<uint32_t> rates(count ? count : 1, 768000);
    if (count)
        airspyhf_get_samplerates(dev_, rates.data(), count);
    uint32_t best = rates[0];
    for (uint32_t r : rates)
        if (std::fabs((double)r - sampleRate_) < std::fabs((double)best - sampleRate_))
            best = r;
    if (airspyhf_set_samplerate(dev_, best) != AIRSPYHF_SUCCESS)
    {
        err = "Airspy HF+: sample rate " + std::to_string(best) + " rejected";
        airspyhf_close(dev_);
        dev_ = nullptr;
        return false;
    }
    sampleRate_ = best;

    // The library does IQ correction, IF shift (no DC spike) and fine tuning.
    airspyhf_set_lib_dsp(dev_, 1);
    airspyhf_set_calibration(dev_, (int32_t)std::lround(ppm_ * 1000.0));
    airspyhf_set_freq(dev_, (uint32_t)std::llround(centerFreq_));
    applyGain();

    cb_ = std::move(cb);
    running_.store(true);
    if (airspyhf_start(dev_, &rxTrampoline, this) != AIRSPYHF_SUCCESS)
    {
        running_.store(false);
        err = "Airspy HF+ start failed";
        airspyhf_close(dev_);
        dev_ = nullptr;
        cb_ = nullptr;
        return false;
    }
    return true;
}

void AirspyHfSource::stop()
{
    running_.store(false);
    if (dev_)
    {
        airspyhf_stop(dev_);
        airspyhf_close(dev_);
        dev_ = nullptr;
    }
    cb_ = nullptr;
}

int AirspyHfSource::handleRx(const float* iq, int n)
{
    if (running_.load() && cb_ && n > 0)
        cb_(iq, n);
    return 0;
}

void AirspyHfSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (dev_)
        airspyhf_set_freq(dev_, (uint32_t)std::llround(hz));
}

void AirspyHfSource::setSampleRate(double hz)
{
    sampleRate_ = hz; // applied (and snapped) on start
}

void AirspyHfSource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (dev_)
        airspyhf_set_calibration(dev_, (int32_t)std::lround(ppm * 1000.0));
}

void AirspyHfSource::setGain(double db)
{
    gainDb_ = db;
    applyGain();
}

void AirspyHfSource::applyGain()
{
    if (!dev_)
        return;
    if (gainDb_ < 0)
    {
        airspyhf_set_hf_agc(dev_, 1);
        airspyhf_set_hf_att(dev_, 0);
    }
    else
    {
        // No real gain stages: "gain" 48 = no attenuation, 0 = 48 dB attenuation.
        airspyhf_set_hf_agc(dev_, 0);
        int att = (int)std::lround((48.0 - std::clamp(gainDb_, 0.0, 48.0)) / 6.0);
        airspyhf_set_hf_att(dev_, (uint8_t)std::clamp(att, 0, 8));
    }
    airspyhf_set_hf_lna(dev_, 1);
}
