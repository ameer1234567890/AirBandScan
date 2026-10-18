// One detected transmission being recorded.
//
// The scanner thread hands every wideband IQ block to each open Channel; the
// Channel's own worker thread mixes its frequency to baseband, demodulates
// AM or NFM to 8 kHz audio and builds a narrow-band spectrogram. When the scanner
// closes it (signal gone / too long) the worker trims trailing silence,
// classifies the audio as voice or not and writes WAV / MP3 / SPG / meta.
#pragma once

#include "engine/config.h"
#include "engine/recording_store.h"

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct IqBlock
{
    uint64_t firstSample = 0;  // absolute index of iq[0] in the sample stream
    std::vector<float> iq;     // interleaved I,Q
    int count() const { return (int)(iq.size() / 2); }
};
using IqBlockPtr = std::shared_ptr<const IqBlock>;

class Channel
{
public:
    Channel(int id, const Config& cfg, RecordingStore& store, double fs,
            double centerHz, double freqHz, uint64_t firstSample, int64_t startMs);
    ~Channel();

    void push(IqBlockPtr b);

    // Scanner bookkeeping (scanner thread only).
    void noteActive(uint64_t sample, double relDb, double levelDb);
    uint64_t lastActiveSample() const { return lastActive_.load(); }
    uint64_t firstSample() const { return firstSample_; }

    // Ask the worker to finish: it drains its queue, writes the files and
    // then reports finished().
    void close(bool splitByMaxLength);
    bool finished() const { return finished_.load(); }

    int    id() const { return id_; }
    double freqHz() const { return freqHz_; }
    int64_t startMs() const { return startMs_; }
    double relDb() const { return relDb_.load(); }
    double peakDb() const { return peakDb_; }
    float  audioLevel() const { return audioLevel_.load(); }
    size_t queued();

private:
    void worker();
    void process(const IqBlock& b);
    void spectrogramFeed(const std::vector<std::complex<float>>& iq);
    void finish();

    const int id_;
    const Config cfg_;
    RecordingStore& store_;
    const double fs_, centerHz_, freqHz_;
    const uint64_t firstSample_;
    const int64_t startMs_;

    std::atomic<uint64_t> lastActive_;
    uint64_t firstActive_ = 0;
    std::atomic<double> relDb_{0.0};
    double peakDb_ = 0.0;
    double levelSum_ = 0.0;
    int levelN_ = 0;
    std::atomic<float> audioLevel_{0.f};

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<IqBlockPtr> queue_;
    bool closing_ = false;
    bool split_ = false;
    std::atomic<bool> finished_{false};

    // DSP state (worker thread only)
    struct Dsp;
    std::unique_ptr<Dsp> dsp_;
    uint64_t nextSample_ = 0; // next absolute sample index expected
};
