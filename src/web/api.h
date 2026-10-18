// REST API + static web UI for AirBandScan.
#pragma once

#include "engine/config.h"
#include "engine/recording_store.h"
#include "engine/scanner.h"
#include "sat/image_store.h"
#include "sat/sat_receiver.h"
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
    ImageStore images;
    SatReceiver sat{images};
    HttpServer http;

    // One SDR: either the airband scanner or the satellite receiver runs.
    std::mutex ctlMtx; // serialises start/stop/restart
    bool startActive(std::string& err);  // whatever cfg.runMode says
    bool startScanner(std::string& err);
    bool startSat(std::string& err);
    void stopAll();
    void stopScanner() { stopAll(); }
    void setStoreRoots(const Config& c);

    HttpResponse route(const HttpRequest& req);
    HttpResponse routeSat(const HttpRequest& req);
    std::string runMode();
};
