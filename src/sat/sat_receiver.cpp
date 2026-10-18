#include "sat/sat_receiver.h"

#include "engine/scanner.h"
#include "sdr/wav_file_source.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr double kMaxQueueSec = 1.0;
constexpr int64_t kPassEndMs = 90 * 1000; // LRPT: no frame for this long ends a pass
constexpr double kSstvAudioRate = 12000.0;
}

int64_t SatReceiver::nowMs() const
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

bool SatReceiver::start(const Config& cfg, std::string& err)
{
    stop();
    cfg_ = cfg;
    lrpt_ = cfg.satMode == "lrpt";
    src_ = Scanner::createSource(cfg.device);
    if (!src_)
    {
        err = "device type '" + cfg.device + "' is not supported by this build";
        std::lock_guard<std::mutex> lk(dspMtx_);
        error_ = err;
        return false;
    }
    double centre;
    if (auto* w = dynamic_cast<WavFileSource*>(src_.get()))
    {
        w->setPath(cfg.wavPath);
        w->setLoop(false);
        w->setCenterFreq(cfg.wavCenterHz);
        centre = cfg.wavCenterHz;
    }
    else
    {
        // One narrow channel needs no wide bandwidth: run an RTL-SDR at
        // 1.024 MS/s to save CPU.
        double rate = cfg.sampleRate;
        if ((cfg.device == "rtlsdr" || cfg.device == "rtltcp") && rate > 1.1e6) rate = 1.024e6;
        // Tune off to the side so the SDR's DC spike stays out of the channel.
        double off = std::min(250e3, rate * 0.25);
        centre = cfg.satFreqHz + off;
        src_->setAddress(cfg.device == "rtltcp" ? cfg.rtlTcpAddress : cfg.plutoUri);
        src_->setSampleRate(rate);
        src_->setGain(cfg.gainDb);
        src_->setPpm(cfg.ppm);
        src_->setBiasTee(cfg.biasTee);
        src_->setCenterFreq(centre);
    }
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        queue_.clear();
        queued_ = 0;
    }
    drops_ = 0;
    quit_ = false;
    if (!src_->start(cfg.deviceIndex, [this](const float* iq, int n) { onSamples(iq, n); }, err))
    {
        logWrite("[sat] start failed: %s", err.c_str());
        src_.reset();
        std::lock_guard<std::mutex> lk(dspMtx_);
        error_ = err;
        return false;
    }

    std::lock_guard<std::mutex> lk(dspMtx_);
    fs_ = src_->sampleRate();
    offsetHz_ = cfg.satFreqHz - centre;
    if (std::fabs(offsetHz_) > fs_ * 0.45)
    {
        err = "the satellite frequency is outside the IQ file's bandwidth";
        error_ = err;
        src_->stop();
        src_.reset();
        return false;
    }
    error_.clear();
    fileDone_ = false;
    imagesSaved_ = 0;
    levelDb_ = -120.f;
    sstv_.reset();
    chain_.reset();
    passActive_ = false;
    lastFrames_ = 0;
    liveCache_.clear();
    liveCacheVersion_ = ~0ull;
    if (lrpt_)
    {
        // ~4 samples per symbol; +-85 kHz holds the 120 kHz signal plus Doppler and tuning error.
        chan_ = std::make_unique<Channelizer>(fs_, offsetHz_, 170000.0, 288000.0, 20000.0);
        chain_ = std::make_unique<LrptChain>(chan_->outputRate(), lrptIsM2x(cfg.satLrptVariant));
    }
    else
    {
        chan_ = std::make_unique<Channelizer>(fs_, offsetHz_, cfg.satSstvBwHz, std::max(48000.0, cfg.satSstvBwHz * 1.6),
                                              5000.0);
        double r = chan_->outputRate();
        audDecim_ = std::max(1, (int)std::lround(r / kSstvAudioRate));
        lp1_ = Biquad::lowpass(r, 3000.0);
        lp2_ = Biquad::lowpass(r, 3000.0);
        audPhase_ = 0;
        sstv_ = std::make_unique<SstvDecoder>(r / audDecim_);
    }
    startedMs_ = nowMs();
    logWrite("[sat] %s on %.4f MHz (%s @ %.3f MS/s, channel %.0f S/s)",
             lrpt_ ? (lrptIsM2x(cfg.satLrptVariant) ? "Meteor-M LRPT (OQPSK)" : "Meteor-M LRPT (QPSK)") : "SSTV",
             cfg.satFreqHz / 1e6, cfg.device.c_str(), fs_ / 1e6, chan_->outputRate());
    running_ = true;
    thread_ = std::thread([this] { dspLoop(); });
    return true;
}

void SatReceiver::stop()
{
    quit_ = true;
    qCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (src_)
    {
        src_->stop();
        src_.reset();
    }
    std::lock_guard<std::mutex> lk(dspMtx_);
    if (running_)
    {
        if (sstv_)
        {
            sstv_->flush();
            SstvDecoder::Result r;
            while (sstv_->take(r)) saveSstv(r);
        }
        if (chain_) finishPass("receiver stopped");
    }
    running_ = false;
}

void SatReceiver::onSamples(const float* iq, int n)
{
    auto b = std::make_shared<IqBlock>();
    b->iq.assign(iq, iq + 2 * n);
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        if (queued_ > (size_t)(fs_ > 0 ? fs_ * kMaxQueueSec : 4e6))
        {
            drops_++;
            return;
        }
        queued_ += (size_t)n;
        queue_.push_back(std::move(b));
    }
    qCv_.notify_one();
}

void SatReceiver::dspLoop()
{
    while (!quit_)
    {
        std::shared_ptr<IqBlock> b;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            if (!queue_.empty())
            {
                b = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= (size_t)b->count();
            }
        }
        std::lock_guard<std::mutex> lk(dspMtx_);
        if (b)
            processBlock(*b);
        else if (src_ && !src_->running() && !fileDone_)
        {
            // End of an IQ file (or the SDR went away): close what is open.
            fileDone_ = true;
            if (cfg_.device != "wav" && error_.empty()) error_ = "SDR stopped delivering samples";
            if (sstv_)
            {
                sstv_->flush();
                SstvDecoder::Result r;
                while (sstv_->take(r)) saveSstv(r);
            }
            if (chain_) finishPass(cfg_.device == "wav" ? "end of file" : "SDR stopped");
            ++liveVersion_;
        }
        if (chain_ && passActive_ && nowMs() - lastFrameMs_ > kPassEndMs) finishPass("signal lost");
    }
}

void SatReceiver::processBlock(const IqBlock& b)
{
    chBuf_.clear();
    chan_->process(b.iq.data(), b.count(), chBuf_);
    if (chBuf_.empty()) return;
    double p = 0;
    for (const cf& s : chBuf_) p += std::norm(s);
    float db = 10.f * std::log10((float)(p / chBuf_.size()) + 1e-20f);
    levelDb_ += (db - levelDb_) * 0.1f;

    if (chain_)
    {
        chain_->process(chBuf_.data(), (int)chBuf_.size());
        uint64_t f = chain_->decoder().frames();
        if (f != lastFrames_)
        {
            int64_t now = nowMs();
            if (!passActive_)
            {
                passActive_ = true;
                passStartMs_ = now;
                logWrite("[sat] LRPT: frames are coming in (pass started)");
            }
            lastFrames_ = f;
            lastFrameMs_ = now;
            ++liveVersion_;
        }
        return;
    }

    // NFM discriminator -> 3 kHz audio low-pass -> ~12 kHz for the SSTV decoder.
    audio_.clear();
    for (const cf& s : chBuf_)
    {
        float d = std::arg(s * std::conj(fmLast_));
        fmLast_ = s;
        float a = lp2_.run(lp1_.run(d));
        if (++audPhase_ >= audDecim_)
        {
            audPhase_ = 0;
            audio_.push_back(a);
        }
    }
    int before = sstv_->rowsDone();
    bool wasBusy = sstv_->busy();
    sstv_->process(audio_.data(), (int)audio_.size());
    if (sstv_->busy() && (!wasBusy || sstv_->rowsDone() != before)) ++liveVersion_;
    if (sstv_->busy() && !wasBusy)
        logWrite("[sat] SSTV: %s picture starting", sstv_->modeName().c_str());
    SstvDecoder::Result r;
    while (sstv_->take(r))
    {
        saveSstv(r);
        ++liveVersion_;
    }
}

void SatReceiver::saveSstv(const SstvDecoder::Result& r)
{
    ImageInfo info;
    info.kind = "sstv";
    info.mode = r.mode;
    int64_t now = nowMs();
    double ageSec = sstv_ ? sstv_->timeSec() - r.startSec : 0.0;
    info.startMs = now - (int64_t)(std::max(0.0, ageSec) * 1000.0);
    info.endMs = now;
    info.freqHz = cfg_.satFreqHz;
    info.complete = r.complete;
    if (!r.complete) info.note = std::to_string(r.rows) + " of " + std::to_string(r.totalRows) + " lines";
    if (images_.add(info, r.img, {}))
    {
        ++imagesSaved_;
        logWrite("[sat] SSTV %s picture saved: %s (%d/%d lines)", r.mode.c_str(), info.id.c_str(), r.rows, r.totalRows);
    }
}

void SatReceiver::finishPass(const char* why)
{
    LrptDecoder& dec = chain_->decoder();
    MsuMrImager& im = dec.imager();
    if (im.segments() > 0 && im.rows() >= 16)
    {
        ImageInfo info;
        info.kind = "lrpt";
        info.mode = lrptIsM2x(cfg_.satLrptVariant) ? "Meteor-M LRPT" : "Meteor-M N2 LRPT";
        info.startMs = passActive_ ? passStartMs_ : nowMs();
        info.endMs = passActive_ ? lastFrameMs_ : nowMs();
        info.freqHz = cfg_.satFreqHz;
        info.complete = true;
        char note[160];
        std::snprintf(note, sizeof(note), "%llu frames, %llu failed RS, channels",
                      (unsigned long long)dec.frames(), (unsigned long long)dec.framesBad());
        info.note = note;
        std::vector<std::pair<std::string, Image>> extra;
        for (int c : im.channels())
        {
            info.note += " " + std::to_string(c);
            extra.push_back({"_ch" + std::to_string(c), im.channel(c)});
        }
        if (images_.add(info, im.composite(), extra))
        {
            ++imagesSaved_;
            logWrite("[sat] LRPT pass saved (%s): %s, %d lines", why, info.id.c_str(), im.rows());
        }
    }
    im.reset();
    passActive_ = false;
    lastFrames_ = dec.frames();
    ++liveVersion_;
}

SatReceiver::Status SatReceiver::status()
{
    Status s;
    s.drops = drops_.load();
    std::lock_guard<std::mutex> lk(dspMtx_);
    s.running = running_.load();
    s.error = error_;
    s.mode = cfg_.satMode;
    s.variant = cfg_.satLrptVariant;
    s.device = cfg_.device;
    s.freqHz = cfg_.satFreqHz;
    s.fs = fs_;
    s.chRate = chan_ ? chan_->outputRate() : 0;
    s.levelDb = levelDb_;
    s.startedMs = startedMs_;
    s.fileDone = fileDone_;
    s.imagesSaved = imagesSaved_;
    s.liveVersion = liveVersion_;
    if (sstv_)
    {
        s.sstvBusy = sstv_->busy();
        s.sstvMode = sstv_->modeName();
        s.sstvRows = sstv_->rowsDone();
        s.sstvTotal = sstv_->rowsTotal();
        s.tone = sstv_->lastFreq();
    }
    if (chain_)
    {
        PskDemod& d = chain_->demod();
        LrptDecoder& dec = chain_->decoder();
        s.carrier = d.locked();
        s.lockQ = d.lockQuality();
        s.offsetHz = d.freqOffsetHz();
        s.viterbi = dec.locked();
        s.ber = dec.ber();
        s.frameSync = dec.frameSync();
        s.frames = dec.frames();
        s.framesBad = dec.framesBad();
        s.packets = dec.packets();
        s.rows = dec.imager().rows();
        s.channels = dec.imager().channels();
        s.passActive = passActive_;
        s.passStartMs = passStartMs_;
        s.constellation = d.constellation(256);
    }
    return s;
}

std::string SatReceiver::livePng()
{
    std::lock_guard<std::mutex> lk(dspMtx_);
    // Re-encode at most every 2 s (an LRPT composite grows to several MB).
    if (liveCacheVersion_ == liveVersion_ || (!liveCache_.empty() && nowMs() - liveCacheMs_ < 2000))
        return liveCache_;
    Image img;
    if (sstv_ && sstv_->busy())
    {
        img = sstv_->partial();
        img.h = std::max(1, sstv_->rowsDone());
        img.px.resize((size_t)img.w * img.h * img.ch);
    }
    else if (chain_ && chain_->decoder().imager().segments() > 0)
        img = scaleToWidth(chain_->decoder().imager().composite(), 784);
    liveCache_ = img.empty() ? std::string() : encodePng(img);
    liveCacheVersion_ = liveVersion_;
    liveCacheMs_ = nowMs();
    return liveCache_;
}
