// Runtime configuration: a flat key = value file (airband.conf) that the web
// Settings page reads and writes. Every field has a sensible airband default.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

struct FreqRange
{
    double lo = 0.0; // Hz
    double hi = 0.0; // Hz
    bool contains(double f) const { return f >= lo && f <= hi; }
};

struct Config
{
    // ---- SDR device ----
    std::string device = "rtlsdr"; // rtlsdr | rtltcp | airspy | airspyhf | sdrplay | hackrf | pluto | wav
    int    deviceIndex = 0;
    double sampleRate = 2.4e6;
    double gainDb = 40.0;          // < 0 = automatic gain
    double ppm = 0.0;
    bool   biasTee = false;
    std::string rtlTcpAddress = "127.0.0.1:1234"; // rtl_tcp server host:port
    std::string plutoUri;          // PlutoSDR: "ip:192.168.2.1", "usb:1.4.5"; empty = auto
    std::string wavPath;           // IQ WAV file (device = wav)
    double wavCenterHz = 120.0e6;  // what the IQ file was centred on

    // ---- Scanning (rtl-sdr-scanner-cpp style) ----
    std::vector<FreqRange> ranges{{118.0e6, 137.0e6}};
    // Data channels (ACARS / VDL2) are never recorded: this is a voice recorder.
    std::vector<FreqRange> ignored{
        {131.523e6, 131.527e6},   // ACARS (Europe)
        {131.548e6, 131.552e6},   // ACARS primary
        {131.723e6, 131.727e6},   // ACARS
        {131.823e6, 131.827e6},   // ACARS
        {136.700e6, 136.975e6},   // VDL2 / data
    };
    double channelStepHz = 25000.0 / 3.0; // snap to the 8.33 kHz airband raster
    double channelBwHz   = 8000.0;        // AM channel filter (total width)
    double startLevelDb  = 6.0;           // open a transmission (dB above noise)
    double stopLevelDb   = 3.0;           // keep it alive while above this
    int    dwellMs       = 500;           // time on each range when idle
    int    timeoutMs     = 1200;          // silence that ends a transmission
    int    minDurationMs = 500;           // shorter bursts are discarded
    int    maxDurationS  = 120;           // split very long transmissions
    int    prerollMs     = 300;           // audio kept from before detection
    int    noiseLearnMs  = 1500;          // initial noise learning per range

    // ---- Voice classification ----
    double voiceThreshold = 0.50; // 0..1 score needed to count as voice

    // ---- Output ----
    std::string recordingsDir = "recordings";
    bool   mp3Auto = true;   // encode an MP3 next to each voice WAV
    int    mp3Bitrate = 32;  // kbps (8 kHz mono -> MPEG-2.5)
    bool   keepOther = true; // keep audio + spectrogram of non-voice signals
    int    retentionDays = 0; // 0 = keep forever

    // ---- Web ----
    std::string httpBind = "0.0.0.0";
    int    httpPort = 8080;
    bool   autostart = true;

    // Serialisation.
    std::map<std::string, std::string> toMap() const;
    // Applies recognised keys; returns a human readable error for bad values.
    bool fromMap(const std::map<std::string, std::string>& kv, std::string& err);

    bool load(const std::string& path, std::string& err);
    bool save(const std::string& path) const;
};

std::string formatRanges(const std::vector<FreqRange>& r);
bool parseRanges(const std::string& s, std::vector<FreqRange>& out, std::string& err);
