#include "engine/channel.h"

#include "audio/audio_files.h"
#include "dsp/channelizer.h"
#include "dsp/jfft.h"
#include "engine/classifier.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
constexpr int    kSpgFft = 256;
constexpr int    kSpgHop = 128;
constexpr double kSpgRowsPerSec = 25.0;
constexpr double kLeadSec = 0.30; // audio kept before the first detection frame
constexpr double kTailSec = 0.30; // ... and after the last active frame
} // namespace

struct Channel::Dsp
{
    Dsp(double fs, double offset, double bw)
        : chan(fs, offset, bw), am(chan.outputRate())
    {
        int n = kSpgFft;
        fft.init(n);
        win.resize(kSpgFft);
        for (int i = 0; i < kSpgFft; ++i)
            win[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (kSpgFft - 1));
        acc.assign(kSpgFft, 0.0);
        ffPerRow = std::max(1, (int)std::lround(chan.outputRate() / kSpgHop / kSpgRowsPerSec));
    }

    Channelizer chan;
    AmDemod am;
    std::vector<cf> iq;          // scratch
    std::vector<float> audio;    // 8 kHz, modulation-normalised
    std::vector<float> carrier;  // carrier amplitude per audio sample

    JFFT fft;
    std::vector<double> win;
    std::vector<cf> hist;        // channel IQ not yet consumed by the spectrogram
    std::vector<JFFT::cpx_type> buf;
    std::vector<double> acc;
    int nAcc = 0, ffPerRow = 1;
    std::vector<std::vector<float>> rows; // dB, fft-shifted
};

Channel::Channel(int id, const Config& cfg, RecordingStore& store, double fs,
                 double centerHz, double freqHz, uint64_t firstSample, int64_t startMs)
    : id_(id), cfg_(cfg), store_(store), fs_(fs), centerHz_(centerHz), freqHz_(freqHz),
      firstSample_(firstSample), startMs_(startMs), lastActive_(firstSample)
{
    dsp_ = std::make_unique<Dsp>(fs, freqHz - centerHz, cfg.channelBwHz);
    nextSample_ = firstSample;
    thread_ = std::thread([this] { worker(); });
}

Channel::~Channel()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        closing_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Channel::push(IqBlockPtr b)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (closing_) return;
        queue_.push_back(std::move(b));
    }
    cv_.notify_one();
}

size_t Channel::queued()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

void Channel::noteActive(uint64_t sample, double relDb, double levelDb)
{
    relDb_.store(relDb);
    if (relDb > peakDb_) peakDb_ = relDb;
    if (firstActive_ == 0) firstActive_ = sample;
    lastActive_.store(sample);
    levelSum_ += levelDb;
    ++levelN_;
}

void Channel::close(bool splitByMaxLength)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        closing_ = true;
        split_ = splitByMaxLength;
    }
    cv_.notify_all();
}

void Channel::worker()
{
    for (;;)
    {
        IqBlockPtr b;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) break; // closing and drained
            b = std::move(queue_.front());
            queue_.pop_front();
        }
        process(*b);
    }
    try
    {
        finish();
    }
    catch (const std::exception& e)
    {
        logWrite("[chan %d] finish failed: %s", id_, e.what());
    }
    finished_.store(true);
}

void Channel::process(const IqBlock& b)
{
    uint64_t end = b.firstSample + (uint64_t)b.count();
    if (end <= nextSample_) return; // already seen (pre-roll overlap)
    int skip = 0;
    if (b.firstSample < nextSample_) skip = (int)(nextSample_ - b.firstSample);
    nextSample_ = end;

    Dsp& d = *dsp_;
    d.iq.clear();
    d.chan.process(b.iq.data() + 2 * skip, b.count() - skip, d.iq);
    size_t a0 = d.audio.size();
    d.am.process(d.iq, d.audio, d.carrier);
    spectrogramFeed(d.iq);

    if (d.audio.size() > a0)
    {
        double e = 0;
        for (size_t i = a0; i < d.audio.size(); ++i) e += (double)d.audio[i] * d.audio[i];
        audioLevel_.store((float)std::sqrt(e / (double)(d.audio.size() - a0)));
    }
}

void Channel::spectrogramFeed(const std::vector<cf>& iq)
{
    Dsp& d = *dsp_;
    d.hist.insert(d.hist.end(), iq.begin(), iq.end());
    size_t pos = 0;
    d.buf.resize(kSpgFft);
    while (d.hist.size() - pos >= (size_t)kSpgFft)
    {
        for (int i = 0; i < kSpgFft; ++i)
        {
            const cf& s = d.hist[pos + i];
            d.buf[i] = JFFT::cpx_type(s.real() * d.win[i], s.imag() * d.win[i]);
        }
        d.fft.fft(d.buf.data(), kSpgFft, JFFT::FORWARD);
        for (int i = 0; i < kSpgFft; ++i)
            d.acc[i] += std::norm(d.buf[(i + kSpgFft / 2) % kSpgFft]);
        if (++d.nAcc >= d.ffPerRow)
        {
            std::vector<float> row(kSpgFft);
            for (int i = 0; i < kSpgFft; ++i)
                row[i] = (float)(10.0 * std::log10(d.acc[i] / d.nAcc + 1e-20));
            d.rows.push_back(std::move(row));
            std::fill(d.acc.begin(), d.acc.end(), 0.0);
            d.nAcc = 0;
        }
        pos += kSpgHop;
    }
    d.hist.erase(d.hist.begin(), d.hist.begin() + (long)pos);
}

void Channel::finish()
{
    Dsp& d = *dsp_;
    const double ar = AmDemod::kAudioRate;

    // Keep [first detection - lead, last activity + tail].
    uint64_t fa = firstActive_ ? firstActive_ : firstSample_;
    double t0 = std::max(0.0, (double)(fa - firstSample_) / fs_ - kLeadSec);
    double t1 = (double)(lastActive_.load() - firstSample_) / fs_ + kTailSec;
    size_t a0 = std::min(d.audio.size(), (size_t)(t0 * ar));
    size_t a1 = std::min(d.audio.size(), (size_t)(t1 * ar));
    if (a1 <= a0)
    {
        logWrite("[chan %d] %.4f MHz: no audio", id_, freqHz_ / 1e6);
        return;
    }
    std::vector<float> audio(d.audio.begin() + (long)a0, d.audio.begin() + (long)a1);
    std::vector<float> carrier(d.carrier.begin() + (long)a0, d.carrier.begin() + (long)a1);
    int durationMs = (int)(audio.size() * 1000.0 / ar);
    // The minimum length applies to the detected part, not the lead/tail.
    int activeMs = (int)((double)(lastActive_.load() - fa) * 1000.0 / fs_);
    if (activeMs < cfg_.minDurationMs)
    {
        logWrite("[chan %d] %.4f MHz: %d ms burst discarded", id_, freqHz_ / 1e6, activeMs);
        return;
    }

    VoiceVerdict v = classifyVoice(audio, carrier, cfg_.voiceThreshold);
    if (!v.voice && !cfg_.keepOther)
    {
        logWrite("[chan %d] %.4f MHz: not voice (%s), not kept", id_, freqHz_ / 1e6, v.reason.c_str());
        return;
    }

    // Squelch: AM noise is normalised by a near-zero carrier when the
    // transmitter is off, so fade the audio out wherever the carrier drops.
    std::vector<float> cs = carrier;
    std::nth_element(cs.begin(), cs.begin() + cs.size() * 7 / 10, cs.end());
    const float cref = std::max(cs[cs.size() * 7 / 10], 1e-9f);
    float gate = 0.f;
    for (size_t i = 0; i < audio.size(); ++i)
    {
        float target = std::clamp((carrier[i] / cref - 0.3f) / 0.3f, 0.f, 1.f);
        gate += (target - gate) * (target > gate ? 0.02f : 0.005f);
        audio[i] *= gate;
    }

    RecordingInfo r;
    r.startMs = startMs_ + (int64_t)(t0 * 1000.0);
    r.durationMs = durationMs;
    r.freqHz = freqHz_;
    r.peakDb = peakDb_;
    r.levelDbfs = levelN_ ? levelSum_ / levelN_ : 0.0;
    r.voice = v.voice;
    r.score = v.score;
    r.voicedFrac = v.voicedFrac;
    r.levelSpreadDb = v.levelSpreadDb;
    r.modDepth = v.modDepth;
    r.reason = v.reason;
    if (split_) r.reason += "; split at max length";
    r.id = store_.makeId(r.startMs, freqHz_);

    std::string wavPath = store_.path(r.id, ".wav");
    std::error_code ec;
    fs::create_directories(fs::path(wavPath).parent_path(), ec);

    // Audio: modulation index -> PCM, soft-limited, 15 ms fades at both ends.
    std::vector<int16_t> pcm(audio.size());
    const size_t fade = (size_t)(0.015 * ar);
    for (size_t i = 0; i < audio.size(); ++i)
    {
        double g = 1.0;
        if (i < fade) g = (double)i / fade;
        if (audio.size() - i < fade) g = std::min(g, (double)(audio.size() - i) / fade);
        double s = std::tanh(1.1 * audio[i]) * 0.89 * g;
        pcm[i] = (int16_t)std::lround(s * 32767.0);
    }
    if (!writeWav16(wavPath, pcm, (int)ar))
    {
        logWrite("[chan %d] cannot write %s", id_, wavPath.c_str());
        return;
    }

    // Spectrogram rows for the same time span.
    const double rps = d.chan.outputRate() / kSpgHop / d.ffPerRow;
    size_t r0 = std::min(d.rows.size(), (size_t)(t0 * rps));
    size_t r1 = std::min(d.rows.size(), (size_t)(t1 * rps) + 1);
    if (r1 > r0)
    {
        std::vector<float> all;
        for (size_t i = r0; i < r1; ++i) all.insert(all.end(), d.rows[i].begin(), d.rows[i].end());
        // Floor from the noise inside the channel passband (the stopband
        // outside the channel filter would drag it far down).
        const double bin = d.chan.outputRate() / kSpgFft;
        const int half = std::max(1, (int)(cfg_.channelBwHz * 0.5 / bin));
        std::vector<float> s;
        for (size_t i = 0; i < all.size(); ++i)
        {
            int c = (int)(i % kSpgFft) - kSpgFft / 2;
            if (std::abs(c) <= half) s.push_back(all[i]);
        }
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        float lo = s[s.size() / 2] - 8.0f;
        float hi = *std::max_element(all.begin(), all.end());
        if (hi - lo < 30.0f) hi = lo + 30.0f;

        SpgHeader h{};
        std::memcpy(h.magic, "SPG1", 4);
        h.cols = kSpgFft;
        h.rows = (uint32_t)(r1 - r0);
        h.rowsPerSec = (float)rps;
        h.binHz = (float)(d.chan.outputRate() / kSpgFft);
        h.dbMin = lo;
        h.dbMax = hi;
        h.centerHz = freqHz_;
        std::vector<uint8_t> cells(all.size());
        for (size_t i = 0; i < all.size(); ++i)
            cells[i] = (uint8_t)std::clamp((int)std::lround((all[i] - lo) * 255.0 / (hi - lo)), 0, 255);
        std::string sp = store_.path(r.id, ".spg");
        if (std::FILE* f = std::fopen(sp.c_str(), "wb"))
        {
            std::fwrite(&h, sizeof(h), 1, f);
            std::fwrite(cells.data(), 1, cells.size(), f);
            std::fclose(f);
            r.hasSpg = true;
        }
    }

    if (r.voice && cfg_.mp3Auto)
    {
        std::string err;
        if (encodeMp3(pcm, (int)ar, cfg_.mp3Bitrate, store_.path(r.id, ".mp3"), err))
            r.hasMp3 = true;
        else
            logWrite("[chan %d] mp3: %s", id_, err.c_str());
    }

    store_.add(r);
    logWrite("[rec] %s %.4f MHz %.1f s %s -> %s", r.voice ? "VOICE" : "other",
             freqHz_ / 1e6, durationMs / 1000.0, v.reason.c_str(), r.id.c_str());
}
