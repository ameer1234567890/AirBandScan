// Airspy HF+ (Dual Port / Discovery) source using libairspyhf directly.
// Covers 9 kHz - 31 MHz and 60 - 260 MHz, so the whole VHF airband.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct airspyhf_device;

class AirspyHfSource : public SdrSource
{
public:
    AirspyHfSource() = default;
    ~AirspyHfSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override; // snapped to a rate the device supports
    void setGain(double db) override;       // <0 = AGC, else 0..48 dB (attenuator 48-gain)
    void setBiasTee(bool on) override { (void)on; }
    void setPpm(double ppm) override;

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    int handleRx(const float* iq, int n);

private:
    void applyGain();

    airspyhf_device* dev_ = nullptr;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 128.0e6;
    double sampleRate_ = 768000.0;
    double gainDb_ = -1.0;
    double ppm_ = 0.0;
};
