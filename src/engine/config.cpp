#include "engine/config.h"

#include "sdr/rtl_tcp_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string num(double v, int prec = 6)
{
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    // Strip trailing zeros for readability ("118.000000" -> "118").
    std::string s(b);
    if (s.find('.') != std::string::npos)
    {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

bool toDouble(const std::string& s, double& out)
{
    char* end = nullptr;
    std::string t = trim(s);
    if (t.empty()) return false;
    out = std::strtod(t.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

bool toBool(const std::string& s)
{
    std::string t = trim(s);
    return t == "1" || t == "true" || t == "on" || t == "yes";
}

} // namespace

// "118-121.5, 131.550" (MHz). A single value means that channel +-2 kHz.
bool parseRanges(const std::string& s, std::vector<FreqRange>& out, std::string& err)
{
    out.clear();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item = trim(item);
        if (item.empty()) continue;
        size_t dash = item.find('-', 1);
        double a = 0, b = 0;
        if (dash == std::string::npos)
        {
            if (!toDouble(item, a)) { err = "bad frequency: " + item; return false; }
            out.push_back({a * 1e6 - 2000.0, a * 1e6 + 2000.0});
        }
        else
        {
            if (!toDouble(item.substr(0, dash), a) || !toDouble(item.substr(dash + 1), b))
            {
                err = "bad range: " + item;
                return false;
            }
            if (b < a) std::swap(a, b);
            out.push_back({a * 1e6, b * 1e6});
        }
    }
    return true;
}

std::string formatRanges(const std::vector<FreqRange>& r)
{
    std::string s;
    for (auto& x : r)
    {
        if (!s.empty()) s += ", ";
        double w = x.hi - x.lo;
        if (std::fabs(w - 4000.0) < 1.0) // single channel form
            s += num((x.lo + 2000.0) / 1e6, 4);
        else
            s += num(x.lo / 1e6, 4) + "-" + num(x.hi / 1e6, 4);
    }
    return s;
}

std::map<std::string, std::string> Config::toMap() const
{
    std::map<std::string, std::string> m;
    m["device"] = device;
    m["device_index"] = std::to_string(deviceIndex);
    m["sample_rate"] = num(sampleRate, 0);
    m["gain_db"] = num(gainDb, 1);
    m["ppm"] = num(ppm, 1);
    m["bias_tee"] = biasTee ? "1" : "0";
    m["wav_path"] = wavPath;
    m["pluto_uri"] = plutoUri;
    m["rtltcp_address"] = rtlTcpAddress;
    m["wav_center_mhz"] = num(wavCenterHz / 1e6);
    m["ranges"] = formatRanges(ranges);
    m["ignored"] = formatRanges(ignored);
    m["channel_step_khz"] = num(channelStepHz / 1000.0, 6);
    m["modulation"] = modulation;
    m["channel_bw_khz"] = num(channelBwHz / 1000.0, 2);
    m["start_level_db"] = num(startLevelDb, 1);
    m["stop_level_db"] = num(stopLevelDb, 1);
    m["dwell_ms"] = std::to_string(dwellMs);
    m["timeout_ms"] = std::to_string(timeoutMs);
    m["min_duration_ms"] = std::to_string(minDurationMs);
    m["max_duration_s"] = std::to_string(maxDurationS);
    m["preroll_ms"] = std::to_string(prerollMs);
    m["noise_learn_ms"] = std::to_string(noiseLearnMs);
    m["voice_threshold"] = num(voiceThreshold, 2);
    m["recordings_dir"] = recordingsDir;
    m["mp3_auto"] = mp3Auto ? "1" : "0";
    m["mp3_bitrate"] = std::to_string(mp3Bitrate);
    m["keep_other"] = keepOther ? "1" : "0";
    m["retention_days"] = std::to_string(retentionDays);
    m["http_bind"] = httpBind;
    m["http_port"] = std::to_string(httpPort);
    m["autostart"] = autostart ? "1" : "0";
    return m;
}

bool Config::fromMap(const std::map<std::string, std::string>& kv, std::string& err)
{
    Config c = *this; // validate into a copy, commit only on success
    for (auto& [key, raw] : kv)
    {
        std::string v = trim(raw);
        double d = 0;
        auto needNum = [&](double lo, double hi) -> bool {
            if (!toDouble(v, d) || d < lo || d > hi)
            {
                err = key + ": expected a number between " + num(lo) + " and " + num(hi);
                return false;
            }
            return true;
        };

        if (key == "device")
        {
            if (v != "rtlsdr" && v != "rtltcp" && v != "airspy" && v != "airspyhf" && v != "sdrplay" && v != "hackrf" && v != "pluto" && v != "wav"
#if defined(AIRBANDSCAN_SIM)
                && v != "sim"
#endif
            )
            {
                err = "device: unknown type " + v;
                return false;
            }
            c.device = v;
        }
        else if (key == "device_index") { if (!needNum(0, 64)) return false; c.deviceIndex = (int)d; }
        else if (key == "sample_rate") { if (!needNum(2.0e5, 2.0e7)) return false; c.sampleRate = d; }
        else if (key == "gain_db") { if (!needNum(-1, 100)) return false; c.gainDb = d; }
        else if (key == "ppm") { if (!needNum(-200, 200)) return false; c.ppm = d; }
        else if (key == "bias_tee") c.biasTee = toBool(v);
        else if (key == "wav_path") c.wavPath = v;
        else if (key == "rtltcp_address")
        {
            std::string h;
            int port = 0;
            if (!RtlTcpSource::parseAddress(v, h, port))
            {
                err = "rtltcp_address: expected host:port, e.g. 192.168.1.20:1234";
                return false;
            }
            c.rtlTcpAddress = v;
        }
        else if (key == "pluto_uri")
        {
            if (v.size() > 200 || v.find_first_of(" \t\r\n") != std::string::npos)
            {
                err = "pluto_uri: expected an address such as ip:192.168.2.1";
                return false;
            }
            c.plutoUri = v;
        }
        else if (key == "wav_center_mhz") { if (!needNum(0.01, 7000)) return false; c.wavCenterHz = d * 1e6; }
        else if (key == "ranges")
        {
            if (!parseRanges(v, c.ranges, err)) return false;
            if (c.ranges.empty()) { err = "ranges: at least one range is required"; return false; }
        }
        else if (key == "ignored") { if (!parseRanges(v, c.ignored, err)) return false; }
        else if (key == "channel_step_khz")
        {
            if (!needNum(0, 100)) return false;
            // "8.33" / "8.333" mean the exact 25/3 kHz airband raster.
            c.channelStepHz = std::fabs(d - 25.0 / 3.0) < 0.01 ? 25000.0 / 3.0 : d * 1000.0;
        }
        else if (key == "modulation")
        {
            std::string m = v == "fm" ? "nfm" : v;
            if (m != "am" && m != "nfm")
            {
                err = "modulation: expected am or nfm";
                return false;
            }
            c.modulation = m;
        }
        else if (key == "channel_bw_khz") { if (!needNum(3, 25)) return false; c.channelBwHz = d * 1000.0; }
        else if (key == "start_level_db") { if (!needNum(1, 60)) return false; c.startLevelDb = d; }
        else if (key == "stop_level_db") { if (!needNum(0.5, 60)) return false; c.stopLevelDb = d; }
        else if (key == "dwell_ms") { if (!needNum(100, 60000)) return false; c.dwellMs = (int)d; }
        else if (key == "timeout_ms") { if (!needNum(200, 30000)) return false; c.timeoutMs = (int)d; }
        else if (key == "min_duration_ms") { if (!needNum(0, 60000)) return false; c.minDurationMs = (int)d; }
        else if (key == "max_duration_s") { if (!needNum(5, 3600)) return false; c.maxDurationS = (int)d; }
        else if (key == "preroll_ms") { if (!needNum(0, 2000)) return false; c.prerollMs = (int)d; }
        else if (key == "noise_learn_ms") { if (!needNum(200, 30000)) return false; c.noiseLearnMs = (int)d; }
        else if (key == "voice_threshold") { if (!needNum(0, 1)) return false; c.voiceThreshold = d; }
        else if (key == "recordings_dir") { if (v.empty()) { err = "recordings_dir: empty"; return false; } c.recordingsDir = v; }
        else if (key == "mp3_auto") c.mp3Auto = toBool(v);
        else if (key == "mp3_bitrate") { if (!needNum(8, 64)) return false; c.mp3Bitrate = (int)d; }
        else if (key == "keep_other") c.keepOther = toBool(v);
        else if (key == "retention_days") { if (!needNum(0, 3650)) return false; c.retentionDays = (int)d; }
        else if (key == "http_bind") c.httpBind = v;
        else if (key == "http_port") { if (!needNum(1, 65535)) return false; c.httpPort = (int)d; }
        else if (key == "autostart") c.autostart = toBool(v);
        // Unknown keys are ignored so old/new config files stay compatible.
    }
    if (c.stopLevelDb > c.startLevelDb)
    {
        err = "stop_level_db must not exceed start_level_db";
        return false;
    }
    *this = c;
    return true;
}

bool Config::load(const std::string& path, std::string& err)
{
    std::ifstream f(path);
    if (!f) { err = "cannot open " + path; return false; }
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return fromMap(kv, err);
}

bool Config::save(const std::string& path) const
{
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# AirBandScan configuration (edited by the web UI)\n"
         "# Frequencies in MHz; ranges as lo-hi, comma separated.\n";
    for (auto& [k, v] : toMap())
        f << k << " = " << v << "\n";
    return (bool)f;
}
