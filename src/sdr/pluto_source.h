// ADALM-Pluto (PlutoSDR / Pluto+) source using libiio. The radio is reached
// by URI: "ip:192.168.2.1" (USB network gadget or Ethernet), "ip:pluto.local",
// "usb:1.4.5"; empty = first Pluto found by a USB scan, else ip:192.168.2.1.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <string>
#include <thread>

struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;
struct iio_channels_mask; // libiio 1.x
struct iio_stream;        // libiio 1.x

class PlutoSource : public SdrSource
{
public:
    PlutoSource() = default;
    ~PlutoSource() override;

    std::vector<SdrDeviceInfo> listDevices() override; // serial = URI

    void setAddress(const std::string& uri) override { uri_ = uri; }
    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;       // <0 = slow_attack AGC, else manual 0..71 dB
    void setBiasTee(bool on) override { (void)on; }
    void setPpm(double ppm) override;
    void setDcBlock(bool on) override { dcBlock_.store(on); }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Normalises user input: "192.168.2.1" -> "ip:192.168.2.1".
    static std::string normaliseUri(const std::string& s);

private:
    bool applyTune();
    void applyGain();
    void readLoop();
    void closeAll();

    std::string uri_;
    iio_context* ctx_ = nullptr;
    iio_device* phy_ = nullptr;
    iio_device* rx_ = nullptr;
    iio_channel* rxCfg_ = nullptr; // phy "voltage0" input: rate, gain, port
    iio_channel* lo_ = nullptr;    // phy "altvoltage0" output: RX LO
    iio_channel* chI_ = nullptr;
    iio_channel* chQ_ = nullptr;
    iio_buffer* buf_ = nullptr;
    iio_channels_mask* mask_ = nullptr; // libiio 1.x only
    iio_stream* stream_ = nullptr;      // libiio 1.x only
    long sampleSize_ = 0;               // libiio 1.x only

    std::thread thread_;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 128.0e6;
    double sampleRate_ = 2.4e6;
    double gainDb_ = 50.0;
    double ppm_ = 0.0;

    std::atomic<bool> dcBlock_{true};
};
