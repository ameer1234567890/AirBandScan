// REST API + static web UI for AirBandScan.
#pragma once

#include "engine/config.h"
#include "engine/recording_store.h"
#include "engine/scanner.h"
#include "web/http_server.h"

#include <mutex>
#include <string>

struct App
{
    std::mutex cfgMtx;
    Config cfg;
    std::string cfgPath = "airband.conf";
    RecordingStore store;
    Scanner scanner{store};
    HttpServer http;

    std::mutex ctlMtx; // serialises start/stop/restart
    bool startScanner(std::string& err);
    void stopScanner();

    HttpResponse route(const HttpRequest& req);
};
