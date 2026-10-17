// RTL-SDR over the network: a client for the rtl_tcp protocol (rtl_tcp,
// rtl_tcp from rtl-sdr-blog, SDR++ / SDR# rtl_tcp servers, Android "RTL-SDR
// driver"). Address "host:port", "[ipv6]:port" or "host" (port 1234).
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtlTcpSource : public SdrSource
{
public:
    RtlTcpSource() = default;
    ~RtlTcpSource() override;

    std::vector<SdrDeviceInfo> listDevices() override; // probes the configured server

    void setAddress(const std::string& addr) override { address_ = addr; }
    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;   // <0 = tuner AGC
    void setBiasTee(bool on) override;
    void setPpm(double ppm) override;
    void setDcBlock(bool on) override { dcBlock_.store(on); }
    // Samples already in flight on the network still belong to the old frequency.
    double retuneSettleSec() const override { return 0.40; }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // "host:port" -> host, port (default 1234). False on a malformed address.
    static bool parseAddress(const std::string& in, std::string& host, int& port);

private:
    bool connectTo(const std::string& addr, std::string& tuner, std::string& err);
    void sendCmd(uint8_t cmd, uint32_t param);
    void applyGain();
    void readLoop();
    void closeSocket();

    std::string address_;
    intptr_t sock_ = -1;
    std::mutex sendMtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    SdrSampleCb cb_;

    double centerFreq_ = 128.0e6;
    double sampleRate_ = 2.4e6;
    double gainDb_ = 40.0;
    double ppm_ = 0.0;
    bool biasTee_ = false;

    std::atomic<bool> dcBlock_{true};
};
