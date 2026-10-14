#include "web/http_server.h"

#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
typedef int SOCKET;
#endif

namespace {

constexpr int kMaxClients = 64;
constexpr size_t kMaxBody = 1 << 20;

bool sendAll(SOCKET s, const char* p, size_t n)
{
    while (n > 0)
    {
        int w = ::send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

const char* statusText(int s)
{
    switch (s)
    {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

void parseQuery(const std::string& qs, std::map<std::string, std::string>& out)
{
    std::stringstream ss(qs);
    std::string kv;
    while (std::getline(ss, kv, '&'))
    {
        if (kv.empty()) continue;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) out[urlDecode(kv)] = "";
        else out[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
    }
}

} // namespace

std::string urlDecode(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
        {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        }
        else o += s[i];
    }
    return o;
}

std::string jsonEscape(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char h[8]; std::snprintf(h, sizeof(h), "\\u%04x", c); o += h; }
        else o += (char)c;
    }
    return o + "\"";
}

std::map<std::string, std::string> HttpRequest::form() const
{
    std::map<std::string, std::string> m;
    parseQuery(body, m);
    return m;
}

bool HttpServer::start(const std::string& bind, int port, HttpHandler h, std::string& err)
{
    stop();
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    handler_ = std::move(h);
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { err = "socket() failed"; return false; }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!bind.empty() && bind != "0.0.0.0")
    {
        unsigned long a = inet_addr(bind.c_str());
        if (a != INADDR_NONE) addr.sin_addr.s_addr = a;
    }
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || ::listen(ls, 32) == SOCKET_ERROR)
    {
        closesocket(ls);
        err = "cannot listen on port " + std::to_string(port) + " (in use?)";
        return false;
    }
    listen_ = (intptr_t)ls;
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop()
{
    if (!running_.exchange(false)) return;
    if (listen_ != -1) closesocket((SOCKET)listen_);
    if (thread_.joinable()) thread_.join();
    listen_ = -1;
    // Let in-flight requests finish (they hold no references to us beyond handler_).
    for (int i = 0; i < 50 && clients_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void HttpServer::acceptLoop()
{
    while (running_)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET((SOCKET)listen_, &fds);
        timeval tv{0, 200000};
        if (::select((int)listen_ + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = ::accept((SOCKET)listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        if (clients_.load() >= kMaxClients)
        {
            const char* busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(c, busy, std::strlen(busy));
            closesocket(c);
            continue;
        }
        clients_++;
        std::thread([this, c] {
            handle((intptr_t)c);
            clients_--;
        }).detach();
    }
}

void HttpServer::handle(intptr_t sockv)
{
    SOCKET s = (SOCKET)sockv;
#if defined(_WIN32)
    DWORD tmo = 15000;
#else
    timeval tmo{15, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

    std::string data;
    char buf[8192];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos && data.size() < 65536)
    {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { closesocket(s); return; }
        data.append(buf, (size_t)n);
        hdrEnd = data.find("\r\n\r\n");
    }
    if (hdrEnd == std::string::npos) { closesocket(s); return; }

    HttpRequest req;
    std::istringstream hs(data.substr(0, hdrEnd));
    std::string line, target, ver;
    std::getline(hs, line);
    {
        std::istringstream rl(line);
        rl >> req.method >> target >> ver;
    }
    while (std::getline(hs, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        v.erase(0, v.find_first_not_of(' '));
        req.headers[k] = v;
    }
    size_t qm = target.find('?');
    req.path = urlDecode(target.substr(0, qm));
    if (qm != std::string::npos) parseQuery(target.substr(qm + 1), req.query);

    size_t clen = 0;
    if (req.headers.count("content-length"))
        clen = (size_t)std::strtoull(req.headers["content-length"].c_str(), nullptr, 10);
    if (clen > kMaxBody) { closesocket(s); return; }
    req.body = data.substr(hdrEnd + 4);
    while (req.body.size() < clen)
    {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.body.append(buf, (size_t)n);
    }
    if (req.body.size() > clen) req.body.resize(clen);

    HttpResponse res;
    try
    {
        res = handler_(req);
    }
    catch (const std::exception& e)
    {
        res = HttpResponse::text(500, std::string("error: ") + e.what());
    }

    std::ostringstream h;
    bool head = req.method == "HEAD";
    auto writeHeaders = [&](int status, size_t length, const std::string& extra) {
        h << "HTTP/1.1 " << status << ' ' << statusText(status) << "\r\n"
          << "Content-Type: " << res.contentType << "\r\n"
          << "Content-Length: " << length << "\r\n"
          << "Connection: close\r\n"
          << extra;
        for (auto& [k, v] : res.headers) h << k << ": " << v << "\r\n";
        if (!res.downloadName.empty())
            h << "Content-Disposition: attachment; filename=\"" << res.downloadName << "\"\r\n";
        h << "\r\n";
        std::string hdr = h.str();
        return sendAll(s, hdr.data(), hdr.size());
    };

    if (!res.filePath.empty())
    {
        std::FILE* f = std::fopen(res.filePath.c_str(), "rb");
        if (!f)
        {
            res = HttpResponse::text(404, "not found");
            writeHeaders(404, res.body.size(), "");
            sendAll(s, res.body.data(), res.body.size());
            closesocket(s);
            return;
        }
        std::fseek(f, 0, SEEK_END);
        long long size = std::ftell(f);
        long long from = 0, to = size - 1;
        int status = 200;
        std::string extra = "Accept-Ranges: bytes\r\n";
        auto rit = req.headers.find("range");
        if (rit != req.headers.end() && rit->second.rfind("bytes=", 0) == 0 && size > 0)
        {
            std::string r = rit->second.substr(6);
            size_t dash = r.find('-');
            if (dash != std::string::npos)
            {
                std::string a = r.substr(0, dash), b = r.substr(dash + 1);
                if (a.empty()) { from = std::max(0LL, size - std::atoll(b.c_str())); }
                else
                {
                    from = std::atoll(a.c_str());
                    if (!b.empty()) to = std::min(size - 1, std::atoll(b.c_str()));
                }
                if (from > to || from >= size)
                {
                    std::fclose(f);
                    writeHeaders(416, 0, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
                    closesocket(s);
                    return;
                }
                status = 206;
                extra += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) +
                         "/" + std::to_string(size) + "\r\n";
            }
        }
        long long len = size > 0 ? to - from + 1 : 0;
        if (writeHeaders(status, (size_t)len, extra) && !head && len > 0)
        {
            std::fseek(f, (long)from, SEEK_SET);
            std::vector<char> chunk(65536);
            while (len > 0)
            {
                size_t want = (size_t)std::min<long long>(len, (long long)chunk.size());
                size_t got = std::fread(chunk.data(), 1, want, f);
                if (got == 0 || !sendAll(s, chunk.data(), got)) break;
                len -= (long long)got;
            }
        }
        std::fclose(f);
    }
    else if (res.staticData)
    {
        std::string extra = res.gzipped ? "Content-Encoding: gzip\r\n" : "";
        extra += "Cache-Control: no-cache\r\n";
        if (writeHeaders(res.status, res.staticSize, extra) && !head)
            sendAll(s, (const char*)res.staticData, res.staticSize);
    }
    else
    {
        if (writeHeaders(res.status, res.body.size(), "Cache-Control: no-store\r\n") && !head)
            sendAll(s, res.body.data(), res.body.size());
    }

#if defined(_WIN32)
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    closesocket(s);
}
