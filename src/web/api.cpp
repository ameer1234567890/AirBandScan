#include "web/api.h"

#include "audio/audio_files.h"
#include "util/log.h"
#include "version.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// Generated at build time from src/web/ui.html (see cmake/embed.cmake).
extern const unsigned char kUiHtml[];
extern const size_t kUiHtmlSize;

namespace {

std::string fmt(double v, int prec)
{
    if (!std::isfinite(v)) return "0";
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

std::string recJson(const RecordingInfo& r)
{
    std::ostringstream o;
    o << "{\"id\":" << jsonEscape(r.id) << ",\"t\":" << r.startMs << ",\"d\":" << r.durationMs
      << ",\"f\":" << fmt(r.freqHz, 1) << ",\"pk\":" << fmt(r.peakDb, 1) << ",\"lv\":" << fmt(r.levelDbfs, 1)
      << ",\"v\":" << (r.voice ? 1 : 0) << ",\"m\":" << (r.manual ? 1 : 0) << ",\"s\":" << fmt(r.score, 2)
      << ",\"vf\":" << fmt(r.voicedFrac, 2) << ",\"ls\":" << fmt(r.levelSpreadDb, 1)
      << ",\"md\":" << fmt(r.modDepth, 3) << ",\"r\":" << jsonEscape(r.reason)
      << ",\"mp3\":" << (r.hasMp3 ? 1 : 0) << ",\"spg\":" << (r.hasSpg ? 1 : 0) << "}";
    return o.str();
}

void floatArray(std::ostringstream& o, const std::vector<float>& v)
{
    o << '[';
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i) o << ',';
        o << fmt(v[i], 1);
    }
    o << ']';
}

int kindOf(const std::string& k)
{
    if (k == "voice") return 1;
    if (k == "other") return 2;
    return 0;
}

const char* deviceTypes()
{
    return "[\"rtlsdr\",\"rtltcp\""
#if defined(HAS_AIRSPY)
           ",\"airspy\""
#endif
#if defined(HAS_AIRSPYHF)
           ",\"airspyhf\""
#endif
#if defined(HAS_SDRPLAY)
           ",\"sdrplay\""
#endif
#if defined(HAS_HACKRF)
           ",\"hackrf\""
#endif
#if defined(HAS_PLUTO)
           ",\"pluto\""
#endif
           ",\"wav\"]";
}

} // namespace

void App::setStoreRoots(const Config& c)
{
    std::string imgRoot = (fs::path(c.recordingsDir) / "images").string();
    if (images.root() != imgRoot)
    {
        images.setRoot(imgRoot);
        images.load();
    }
}

bool App::startActive(std::string& err)
{
    std::string mode;
    {
        std::lock_guard<std::mutex> l2(cfgMtx);
        mode = cfg.runMode;
    }
    return mode == "satellite" ? startSat(err) : startScanner(err);
}

bool App::startScanner(std::string& err)
{
    std::lock_guard<std::mutex> lk(ctlMtx);
    Config c;
    {
        std::lock_guard<std::mutex> l2(cfgMtx);
        c = cfg;
    }
    sat.stop();
    store.setRoot(c.recordingsDir);
    setStoreRoots(c);
    return scanner.start(c, err);
}

bool App::startSat(std::string& err)
{
    std::lock_guard<std::mutex> lk(ctlMtx);
    Config c;
    {
        std::lock_guard<std::mutex> l2(cfgMtx);
        c = cfg;
    }
    scanner.stop();
    setStoreRoots(c);
    return sat.start(c, err);
}

void App::stopAll()
{
    std::lock_guard<std::mutex> lk(ctlMtx);
    scanner.stop();
    sat.stop();
}

std::string App::runMode()
{
    std::lock_guard<std::mutex> lk(cfgMtx);
    return cfg.runMode;
}

HttpResponse App::routeSat(const HttpRequest& req)
{
    const std::string& p = req.path;
    const bool post = req.method == "POST";

    if (p == "/api/sat/status")
    {
        SatReceiver::Status s = sat.status();
        std::ostringstream o;
        o << "{\"running\":" << (s.running ? 1 : 0) << ",\"error\":" << jsonEscape(s.error)
          << ",\"mode\":" << jsonEscape(s.mode) << ",\"variant\":" << jsonEscape(s.variant)
          << ",\"device\":" << jsonEscape(s.device) << ",\"freq\":" << fmt(s.freqHz, 0)
          << ",\"fs\":" << fmt(s.fs, 0) << ",\"chRate\":" << fmt(s.chRate, 0)
          << ",\"level\":" << fmt(s.levelDb, 1) << ",\"started\":" << s.startedMs
          << ",\"drops\":" << s.drops << ",\"fileDone\":" << (s.fileDone ? 1 : 0)
          << ",\"sstvBusy\":" << (s.sstvBusy ? 1 : 0) << ",\"sstvMode\":" << jsonEscape(s.sstvMode)
          << ",\"sstvRows\":" << s.sstvRows << ",\"sstvTotal\":" << s.sstvTotal << ",\"tone\":" << fmt(s.tone, 0)
          << ",\"carrier\":" << (s.carrier ? 1 : 0) << ",\"lockQ\":" << fmt(s.lockQ, 2)
          << ",\"offset\":" << fmt(s.offsetHz, 0) << ",\"viterbi\":" << (s.viterbi ? 1 : 0)
          << ",\"ber\":" << fmt(s.ber, 3) << ",\"frameSync\":" << (s.frameSync ? 1 : 0)
          << ",\"frames\":" << s.frames << ",\"framesBad\":" << s.framesBad << ",\"packets\":" << s.packets
          << ",\"rows\":" << s.rows << ",\"passActive\":" << (s.passActive ? 1 : 0)
          << ",\"passStart\":" << s.passStartMs << ",\"channels\":[";
        for (size_t i = 0; i < s.channels.size(); ++i) o << (i ? "," : "") << s.channels[i];
        o << "],\"const\":[";
        for (size_t i = 0; i < s.constellation.size(); ++i)
            o << (i ? "," : "") << fmt(s.constellation[i].real(), 2) << ',' << fmt(s.constellation[i].imag(), 2);
        o << "],\"saved\":" << s.imagesSaved << ",\"live\":" << s.liveVersion
          << ",\"runMode\":" << jsonEscape(runMode()) << '}';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/sat/live.png")
    {
        std::string png = sat.livePng();
        if (png.empty()) return HttpResponse::text(404, "no picture in progress");
        HttpResponse r;
        r.contentType = "image/png";
        r.body = std::move(png);
        r.headers["Cache-Control"] = "no-store";
        return r;
    }

    // Start the satellite receiver: form fields sat_mode, sat_freq_mhz, sat_lrpt_variant.
    if (p == "/api/sat/start" && post)
    {
        std::string err;
        auto form = req.form();
        form["run_mode"] = "satellite";
        Config c;
        {
            std::lock_guard<std::mutex> lk(cfgMtx);
            c = cfg;
        }
        if (!c.fromMap(form, err)) return HttpResponse::json("{\"ok\":0,\"error\":" + jsonEscape(err) + "}", 400);
        {
            std::lock_guard<std::mutex> lk(cfgMtx);
            cfg = c;
            if (!cfg.save(cfgPath)) logWrite("[api] cannot save %s", cfgPath.c_str());
        }
        bool ok = startSat(err);
        return HttpResponse::json(std::string("{\"ok\":") + (ok ? "1" : "0") + ",\"error\":" + jsonEscape(err) + "}");
    }

    // Stop the satellite receiver and go back to scanning the airband.
    if (p == "/api/sat/stop" && post)
    {
        {
            std::lock_guard<std::mutex> lk(cfgMtx);
            cfg.runMode = "scanner";
            if (!cfg.save(cfgPath)) logWrite("[api] cannot save %s", cfgPath.c_str());
        }
        std::string err;
        bool ok = req.q("scan", "1") == "1" ? startScanner(err) : (stopAll(), true);
        return HttpResponse::json(std::string("{\"ok\":") + (ok ? "1" : "0") + ",\"error\":" + jsonEscape(err) + "}");
    }

    if (p == "/api/images")
    {
        int total = 0;
        auto items = images.list(req.q("kind", ""), std::max(0, std::atoi(req.q("offset", "0").c_str())),
                                 std::clamp(std::atoi(req.q("limit", "40").c_str()), 1, 500), total);
        std::ostringstream o;
        o << "{\"total\":" << total << ",\"version\":" << images.version() << ",\"items\":[";
        for (size_t i = 0; i < items.size(); ++i)
        {
            auto& r = items[i];
            o << (i ? "," : "") << "{\"id\":" << jsonEscape(r.id) << ",\"kind\":" << jsonEscape(r.kind)
              << ",\"mode\":" << jsonEscape(r.mode) << ",\"t\":" << r.startMs << ",\"end\":" << r.endMs
              << ",\"f\":" << fmt(r.freqHz, 0) << ",\"w\":" << r.w << ",\"h\":" << r.h
              << ",\"complete\":" << (r.complete ? 1 : 0) << ",\"note\":" << jsonEscape(r.note) << ",\"files\":[";
            for (size_t k = 0; k < r.files.size(); ++k) o << (k ? "," : "") << jsonEscape(r.files[k]);
            o << "]}";
        }
        o << "]}";
        return HttpResponse::json(o.str());
    }

    // /api/img/<id>/delete
    if (p.rfind("/api/img/", 0) == 0 && post)
    {
        std::string rest = p.substr(9);
        size_t sl = rest.find('/');
        if (sl == std::string::npos || rest.substr(sl + 1) != "delete") return HttpResponse::text(404, "not found");
        bool ok = images.remove(rest.substr(0, sl));
        return HttpResponse::json(std::string("{\"ok\":") + (ok ? "1" : "0") + "}", ok ? 200 : 404);
    }

    return HttpResponse::text(404, "not found");
}

HttpResponse App::route(const HttpRequest& req)
{
    const std::string& p = req.path;
    const bool post = req.method == "POST";

    if (p == "/" || p == "/index.html")
    {
        HttpResponse r;
        r.contentType = "text/html; charset=utf-8";
        r.staticData = kUiHtml;
        r.staticSize = kUiHtmlSize;
        return r;
    }

    // ---- Recording files: /rec/<id>.<wav|mp3|spg> ----
    if (p.rfind("/rec/", 0) == 0)
    {
        std::string name = p.substr(5);
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) return HttpResponse::text(404, "not found");
        std::string id = name.substr(0, dot), ext = name.substr(dot);
        if (ext != ".wav" && ext != ".mp3" && ext != ".spg") return HttpResponse::text(404, "not found");
        if (ext == ".mp3")
        {
            int kbps;
            {
                std::lock_guard<std::mutex> lk(cfgMtx);
                kbps = cfg.mp3Bitrate;
            }
            std::string err;
            if (!store.ensureMp3(id, kbps, err)) return HttpResponse::text(500, "MP3 encoding failed: " + err);
        }
        std::string path = store.path(id, ext.c_str());
        if (path.empty() || !fs::exists(path)) return HttpResponse::text(404, "not found");
        HttpResponse r;
        r.filePath = path;
        r.contentType = ext == ".wav" ? "audio/wav" : ext == ".mp3" ? "audio/mpeg" : "application/octet-stream";
        if (req.q("dl") == "1")
        {
            RecordingInfo info;
            std::string nice = id;
            if (store.get(id, info))
                nice = id.substr(0, 15) + "_" + fmt(info.freqHz / 1e6, 3) + "MHz";
            r.downloadName = nice + ext;
        }
        return r;
    }

    // ---- Satellite images: /img/<id>[_thumb|_chN].png ----
    if (p.rfind("/img/", 0) == 0)
    {
        std::string path = images.file(p.substr(5));
        if (path.empty() || !fs::exists(path)) return HttpResponse::text(404, "not found");
        HttpResponse r;
        r.filePath = path;
        r.contentType = "image/png";
        if (req.q("dl") == "1") r.downloadName = p.substr(5);
        return r;
    }

    if (p.rfind("/api/", 0) != 0) return HttpResponse::text(404, "not found");

    if (p.rfind("/api/sat", 0) == 0 || p.rfind("/api/images", 0) == 0 || p.rfind("/api/img/", 0) == 0)
        return routeSat(req);

    if (p == "/api/status")
    {
        Scanner::Status s = scanner.status();
        int nv = 0, no = 0;
        store.counts(nv, no);
        std::ostringstream o;
        o << "{\"version\":" << jsonEscape(AIRBANDSCAN_VERSION)
          << ",\"running\":" << (s.running ? 1 : 0) << ",\"error\":" << jsonEscape(s.error)
          << ",\"device\":" << jsonEscape(s.device) << ",\"fs\":" << fmt(s.fs, 0)
          << ",\"center\":" << fmt(s.centerHz, 0) << ",\"binHz\":" << fmt(s.binHz, 1)
          << ",\"window\":" << s.windowIdx << ",\"windows\":[";
        for (size_t i = 0; i < s.windows.size(); ++i)
        {
            if (i) o << ',';
            o << "{\"lo\":" << fmt(s.windows[i].lo, 0) << ",\"hi\":" << fmt(s.windows[i].hi, 0)
              << ",\"ready\":" << (i < s.windowReady.size() && s.windowReady[i] ? 1 : 0) << '}';
        }
        o << "],\"live\":[";
        for (size_t i = 0; i < s.live.size(); ++i)
        {
            auto& l = s.live[i];
            if (i) o << ',';
            o << "{\"id\":" << l.id << ",\"f\":" << fmt(l.freqHz, 1) << ",\"t\":" << l.startMs
              << ",\"rel\":" << fmt(l.relDb, 1) << ",\"a\":" << fmt(l.audio, 3) << '}';
        }
        o << "],\"drops\":" << s.drops << ",\"detections\":" << s.detections
          << ",\"fps\":" << fmt(s.framesPerSec, 1) << ",\"started\":" << s.startedMs
          << ",\"voiceCount\":" << nv << ",\"otherCount\":" << no
          << ",\"storeVersion\":" << store.version()
          << ",\"runMode\":" << jsonEscape(runMode()) << ",\"satRunning\":" << (sat.running() ? 1 : 0)
          << ",\"imageCount\":" << images.count() << ",\"imagesVersion\":" << images.version()
          << ",\"mp3\":" << jsonEscape(mp3Backend()) << '}';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/spectrum")
    {
        Scanner::Spectrum sp = scanner.spectrum();
        std::ostringstream o;
        o << "{\"valid\":" << (sp.valid ? 1 : 0) << ",\"window\":" << sp.windowIdx
          << ",\"lo\":" << fmt(sp.lo, 0) << ",\"hi\":" << fmt(sp.hi, 0) << ",\"center\":" << fmt(sp.centerHz, 0)
          << ",\"start\":" << fmt(sp.startLevel, 1) << ",\"stop\":" << fmt(sp.stopLevel, 1) << ",\"db\":";
        floatArray(o, sp.db);
        o << ",\"noise\":";
        floatArray(o, sp.noise);
        o << '}';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/history")
    {
        Scanner::History h;
        if (!scanner.history(std::atoi(req.q("w", "0").c_str()), h)) return HttpResponse::text(404, "no such window");
        // Binary: u32 cols, u32 rows, f64 lo, f64 hi, f64 time[rows], u8 cells[rows*cols]
        uint32_t cols = (uint32_t)h.cols, rows = (uint32_t)h.timeMs.size();
        std::string b;
        b.append((const char*)&cols, 4);
        b.append((const char*)&rows, 4);
        b.append((const char*)&h.lo, 8);
        b.append((const char*)&h.hi, 8);
        for (int64_t t : h.timeMs) { double d = (double)t; b.append((const char*)&d, 8); }
        b.append((const char*)h.cells.data(), h.cells.size());
        HttpResponse r;
        r.contentType = "application/octet-stream";
        r.body = std::move(b);
        return r;
    }

    if (p == "/api/recordings")
    {
        RecordingStore::Query q;
        q.kind = kindOf(req.q("kind", "voice"));
        q.offset = std::max(0, std::atoi(req.q("offset", "0").c_str()));
        q.limit = std::clamp(std::atoi(req.q("limit", "50").c_str()), 1, 1000);
        q.freqHz = std::atof(req.q("freq", "0").c_str()) * 1e6;
        int total = 0;
        auto items = store.list(q, total);
        std::ostringstream o;
        o << "{\"total\":" << total << ",\"version\":" << store.version() << ",\"items\":[";
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i) o << ',';
            o << recJson(items[i]);
        }
        o << "]}";
        return HttpResponse::json(o.str());
    }

    if (p == "/api/frequencies")
    {
        auto fr = store.frequencies(kindOf(req.q("kind", "voice")));
        std::ostringstream o;
        o << '[';
        for (size_t i = 0; i < fr.size(); ++i)
        {
            if (i) o << ',';
            o << "{\"f\":" << fmt(fr[i].first, 0) << ",\"n\":" << fr[i].second << '}';
        }
        o << ']';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/config")
    {
        if (post)
        {
            std::string err;
            Config c;
            {
                std::lock_guard<std::mutex> lk(cfgMtx);
                c = cfg;
            }
            if (!c.fromMap(req.form(), err)) return HttpResponse::json("{\"ok\":0,\"error\":" + jsonEscape(err) + "}", 400);
            {
                std::lock_guard<std::mutex> lk(cfgMtx);
                cfg = c;
                if (!cfg.save(cfgPath)) logWrite("[api] cannot save %s", cfgPath.c_str());
            }
            if (store.root() != c.recordingsDir)
            {
                store.setRoot(c.recordingsDir);
                store.load();
            }
            logWrite("[api] configuration updated, restarting the %s", c.runMode == "satellite" ? "satellite receiver" : "scanner");
            bool ok = startActive(err);
            return HttpResponse::json(std::string("{\"ok\":1,\"started\":") + (ok ? "1" : "0") +
                                      ",\"error\":" + jsonEscape(ok ? "" : err) + "}");
        }
        std::map<std::string, std::string> m;
        {
            std::lock_guard<std::mutex> lk(cfgMtx);
            m = cfg.toMap();
        }
        std::ostringstream o;
        o << "{\"config\":{";
        bool first = true;
        for (auto& [k, v] : m)
        {
            if (!first) o << ',';
            first = false;
            o << jsonEscape(k) << ':' << jsonEscape(v);
        }
        o << "},\"devices\":" << deviceTypes() << ",\"mp3\":" << jsonEscape(mp3Backend()) << '}';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/devices")
    {
        std::string addr = req.q("address", "");
        if (addr.empty())
        {
            std::lock_guard<std::mutex> lk(cfgMtx);
            addr = cfg.rtlTcpAddress;
        }
        auto list = Scanner::listDevices(req.q("type", "rtlsdr"), addr);
        std::ostringstream o;
        o << '[';
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (i) o << ',';
            o << "{\"index\":" << list[i].index << ",\"name\":" << jsonEscape(list[i].name)
              << ",\"serial\":" << jsonEscape(list[i].serial) << '}';
        }
        o << ']';
        return HttpResponse::json(o.str());
    }

    if (p == "/api/start" && post)
    {
        std::string err;
        bool ok = startActive(err);
        return HttpResponse::json(std::string("{\"ok\":") + (ok ? "1" : "0") + ",\"error\":" + jsonEscape(err) + "}");
    }
    if (p == "/api/stop" && post)
    {
        stopScanner();
        return HttpResponse::json("{\"ok\":1}");
    }

    // /api/rec/<id>/delete  /api/rec/<id>/voice?v=0|1
    if (p.rfind("/api/rec/", 0) == 0 && post)
    {
        std::string rest = p.substr(9);
        size_t sl = rest.find('/');
        if (sl == std::string::npos) return HttpResponse::text(404, "not found");
        std::string id = rest.substr(0, sl), act = rest.substr(sl + 1);
        bool ok = false;
        if (act == "delete") ok = store.remove(id);
        else if (act == "voice") ok = store.setVoice(id, req.q("v", "1") == "1");
        else return HttpResponse::text(404, "not found");
        return HttpResponse::json(std::string("{\"ok\":") + (ok ? "1" : "0") + "}", ok ? 200 : 404);
    }

    // Bulk delete: /api/recordings/delete?kind=other[&before=ms]
    if (p == "/api/recordings/delete" && post)
    {
        RecordingStore::Query q;
        q.kind = kindOf(req.q("kind", "other"));
        q.limit = 1000000;
        int total = 0;
        auto items = store.list(q, total);
        int64_t before = std::atoll(req.q("before", "0").c_str());
        int n = 0;
        for (auto& r : items)
            if (before == 0 || r.startMs < before) n += store.remove(r.id) ? 1 : 0;
        return HttpResponse::json("{\"ok\":1,\"deleted\":" + std::to_string(n) + "}");
    }

    return HttpResponse::text(404, "not found");
}
