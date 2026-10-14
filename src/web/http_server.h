// Minimal embedded HTTP/1.1 server (Windows + POSIX sockets, no deps).
// One short-lived thread per connection; supports GET/POST/HEAD, query
// strings, url-encoded forms and byte-range file responses (needed for
// seeking in <audio>).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

struct HttpRequest
{
    std::string method;
    std::string path;                          // decoded, without query
    std::map<std::string, std::string> query;  // decoded
    std::map<std::string, std::string> headers; // lower-case keys
    std::string body;

    std::string q(const std::string& k, const std::string& def = "") const
    {
        auto it = query.find(k);
        return it == query.end() ? def : it->second;
    }
    // Parse body as application/x-www-form-urlencoded.
    std::map<std::string, std::string> form() const;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::string filePath;      // if set, stream this file instead of body
    std::string downloadName;  // if set, Content-Disposition: attachment
    const unsigned char* staticData = nullptr; // or serve this buffer
    size_t staticSize = 0;
    bool gzipped = false;
    std::map<std::string, std::string> headers;

    static HttpResponse json(const std::string& s, int status = 200)
    {
        HttpResponse r;
        r.status = status;
        r.contentType = "application/json";
        r.body = s;
        return r;
    }
    static HttpResponse text(int status, const std::string& s)
    {
        HttpResponse r;
        r.status = status;
        r.body = s;
        return r;
    }
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer
{
public:
    ~HttpServer() { stop(); }
    bool start(const std::string& bind, int port, HttpHandler h, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

private:
    void acceptLoop();
    void handle(intptr_t sock);

    HttpHandler handler_;
    intptr_t listen_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
};

std::string urlDecode(const std::string& s);
std::string jsonEscape(const std::string& s);
