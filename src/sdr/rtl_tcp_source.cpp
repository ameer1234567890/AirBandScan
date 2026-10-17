#include "sdr/rtl_tcp_source.h"

#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define SHUT_RDWR SD_BOTH
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace {
// rtl_tcp commands: 1 byte id + 32-bit big-endian parameter.
enum : uint8_t
{
    kSetFreq = 0x01,
    kSetSampleRate = 0x02,
    kSetGainMode = 0x03,   // 0 = auto, 1 = manual
    kSetGain = 0x04,       // tenths of a dB
    kSetFreqCorrection = 0x05,
    kSetAgcMode = 0x08,    // RTL2832 digital AGC
    kSetBiasTee = 0x0e,
};

constexpr int kConnectTimeoutMs = 4000;

const char* tunerName(uint32_t t)
{
    switch (t)
    {
    case 1: return "E4000";
    case 2: return "FC0012";
    case 3: return "FC0013";
    case 4: return "FC2580";
    case 5: return "R820T";
    case 6: return "R828D";
    default: return "unknown tuner";
    }
}

void netInit()
{
#if defined(_WIN32)
    static bool done = false;
    if (!done)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

void setBlocking(sock_t s, bool blocking)
{
#if defined(_WIN32)
    u_long nb = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

// connect() with a timeout so an unreachable host fails in seconds, not minutes.
bool connectTimeout(sock_t s, const sockaddr* sa, socklen_t len)
{
    setBlocking(s, false);
    int r = ::connect(s, sa, len);
    if (r != 0)
    {
#if defined(_WIN32)
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(s, &wf); FD_SET(s, &ef);
        timeval tv{kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
        if (select(0, nullptr, &wf, &ef, &tv) <= 0 || FD_ISSET(s, &ef)) return false;
#else
        if (errno != EINPROGRESS) return false;
        pollfd p{s, POLLOUT, 0};
        if (poll(&p, 1, kConnectTimeoutMs) <= 0) return false;
        int soErr = 0;
        socklen_t l = sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &l) != 0 || soErr != 0) return false;
#endif
    }
    setBlocking(s, true);
    return true;
}

bool recvAll(sock_t s, uint8_t* buf, int len)
{
    while (len > 0)
    {
        int r = ::recv(s, (char*)buf, len, 0);
        if (r <= 0) return false;
        buf += r;
        len -= r;
    }
    return true;
}
} // namespace

RtlTcpSource::~RtlTcpSource()
{
    stop();
}

bool RtlTcpSource::parseAddress(const std::string& in, std::string& host, int& port)
{
    std::string s = in;
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    if (s.rfind("rtl_tcp://", 0) == 0) s = s.substr(10);
    else if (s.rfind("tcp://", 0) == 0) s = s.substr(6);
    if (s.empty()) s = "127.0.0.1";
    port = 1234;
    std::string p;
    if (s[0] == '[') // [ipv6]:port
    {
        size_t e = s.find(']');
        if (e == std::string::npos) return false;
        host = s.substr(1, e - 1);
        if (e + 1 < s.size())
        {
            if (s[e + 1] != ':') return false;
            p = s.substr(e + 2);
        }
    }
    else if (std::count(s.begin(), s.end(), ':') == 1)
    {
        size_t c = s.find(':');
        host = s.substr(0, c);
        p = s.substr(c + 1);
    }
    else
        host = s; // plain host or bare IPv6
    if (host.empty()) return false;
    if (!p.empty())
    {
        char* end = nullptr;
        long v = std::strtol(p.c_str(), &end, 10);
        if (*end || v < 1 || v > 65535) return false;
        port = (int)v;
    }
    return true;
}

bool RtlTcpSource::connectTo(const std::string& addr, std::string& tuner, std::string& err)
{
    netInit();
    std::string host;
    int port = 0;
    if (!parseAddress(addr, host, port))
    {
        err = "rtl_tcp: bad address '" + addr + "' (expected host:port)";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    {
        err = "rtl_tcp: cannot resolve " + host;
        return false;
    }
    sock_t s = INVALID_SOCKET;
    for (addrinfo* a = res; a; a = a->ai_next)
    {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connectTimeout(s, a->ai_addr, (socklen_t)a->ai_addrlen)) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET)
    {
        err = "rtl_tcp: cannot connect to " + host + ":" + std::to_string(port) + " (is rtl_tcp running there?)";
        return false;
    }

    // A dead server should end the stream, not hang it forever.
#if defined(_WIN32)
    DWORD tmo = 5000;
#else
    timeval tmo{5, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    // Keep the receive buffer modest: whatever sits in it is stale after a hop.
    int rcv = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));

    // 12-byte greeting: "RTL0", tuner type, gain count (big-endian).
    uint8_t hdr[12];
    if (!recvAll(s, hdr, 12) || std::memcmp(hdr, "RTL0", 4) != 0)
    {
        closesocket(s);
        err = "rtl_tcp: " + host + ":" + std::to_string(port) + " did not answer like an rtl_tcp server";
        return false;
    }
    uint32_t t = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
    tuner = tunerName(t);
    sock_ = (intptr_t)s;
    return true;
}

std::vector<SdrDeviceInfo> RtlTcpSource::listDevices()
{
    // rtl_tcp serves one dongle: report it if the configured server answers.
    std::string tuner, err;
    std::vector<SdrDeviceInfo> out;
    if (!connectTo(address_, tuner, err))
        return out;
    closeSocket();
    SdrDeviceInfo d;
    d.name = "rtl_tcp " + tuner;
    d.serial = address_.empty() ? "127.0.0.1:1234" : address_;
    out.push_back(d);
    return out;
}

bool RtlTcpSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    std::string tuner;
    if (!connectTo(address_, tuner, err))
        return false;
    logWrite("[rtl_tcp] connected to %s (%s)", address_.empty() ? "127.0.0.1:1234" : address_.c_str(), tuner.c_str());

    sendCmd(kSetSampleRate, (uint32_t)std::llround(sampleRate_));
    sendCmd(kSetFreqCorrection, (uint32_t)(int32_t)std::lround(ppm_));
    sendCmd(kSetFreq, (uint32_t)std::llround(centerFreq_));
    sendCmd(kSetAgcMode, 0);
    applyGain();
    sendCmd(kSetBiasTee, biasTee_ ? 1 : 0);

    cb_ = std::move(cb);
    running_.store(true);
    thread_ = std::thread(&RtlTcpSource::readLoop, this);
    return true;
}

void RtlTcpSource::readLoop()
{
    std::vector<uint8_t> buf(64 * 1024 + 1);
    std::vector<float> out;
    size_t carry = 0; // an odd byte left over from the previous recv
    float offRe = 0.0f, offIm = 0.0f;
    const float rate = (float)(50.0 / sampleRate_);
    while (running_.load())
    {
        int r = ::recv((sock_t)sock_, (char*)buf.data() + carry, (int)(buf.size() - 1 - carry), 0);
        if (!running_.load())
            break;
        if (r <= 0)
        {
            logWrite("[rtl_tcp] connection closed or timed out");
            running_.store(false); // the scanner reports "SDR stopped delivering samples"
            break;
        }
        size_t total = carry + (size_t)r;
        size_t n = total / 2;
        out.resize(n * 2);
        const bool dc = dcBlock_.load();
        for (size_t i = 0; i < n; ++i)
        {
            float re = ((int)buf[i * 2] - 128) * (1.0f / 128.0f);
            float im = ((int)buf[i * 2 + 1] - 128) * (1.0f / 128.0f);
            if (dc)
            {
                re -= offRe; offRe += re * rate;
                im -= offIm; offIm += im * rate;
            }
            out[i * 2] = re;
            out[i * 2 + 1] = im;
        }
        carry = total & 1;
        if (carry)
            buf[0] = buf[total - 1];
        if (cb_ && n)
            cb_(out.data(), (int)n);
    }
}

void RtlTcpSource::stop()
{
    running_.store(false);
    if (sock_ != -1)
        ::shutdown((sock_t)sock_, SHUT_RDWR); // wakes the blocked recv
    if (thread_.joinable())
        thread_.join();
    closeSocket();
    cb_ = nullptr;
}

void RtlTcpSource::closeSocket()
{
    if (sock_ != -1)
        closesocket((sock_t)sock_);
    sock_ = -1;
}

void RtlTcpSource::sendCmd(uint8_t cmd, uint32_t param)
{
    std::lock_guard<std::mutex> lk(sendMtx_);
    if (sock_ == -1)
        return;
    uint8_t b[5] = {cmd, (uint8_t)(param >> 24), (uint8_t)(param >> 16), (uint8_t)(param >> 8), (uint8_t)param};
    ::send((sock_t)sock_, (const char*)b, 5, 0);
}

void RtlTcpSource::applyGain()
{
    if (gainDb_ < 0)
        sendCmd(kSetGainMode, 0);
    else
    {
        sendCmd(kSetGainMode, 1);
        sendCmd(kSetGain, (uint32_t)std::lround(gainDb_ * 10.0)); // server picks the nearest step
    }
}

void RtlTcpSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    sendCmd(kSetFreq, (uint32_t)std::llround(hz));
}

void RtlTcpSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    sendCmd(kSetSampleRate, (uint32_t)std::llround(hz));
}

void RtlTcpSource::setGain(double db)
{
    gainDb_ = db;
    applyGain();
}

void RtlTcpSource::setBiasTee(bool on)
{
    biasTee_ = on;
    sendCmd(kSetBiasTee, on ? 1 : 0);
}

void RtlTcpSource::setPpm(double ppm)
{
    ppm_ = ppm;
    sendCmd(kSetFreqCorrection, (uint32_t)(int32_t)std::lround(ppm));
}
