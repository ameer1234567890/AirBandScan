// Satellite receiver: holds the SDR on one frequency and decodes
//   * SSTV (ISS 145.800 MHz, CubeSats): NFM demodulator -> SstvDecoder, and
//   * Meteor-M LRPT (137.900 / 137.100 MHz): OQPSK/QPSK -> LrptDecoder.
// It runs instead of the scanner (one SDR). Finished pictures go to the
// ImageStore: every SSTV image, and each LRPT pass once its signal is gone.
#pragma once

#include "engine/channel.h"
#include "engine/config.h"
#include "sat/image_store.h"
#include "sat/lrpt_chain.h"
#include "sat/sstv.h"
#include "sdr/sdr_source.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SatReceiver
{
public:
    struct Status
    {
        bool running = false;
        std::string error, mode, variant, device;
        double freqHz = 0, fs = 0, chRate = 0;
        float levelDb = -120.f;
        int64_t startedMs = 0;
        uint64_t drops = 0;
        bool fileDone = false;          // WAV input finished
        // SSTV
        bool sstvBusy = false;
        std::string sstvMode;
        int sstvRows = 0, sstvTotal = 0;
        float tone = 0;                 // audio frequency (Hz) for tuning
        // LRPT
        bool carrier = false, viterbi = false, frameSync = false, passActive = false;
        double lockQ = 0, offsetHz = 0, ber = 0;
        uint64_t frames = 0, framesBad = 0, packets = 0;
        int rows = 0;
        std::vector<int> channels;
        int64_t passStartMs = 0;
        std::vector<cf> constellation;
        // both
        int imagesSaved = 0;
        uint64_t liveVersion = 0;       // changes when the live picture does
    };

    explicit SatReceiver(ImageStore& images) : images_(images) {}
    ~SatReceiver() { stop(); }

    bool start(const Config& cfg, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    Status status();
    // PNG of the picture being received (empty if none).
    std::string livePng();

private:
    void onSamples(const float* iq, int n);
    void dspLoop();
    void processBlock(const IqBlock& b);
    void saveSstv(const SstvDecoder::Result& r);
    void finishPass(const char* why);
    int64_t nowMs() const;

    ImageStore& images_;
    Config cfg_;
    std::unique_ptr<SdrSource> src_;
    std::thread thread_;
    std::atomic<bool> running_{false}, quit_{false};

    std::mutex qMtx_;
    std::condition_variable qCv_;
    std::deque<std::shared_ptr<IqBlock>> queue_;
    size_t queued_ = 0;
    std::atomic<uint64_t> drops_{0};

    // DSP state (dspMtx_ guards it against status / live readers)
    mutable std::mutex dspMtx_;
    double fs_ = 0, offsetHz_ = 0;
    bool lrpt_ = false;
    std::unique_ptr<Channelizer> chan_;
    std::vector<cf> chBuf_;
    float levelDb_ = -120.f;
    // SSTV
    std::unique_ptr<SstvDecoder> sstv_;
    cf fmLast_{1.f, 0.f};
    Biquad lp1_, lp2_;
    int audDecim_ = 1, audPhase_ = 0;
    std::vector<float> audio_;
    // LRPT
    std::unique_ptr<LrptChain> chain_;
    uint64_t lastFrames_ = 0;
    int64_t lastFrameMs_ = 0, passStartMs_ = 0;
    bool passActive_ = false;

    int imagesSaved_ = 0;
    uint64_t liveVersion_ = 0;
    std::string liveCache_;
    uint64_t liveCacheVersion_ = ~0ull;
    int64_t liveCacheMs_ = 0;
    std::string error_;
    bool fileDone_ = false;
    int64_t startedMs_ = 0;
};
