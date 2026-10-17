// Airband scanner engine.
//
// Port of rtl-sdr-scanner-cpp's detection pipeline onto AirScope's
// lightweight SDR/DSP stack (no GNU Radio / SoapySDR / MQTT):
//
//   * configured ranges are split into windows the SDR can see at once and
//     hopped through (dwell time per window, stay while something is live);
//   * every 20 ms a Welch-averaged FFT frame is computed, averaged in time
//     and smoothed in frequency;
//   * each window learns its own noise floor, then keeps adapting it slowly;
//   * a bin rising start_level dB above noise opens a transmission, snapped
//     to the airband channel raster; it stays open while above stop_level
//     and closes after timeout_ms of silence (or max_duration_s);
//   * every open transmission becomes a Channel (AM demod + recorder) which
//     is seeded with a pre-roll from an IQ ring so the first syllable is kept.
#pragma once

#include "dsp/jfft.h"
#include "engine/channel.h"
#include "engine/config.h"
#include "engine/recording_store.h"
#include "sdr/sdr_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Scanner
{
public:
    struct Window
    {
        double lo, hi, center;
    };

    struct Live
    {
        int     id;
        double  freqHz;
        int64_t startMs;
        double  relDb;
        float   audio;
    };

    struct Status
    {
        bool running = false;
        std::string error;
        std::string device;
        double fs = 0, centerHz = 0, binHz = 0;
        int windowIdx = 0;
        std::vector<Window> windows;
        std::vector<bool> windowReady;
        std::vector<Live> live;
        uint64_t drops = 0;
        uint64_t detections = 0;
        double framesPerSec = 0;
        int64_t startedMs = 0;
    };

    struct Spectrum
    {
        bool valid = false;
        int windowIdx = 0;
        double lo = 0, hi = 0, centerHz = 0;
        double startLevel = 0, stopLevel = 0;
        std::vector<float> db, noise; // pooled to <= kMaxCols, lo..hi
    };

    struct History
    {
        double lo = 0, hi = 0;
        int cols = 0;
        std::vector<int64_t> timeMs;
        std::vector<uint8_t> cells; // rows * cols, dB = v / 2 - 130
    };

    static constexpr int kMaxCols = 1024;

    explicit Scanner(RecordingStore& store) : store_(store) {}
    ~Scanner() { stop(); }

    bool start(const Config& cfg, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    Status status();
    Spectrum spectrum();
    bool history(int window, History& out);

    // address: rtl_tcp server / Pluto URI for the network sources.
    static std::vector<SdrDeviceInfo> listDevices(const std::string& type, const std::string& address = "");
    static std::vector<Window> splitWindows(const std::vector<FreqRange>& ranges, double fs);

private:
    struct WinState
    {
        std::vector<float> noise;
        std::vector<double> learnSum;
        int learnFrames = 0;
        bool ready = false;
        // Long-term waterfall (one row per second while dwelling here).
        std::vector<float> histMax;
        int histN = 0;
    };

    struct Active
    {
        std::shared_ptr<Channel> ch;
        int bin;
    };

    void onSamples(const float* iq, int n);
    void dspLoop();
    void tune(int w);
    void frame();
    void detect(WinState& W, const std::vector<float>& sm, uint64_t frameEnd);
    void pool(const std::vector<float>& src, std::vector<float>& dst) const;
    bool ignored(double f) const;
    int64_t nowMs() const;

    RecordingStore& store_;
    Config cfg_;
    std::unique_ptr<SdrSource> src_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};

    // SDR thread -> DSP thread
    std::mutex qMtx_;
    std::condition_variable qCv_;
    std::deque<std::shared_ptr<IqBlock>> queue_;
    size_t queuedSamples_ = 0;
    std::atomic<uint64_t> drops_{0};

    // DSP thread state
    double fs_ = 0;
    int N_ = 4096;
    int K_ = 1;  // FFTs per frame
    int T_ = 8;  // frames in the time average
    int g_ = 4;  // grouping half-width in bins
    double binHz_ = 0;
    std::vector<Window> windows_;
    std::vector<WinState> ws_;
    int cur_ = 0;
    double centerHz_ = 0;
    uint64_t sample_ = 0;      // absolute sample counter (end of last block)
    uint64_t frameEnd_ = 0;    // sample index at the end of the current frame
    uint64_t ringHeadSample_ = 0;
    uint64_t skip_ = 0;        // samples to discard after a retune
    uint64_t dwellStart_ = 0;
    JFFT fft_;
    std::vector<double> window_;
    double winNorm_ = 1.0;
    std::vector<JFFT::cpx_type> fftBuf_;
    int fftFill_ = 0;
    std::vector<double> welch_;
    int welchN_ = 0;
    std::deque<std::vector<float>> avgRing_;
    std::vector<double> avgSum_;
    std::vector<float> ring_; // pre-roll IQ (interleaved)
    size_t ringPos_ = 0, ringCount_ = 0;
    std::vector<Active> active_;
    std::vector<std::shared_ptr<Channel>> closing_;
    int nextId_ = 1;
    int iLo_ = 0, iHi_ = 0;

    // Shared with API threads
    mutable std::mutex snapMtx_;
    Status status_;
    Spectrum spec_;
    std::vector<History> hist_;
    uint64_t frames_ = 0;
    int64_t fpsT0_ = 0;
    uint64_t fpsF0_ = 0;
};
