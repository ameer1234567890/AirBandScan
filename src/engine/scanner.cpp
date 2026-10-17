#include "engine/scanner.h"

#include "sdr/rtl_sdr_source.h"
#include "sdr/rtl_tcp_source.h"
#include "sdr/wav_file_source.h"
#if defined(HAS_AIRSPY)
#include "sdr/airspy_source.h"
#endif
#if defined(HAS_AIRSPYHF)
#include "sdr/airspyhf_source.h"
#endif
#if defined(HAS_PLUTO)
#include "sdr/pluto_source.h"
#endif
#if defined(HAS_SDRPLAY)
#include "sdr/sdrplay_source.h"
#endif
#if defined(HAS_HACKRF)
#include "sdr/hackrf_source.h"
#endif
#include "util/log.h"
#if defined(AIRBANDSCAN_SIM)
#include "sim_source.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr double kFrameSec = 0.020;     // detection frame (rtl-sdr-scanner: 50 fps)
constexpr double kTimeAvgSec = 0.160;   // time averaging (GROUPING_Y)
constexpr double kUsable = 0.80;        // usable share of the sample rate
constexpr size_t kMaxHistRows = 3600;
constexpr double kMaxQueueSec = 1.0;

std::unique_ptr<SdrSource> makeSource(const std::string& type)
{
    if (type == "wav") return std::make_unique<WavFileSource>();
#if defined(AIRBANDSCAN_SIM)
    if (type == "sim") return std::make_unique<SimSource>();
#endif
#if defined(HAS_AIRSPY)
    if (type == "airspy") return std::make_unique<AirspySource>();
#endif
#if defined(HAS_AIRSPYHF)
    if (type == "airspyhf") return std::make_unique<AirspyHfSource>();
#endif
#if defined(HAS_PLUTO)
    if (type == "pluto") return std::make_unique<PlutoSource>();
#endif
#if defined(HAS_SDRPLAY)
    if (type == "sdrplay") return std::make_unique<SdrplaySource>();
#endif
#if defined(HAS_HACKRF)
    if (type == "hackrf") return std::make_unique<HackRfSource>();
#endif
    if (type == "rtlsdr") return std::make_unique<RtlSdrSource>();
    if (type == "rtltcp") return std::make_unique<RtlTcpSource>();
    return nullptr;
}

uint8_t dbToCell(float db)
{
    return (uint8_t)std::clamp((int)std::lround((db + 130.0f) * 2.0f), 0, 255);
}

} // namespace

int64_t Scanner::nowMs() const
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<SdrDeviceInfo> Scanner::listDevices(const std::string& type, const std::string& address)
{
    auto s = makeSource(type);
    if (!s) return {};
    if (type == "rtltcp")
        s->setAddress(address);
    return s->listDevices();
}

std::vector<Scanner::Window> Scanner::splitWindows(const std::vector<FreqRange>& ranges, double fs)
{
    std::vector<Window> out;
    double usable = fs * kUsable;
    for (auto& r : ranges)
    {
        double w = r.hi - r.lo;
        int n = std::max(1, (int)std::ceil(w / usable - 1e-9));
        double step = w / n;
        for (int i = 0; i < n; ++i)
        {
            double lo = r.lo + i * step, hi = lo + step;
            out.push_back({lo, hi, (lo + hi) * 0.5});
        }
    }
    return out;
}

bool Scanner::ignored(double f) const
{
    for (auto& r : cfg_.ignored)
        if (r.contains(f)) return true;
    return false;
}

bool Scanner::start(const Config& cfg, std::string& err)
{
    stop();
    cfg_ = cfg;
    src_ = makeSource(cfg.device);
    if (!src_)
    {
        err = "device type '" + cfg.device + "' is not supported by this build";
        std::lock_guard<std::mutex> lk(snapMtx_);
        status_ = Status{};
        status_.error = err;
        return false;
    }
    if (auto* w = dynamic_cast<WavFileSource*>(src_.get()))
    {
        w->setPath(cfg.wavPath);
        w->setLoop(true);
        w->setCenterFreq(cfg.wavCenterHz);
    }
    else
    {
        src_->setAddress(cfg.device == "rtltcp" ? cfg.rtlTcpAddress : cfg.plutoUri);
        src_->setSampleRate(cfg.sampleRate);
        src_->setGain(cfg.gainDb);
        src_->setPpm(cfg.ppm);
        src_->setBiasTee(cfg.biasTee);
        std::vector<Window> wins = splitWindows(cfg.ranges, cfg.sampleRate);
        src_->setCenterFreq(wins.empty() ? 120e6 : wins.front().center);
    }

    {
        std::lock_guard<std::mutex> lk(qMtx_);
        queue_.clear();
        queuedSamples_ = 0;
    }
    drops_ = 0;
    quit_ = false;
    if (!src_->start(cfg.deviceIndex, [this](const float* iq, int n) { onSamples(iq, n); }, err))
    {
        logWrite("[scan] start failed: %s", err.c_str());
        src_.reset();
        std::lock_guard<std::mutex> lk(snapMtx_);
        status_ = Status{};
        status_.error = err;
        return false;
    }

    fs_ = src_->sampleRate();
    if (cfg.device == "wav")
    {
        // A recording can't be retuned: scan the configured ranges that fall
        // inside the file's bandwidth, or the whole file if none do.
        double c = cfg.wavCenterHz, half = fs_ * kUsable / 2;
        std::vector<FreqRange> clipped;
        for (auto& r : cfg.ranges)
        {
            double lo = std::max(r.lo, c - half), hi = std::min(r.hi, c + half);
            if (hi > lo) clipped.push_back({lo, hi});
        }
        if (clipped.empty()) clipped.push_back({c - half, c + half});
        double lo = clipped.front().lo, hi = clipped.front().hi;
        for (auto& r : clipped) { lo = std::min(lo, r.lo); hi = std::max(hi, r.hi); }
        windows_ = {{lo, hi, c}};
    }
    else
        windows_ = splitWindows(cfg.ranges, fs_);

    // FFT sized for <= ~1 kHz bins (rtl-sdr-scanner uses a fixed max step).
    N_ = 1024;
    while (N_ < 16384 && fs_ / N_ > 1000.0) N_ *= 2;
    binHz_ = fs_ / N_;
    K_ = std::max(1, (int)std::lround(fs_ * kFrameSec / N_));
    T_ = std::max(2, (int)std::lround(kTimeAvgSec / (K_ * N_ / fs_)));
    g_ = std::max(2, (int)std::ceil(cfg.channelBwHz * 0.5 / binHz_));
    int n = N_;
    fft_.init(n);
    window_.resize(N_);
    double sw = 0;
    for (int i = 0; i < N_; ++i)
    {
        double x = 2.0 * M_PI * i / (N_ - 1);
        window_[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
        sw += window_[i];
    }
    winNorm_ = 1.0 / (sw * sw); // full-scale tone -> 0 dB
    fftBuf_.assign(N_, 0);
    welch_.assign(N_, 0.0);
    avgSum_.assign(N_, 0.0);
    avgRing_.clear();
    fftFill_ = welchN_ = 0;
    ws_.assign(windows_.size(), WinState{});
    ring_.assign((size_t)(fs_ * (cfg.prerollMs / 1000.0 + 0.1)) * 2 + 2, 0.f);
    ringPos_ = ringCount_ = 0;
    active_.clear();
    closing_.clear();
    sample_ = 0;
    frames_ = 0;

    {
        std::lock_guard<std::mutex> lk(snapMtx_);
        status_ = Status{};
        status_.running = true;
        status_.device = cfg.device;
        status_.fs = fs_;
        status_.binHz = binHz_;
        status_.windows = windows_;
        status_.windowReady.assign(windows_.size(), false);
        status_.startedMs = nowMs();
        spec_ = Spectrum{};
        hist_.assign(windows_.size(), History{});
    }

    logWrite("[scan] %s @ %.3f MS/s, FFT %d (%.0f Hz bins), %d FFT/frame, %d window(s)",
             cfg.device.c_str(), fs_ / 1e6, N_, binHz_, K_, (int)windows_.size());
    for (size_t i = 0; i < windows_.size(); ++i)
        logWrite("[scan]   window %d: %.4f - %.4f MHz", (int)i, windows_[i].lo / 1e6, windows_[i].hi / 1e6);

    tune(0); // also computes the detection bin range (WAV: centre is fixed)
    running_ = true;
    fpsT0_ = nowMs();
    fpsF0_ = 0;
    thread_ = std::thread([this] { dspLoop(); });
    return true;
}

void Scanner::stop()
{
    quit_ = true;
    qCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (src_)
    {
        src_->stop();
        src_.reset();
    }
    // Close anything still recording so it is written out.
    for (auto& a : active_) a.ch->close(false);
    for (auto& a : active_) closing_.push_back(a.ch);
    active_.clear();
    closing_.clear(); // ~Channel joins the worker after it finished writing
    running_ = false;
    std::lock_guard<std::mutex> lk(snapMtx_);
    status_.running = false;
    status_.live.clear();
}

void Scanner::onSamples(const float* iq, int n)
{
    auto b = std::make_shared<IqBlock>();
    b->iq.assign(iq, iq + 2 * n);
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        if (queuedSamples_ > (size_t)(fs_ > 0 ? fs_ * kMaxQueueSec : 4e6))
        {
            drops_++;
            return;
        }
        queuedSamples_ += (size_t)n;
        queue_.push_back(std::move(b));
    }
    qCv_.notify_one();
}

void Scanner::tune(int w)
{
    cur_ = w;
    centerHz_ = windows_[w].center;
    src_->setCenterFreq(centerHz_);
    {
        // Samples already queued belong to the old frequency.
        std::lock_guard<std::mutex> lk(qMtx_);
        queue_.clear();
        queuedSamples_ = 0;
    }
    skip_ = (uint64_t)(fs_ * src_->retuneSettleSec());
    fftFill_ = welchN_ = 0;
    std::fill(welch_.begin(), welch_.end(), 0.0);
    avgRing_.clear();
    std::fill(avgSum_.begin(), avgSum_.end(), 0.0);
    ringCount_ = 0;
    dwellStart_ = sample_ + skip_;

    // Bins inside this window (and inside the usable part of the spectrum).
    double half = fs_ * kUsable / 2;
    double lo = std::max(windows_[w].lo, centerHz_ - half);
    double hi = std::min(windows_[w].hi, centerHz_ + half);
    iLo_ = std::clamp((int)std::floor((lo - centerHz_) / binHz_) + N_ / 2, 0, N_ - 1);
    iHi_ = std::clamp((int)std::ceil((hi - centerHz_) / binHz_) + N_ / 2, 0, N_ - 1);
}

void Scanner::dspLoop()
{
    while (!quit_)
    {
        std::shared_ptr<IqBlock> b;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            if (queue_.empty())
            {
                if (src_ && !src_->running())
                {
                    std::lock_guard<std::mutex> sl(snapMtx_);
                    if (status_.error.empty()) status_.error = "SDR stopped delivering samples";
                }
                continue;
            }
            b = std::move(queue_.front());
            queue_.pop_front();
            queuedSamples_ -= (size_t)b->count();
        }

        int n = b->count();
        int off = 0;
        if (skip_ > 0)
        {
            uint64_t s = std::min<uint64_t>(skip_, (uint64_t)n);
            skip_ -= s;
            sample_ += s;
            off = (int)s;
            if (off >= n) continue;
            b->iq.erase(b->iq.begin(), b->iq.begin() + 2 * off);
            n -= off;
        }
        b->firstSample = sample_;
        sample_ += (uint64_t)n; // block end

        // Pre-roll ring.
        const size_t cap = ring_.size() / 2;
        for (int i = 0; i < n; ++i)
        {
            ring_[2 * ringPos_] = b->iq[2 * i];
            ring_[2 * ringPos_ + 1] = b->iq[2 * i + 1];
            ringPos_ = (ringPos_ + 1) % cap;
        }
        ringCount_ = std::min(cap, ringCount_ + (size_t)n);
        ringHeadSample_ = sample_;

        IqBlockPtr shared = b;
        for (auto& a : active_) a.ch->push(shared);

        // Welch FFT accumulation; a frame every K_ FFTs.
        for (int i = 0; i < n; ++i)
        {
            fftBuf_[fftFill_] = JFFT::cpx_type(b->iq[2 * i] * window_[fftFill_],
                                               b->iq[2 * i + 1] * window_[fftFill_]);
            if (++fftFill_ < N_) continue;
            fftFill_ = 0;
            fft_.fft(fftBuf_.data(), N_, JFFT::FORWARD);
            for (int k = 0; k < N_; ++k) welch_[k] += std::norm(fftBuf_[k]);
            if (++welchN_ >= K_)
            {
                frameEnd_ = b->firstSample + (uint64_t)i + 1;
                frame();
                welchN_ = 0;
                std::fill(welch_.begin(), welch_.end(), 0.0);
                if (skip_ > 0) break; // frame() retuned: rest of block is stale
            }
        }

        // Reap channels that finished writing.
        closing_.erase(std::remove_if(closing_.begin(), closing_.end(),
                                      [](const std::shared_ptr<Channel>& c) { return c->finished(); }),
                       closing_.end());
    }
}

void Scanner::pool(const std::vector<float>& src, std::vector<float>& dst) const
{
    int count = iHi_ - iLo_ + 1;
    int f = std::max(1, (count + kMaxCols - 1) / kMaxCols);
    int cols = (count + f - 1) / f;
    dst.assign(cols, -200.f);
    for (int i = 0; i < count; ++i)
        dst[i / f] = std::max(dst[i / f], src[iLo_ + i]);
}

void Scanner::frame()
{
    ++frames_;
    // Power spectrum in dB, fft-shifted.
    std::vector<float> db(N_);
    const double scale = winNorm_ / K_;
    for (int i = 0; i < N_; ++i)
        db[i] = (float)(10.0 * std::log10(welch_[(i + N_ / 2) % N_] * scale + 1e-20));
    // The DC blocker leaves a hole at the centre bin: patch with a neighbour.
    for (int k = -1; k <= 1; ++k) db[N_ / 2 + k] = db[N_ / 2 - 3];

    // Time average over T_ frames.
    avgRing_.push_back(db);
    for (int i = 0; i < N_; ++i) avgSum_[i] += db[i];
    if ((int)avgRing_.size() > T_)
    {
        const auto& old = avgRing_.front();
        for (int i = 0; i < N_; ++i) avgSum_[i] -= old[i];
        avgRing_.pop_front();
    }
    if ((int)avgRing_.size() < T_) return;

    // Light frequency smoothing (3 bins).
    std::vector<float> sm(N_);
    for (int i = 0; i < N_; ++i)
    {
        int a = std::max(0, i - 1), b = std::min(N_ - 1, i + 1);
        double s = 0;
        for (int k = a; k <= b; ++k) s += avgSum_[k];
        sm[i] = (float)(s / ((b - a + 1) * (double)T_));
    }

    WinState& W = ws_[cur_];
    const uint64_t frameEnd = frameEnd_;
    if (!W.ready)
    {
        if (W.learnSum.empty()) W.learnSum.assign(N_, 0.0);
        for (int i = 0; i < N_; ++i) W.learnSum[i] += sm[i];
        W.learnFrames++;
        double learned = W.learnFrames * K_ * N_ / fs_;
        if (learned * 1000.0 >= cfg_.noiseLearnMs)
        {
            W.noise.resize(N_);
            for (int i = 0; i < N_; ++i) W.noise[i] = (float)(W.learnSum[i] / W.learnFrames);
            W.learnSum.clear();
            W.ready = true;
            logWrite("[scan] noise learned for %.3f-%.3f MHz", windows_[cur_].lo / 1e6, windows_[cur_].hi / 1e6);
            std::lock_guard<std::mutex> lk(snapMtx_);
            status_.windowReady[cur_] = true;
        }
    }
    else
        detect(W, sm, frameEnd);

    // Long-term waterfall: max-hold, one row per second of listening on this
    // window (accumulated across hops, so short dwells still produce rows).
    if (W.histMax.empty()) { W.histMax.assign(N_, -200.f); W.histN = 0; }
    for (int i = iLo_; i <= iHi_; ++i) W.histMax[i] = std::max(W.histMax[i], sm[i]);
    W.histN++;
    bool rowDue = W.histN * (double)K_ * N_ / fs_ >= 1.0;

    // Publish snapshots for the web API.
    {
        std::lock_guard<std::mutex> lk(snapMtx_);
        spec_.valid = true;
        spec_.windowIdx = cur_;
        spec_.lo = centerHz_ + (iLo_ - N_ / 2) * binHz_;
        spec_.hi = centerHz_ + (iHi_ - N_ / 2) * binHz_;
        spec_.centerHz = centerHz_;
        spec_.startLevel = cfg_.startLevelDb;
        spec_.stopLevel = cfg_.stopLevelDb;
        pool(sm, spec_.db);
        if (W.ready) pool(W.noise, spec_.noise);
        else spec_.noise.clear();

        if (rowDue)
        {
            std::vector<float> pooled;
            pool(W.histMax, pooled);
            History& H = hist_[cur_];
            H.lo = spec_.lo;
            H.hi = spec_.hi;
            if (H.cols != (int)pooled.size()) { H.cols = (int)pooled.size(); H.cells.clear(); H.timeMs.clear(); }
            H.timeMs.push_back(nowMs());
            for (float v : pooled) H.cells.push_back(dbToCell(v));
            if (H.timeMs.size() > kMaxHistRows)
            {
                H.timeMs.erase(H.timeMs.begin());
                H.cells.erase(H.cells.begin(), H.cells.begin() + H.cols);
            }
        }

        status_.centerHz = centerHz_;
        status_.windowIdx = cur_;
        status_.drops = drops_.load();
        status_.live.clear();
        for (auto& a : active_)
            status_.live.push_back({a.ch->id(), a.ch->freqHz(), a.ch->startMs(), a.ch->relDb(), a.ch->audioLevel()});
        int64_t t = nowMs();
        if (t - fpsT0_ >= 2000)
        {
            status_.framesPerSec = (frames_ - fpsF0_) * 1000.0 / (t - fpsT0_);
            fpsT0_ = t;
            fpsF0_ = frames_;
        }
    }
    if (rowDue)
    {
        std::fill(W.histMax.begin(), W.histMax.end(), -200.f);
        W.histN = 0;
    }

    // Hop to the next window when idle and the dwell time is over.
    if (windows_.size() > 1 && W.ready && active_.empty() &&
        frameEnd > dwellStart_ && (double)(frameEnd - dwellStart_) * 1000.0 / fs_ >= cfg_.dwellMs)
    {
        tune((cur_ + 1) % (int)windows_.size());
    }
}

void Scanner::detect(WinState& W, const std::vector<float>& sm, uint64_t frameEnd)
{
    std::vector<float> rel(N_);
    for (int i = 0; i < N_; ++i) rel[i] = sm[i] - W.noise[i];

    auto peakAround = [&](int c, int& at) {
        float best = -1e9f;
        for (int k = std::max(iLo_, c - g_); k <= std::min(iHi_, c + g_); ++k)
            if (rel[k] > best) { best = rel[k]; at = k; }
        return best;
    };

    // ---- Update open transmissions ----
    const uint64_t timeout = (uint64_t)(cfg_.timeoutMs / 1000.0 * fs_);
    const uint64_t maxLen = (uint64_t)(cfg_.maxDurationS * fs_);
    for (size_t j = 0; j < active_.size();)
    {
        Active& a = active_[j];
        int at = a.bin;
        float lvl = peakAround(a.bin, at);
        if (lvl >= cfg_.stopLevelDb) a.ch->noteActive(frameEnd, lvl, sm[at]);
        bool silent = frameEnd - a.ch->lastActiveSample() > timeout;
        bool tooLong = frameEnd - a.ch->firstSample() > maxLen;
        if (silent || tooLong)
        {
            a.ch->close(tooLong && !silent);
            closing_.push_back(a.ch);
            active_.erase(active_.begin() + (long)j);
            continue;
        }
        ++j;
    }

    // ---- Look for new transmissions ----
    std::vector<int> cand;
    for (int i = iLo_; i <= iHi_; ++i)
        if (rel[i] >= cfg_.startLevelDb) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return rel[a] > rel[b]; });

    auto nearActive = [&](int bin) {
        for (auto& a : active_)
            if (std::abs(a.bin - bin) <= g_) return true;
        return false;
    };

    for (int i : cand)
    {
        if (nearActive(i)) continue;
        // Climb to the local maximum so the edge of a wide signal can't open
        // a second channel next to the real carrier.
        int p = i;
        for (int it = 0; it < 8; ++it)
        {
            int q = p;
            peakAround(p, q);
            if (q == p) break;
            p = q;
        }
        if (nearActive(p)) continue;

        // Power-weighted centroid over +-1 bin, then snap to the channel raster.
        double sw = 0, sf = 0;
        for (int k = std::max(0, p - 1); k <= std::min(N_ - 1, p + 1); ++k)
        {
            double w = std::pow(10.0, rel[k] / 10.0);
            sw += w;
            sf += w * k;
        }
        double idx = sw > 0 ? sf / sw : p;
        double f = centerHz_ + (idx - N_ / 2) * binHz_;
        if (cfg_.channelStepHz > 0) f = std::round(f / cfg_.channelStepHz) * cfg_.channelStepHz;
        if (f < windows_[cur_].lo || f > windows_[cur_].hi || ignored(f)) continue;

        bool dup = false;
        for (auto& a : active_)
        {
            double df = std::fabs(a.ch->freqHz() - f);
            // Same channel, or splatter from a much stronger neighbour.
            if (df < std::max(2000.0, cfg_.channelStepHz * 0.5) ||
                (df < 13000.0 && rel[p] < a.ch->relDb() - 15.0))
                dup = true;
        }
        if (dup) continue;

        // Seed with the pre-roll so the start of the call is not lost.
        size_t pre = std::min(ringCount_, (size_t)(fs_ * cfg_.prerollMs / 1000.0));
        const size_t cap = ring_.size() / 2;
        auto blk = std::make_shared<IqBlock>();
        blk->iq.resize(pre * 2);
        size_t startPos = (ringPos_ + cap - pre) % cap;
        for (size_t k = 0; k < pre; ++k)
        {
            size_t r = (startPos + k) % cap;
            blk->iq[2 * k] = ring_[2 * r];
            blk->iq[2 * k + 1] = ring_[2 * r + 1];
        }
        // The ring always reaches the end of the current block.
        blk->firstSample = ringHeadSample_ - pre;
        int64_t startMs = nowMs() - (int64_t)((ringHeadSample_ - blk->firstSample) * 1000.0 / fs_);

        auto ch = std::make_shared<Channel>(nextId_++, cfg_, store_, fs_, centerHz_, f,
                                            blk->firstSample, startMs);
        ch->push(blk);
        ch->noteActive(frameEnd, rel[p], sm[p]);
        active_.push_back({ch, (int)std::lround((f - centerHz_) / binHz_) + N_ / 2});
        {
            std::lock_guard<std::mutex> lk(snapMtx_);
            status_.detections++;
        }
        logWrite("[scan] start %.4f MHz (+%.1f dB)", f / 1e6, rel[p]);
    }

    // ---- Adapt the noise floor away from open transmissions ----
    for (int i = iLo_; i <= iHi_; ++i)
    {
        bool busy = false;
        for (auto& a : active_)
            if (std::abs(a.bin - i) <= g_ + 2) { busy = true; break; }
        if (busy || rel[i] >= cfg_.startLevelDb) continue;
        float alpha = rel[i] < 0 ? 0.02f : 0.002f;
        W.noise[i] += alpha * (sm[i] - W.noise[i]);
    }
}

Scanner::Status Scanner::status()
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    Status s = status_;
    s.running = running_.load() && s.error.empty();
    return s;
}

Scanner::Spectrum Scanner::spectrum()
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    return spec_;
}

bool Scanner::history(int window, History& out)
{
    std::lock_guard<std::mutex> lk(snapMtx_);
    if (window < 0 || window >= (int)hist_.size()) return false;
    out = hist_[window];
    return true;
}
