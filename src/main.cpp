// AirBandScan — automatic VHF airband scanner and voice recorder with a web interface.
//
// Headless: scans the configured VHF airband ranges, records every AM voice
// transmission to WAV + MP3, and serves a browser UI (recordings, spectrum,
// undecoded signals, settings). Runs on Windows and Linux.

#include "audio/audio_files.h"
#include "util/log.h"
#include "version.h"
#include "web/api.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static std::atomic<bool> g_quit{false};

static void onSignal(int) { g_quit = true; }

static void usage()
{
    std::printf(
        "AirBandScan %s - automatic VHF airband scanner\n\n"
        "usage: airbandscan [options]\n"
        "  -c, --config FILE   configuration file (default: airband.conf)\n"
        "  -p, --port N        web interface port (overrides config)\n"
        "  -d, --dir DIR       recordings directory (overrides config)\n"
        "      --no-autostart  start with the scanner stopped\n"
        "  -h, --help          this help\n",
        AIRBANDSCAN_VERSION);
}

int main(int argc, char** argv)
{
    App app;
    int portOverride = 0;
    std::string dirOverride;
    bool noAutostart = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "-c" || a == "--config") app.cfgPath = next();
        else if (a == "-p" || a == "--port") portOverride = std::atoi(next().c_str());
        else if (a == "-d" || a == "--dir") dirOverride = next();
        else if (a == "--no-autostart") noAutostart = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
    }

    std::string err;
    if (fs::exists(app.cfgPath))
    {
        if (!app.cfg.load(app.cfgPath, err))
        {
            std::fprintf(stderr, "config error in %s: %s\n", app.cfgPath.c_str(), err.c_str());
            return 1;
        }
    }
    else
    {
        app.cfg.save(app.cfgPath);
        logWrite("[main] wrote default configuration to %s", app.cfgPath.c_str());
    }
    if (portOverride > 0) app.cfg.httpPort = portOverride;
    if (!dirOverride.empty()) app.cfg.recordingsDir = dirOverride;

    logWrite("[main] AirBandScan %s, MP3 encoder: %s", AIRBANDSCAN_VERSION, mp3Backend().c_str());

    app.store.setRoot(app.cfg.recordingsDir);
    app.store.load();
    app.store.cleanup(app.cfg.retentionDays);

    if (!app.http.start(app.cfg.httpBind, app.cfg.httpPort,
                        [&app](const HttpRequest& r) { return app.route(r); }, err))
    {
        std::fprintf(stderr, "web server: %s\n", err.c_str());
        return 1;
    }
    logWrite("[main] web interface: http://localhost:%d/", app.cfg.httpPort);

    if (app.cfg.autostart && !noAutostart)
    {
        if (!app.startScanner(err))
            logWrite("[main] scanner not started: %s (fix it in Settings)", err.c_str());
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto lastCleanup = std::chrono::steady_clock::now();
    while (!g_quit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (std::chrono::steady_clock::now() - lastCleanup > std::chrono::hours(1))
        {
            lastCleanup = std::chrono::steady_clock::now();
            int days;
            {
                std::lock_guard<std::mutex> lk(app.cfgMtx);
                days = app.cfg.retentionDays;
            }
            app.store.cleanup(days);
        }
    }

    logWrite("[main] shutting down");
    app.stopScanner();
    app.http.stop();
    return 0;
}
