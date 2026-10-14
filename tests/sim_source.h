// Test-only SDR: white noise plus AM carriers at fixed absolute frequencies,
// honouring setCenterFreq() so the scanner's range hopping can be exercised.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <thread>
#include <vector>

struct SimSignal { double freqHz, startSec, durSec, amp; };

class SimSource : public SdrSource
{
public:
    static std::vector<SimSignal> signals;

    std::vector<SdrDeviceInfo> listDevices() override { return {{0, "sim", "0"}}; }
    void setCenterFreq(double hz) override { center_ = hz; }
    void setSampleRate(double hz) override { fs_ = hz; }
    void setGain(double) override {}
    void setBiasTee(bool) override {}
    void setPpm(double) override {}
    double centerFreq() const override { return center_; }
    double sampleRate() const override { return fs_; }
    bool running() const override { return run_; }
    void stop() override { run_ = false; if (th_.joinable()) th_.join(); }
    ~SimSource() override { stop(); }

    bool start(int, SdrSampleCb cb, std::string&) override
    {
        run_ = true;
        th_ = std::thread([this, cb] {
            const int B = 16384;
            std::vector<float> buf(2 * B);
            uint64_t n = 0, rng = 88172645463325252ull;
            auto uni = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); };
            auto t0 = std::chrono::steady_clock::now();
            while (run_)
            {
                double c = center_.load();
                for (int i = 0; i < B; ++i)
                {
                    double t = (double)(n + i) / fs_;
                    double re = (uni() + uni() + uni() - 1.5) * 0.03, im = (uni() + uni() + uni() - 1.5) * 0.03;
                    for (auto& s : signals)
                    {
                        if (t < s.startSec || t > s.startSec + s.durSec || std::fabs(s.freqHz - c) > fs_ * 0.45) continue;
                        double m = 1.0 + 0.6 * std::sin(2 * M_PI * 400 * t) * (std::sin(2 * M_PI * 3 * t) > 0 ? 1 : 0);
                        double ph = 2 * M_PI * (s.freqHz - c) * t;
                        re += s.amp * m * std::cos(ph);
                        im += s.amp * m * std::sin(ph);
                    }
                    buf[2 * i] = (float)re;
                    buf[2 * i + 1] = (float)im;
                }
                n += B;
                cb(buf.data(), B);
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>((double)n / fs_));
            }
        });
        return true;
    }

private:
    std::atomic<double> center_{120e6};
    double fs_ = 2.4e6;
    std::atomic<bool> run_{false};
    std::thread th_;
};
