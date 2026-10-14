#include "engine/recording_store.h"

#include "audio/audio_files.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

bool validId(const std::string& id)
{
    // YYYYMMDD-HHMMSS-mmm-FFFFFFF[-n]
    if (id.size() < 20 || id.size() > 40) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

std::string dayDir(const std::string& id)
{
    // "20261014-..." -> "2026-10-14"
    return id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2);
}

int64_t nowMs()
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

void RecordingStore::setRoot(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(mtx_);
    root_ = dir;
}

std::string RecordingStore::root() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return root_;
}

std::string RecordingStore::makeId(int64_t startMs, double freqHz)
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::time_t t = (std::time_t)(startMs / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    char id[64];
    std::snprintf(id, sizeof(id), "%s-%03d-%07lld", ts, (int)(startMs % 1000),
                  (long long)std::llround(freqHz / 1000.0));
    std::string base = id;
    std::string out = base;
    for (int n = 1; fs::exists(fs::path(root_) / dayDir(out) / (out + ".meta")); ++n)
        out = base + "-" + std::to_string(n);
    return out;
}

std::string RecordingStore::path(const std::string& id, const char* ext) const
{
    if (!validId(id)) return "";
    std::lock_guard<std::mutex> lk(mtx_);
    return (fs::path(root_) / dayDir(id) / (id + ext)).string();
}

bool RecordingStore::writeMeta(const RecordingInfo& r)
{
    fs::path p = fs::path(root_) / dayDir(r.id) / (r.id + ".meta");
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    if (!f) return false;
    char b[64];
    f << "id=" << r.id << "\n";
    f << "start_ms=" << r.startMs << "\n";
    f << "duration_ms=" << r.durationMs << "\n";
    std::snprintf(b, sizeof(b), "%.1f", r.freqHz);       f << "freq_hz=" << b << "\n";
    std::snprintf(b, sizeof(b), "%.1f", r.peakDb);       f << "peak_db=" << b << "\n";
    std::snprintf(b, sizeof(b), "%.1f", r.levelDbfs);    f << "level_dbfs=" << b << "\n";
    f << "voice=" << (r.voice ? 1 : 0) << "\n";
    f << "manual=" << (r.manual ? 1 : 0) << "\n";
    std::snprintf(b, sizeof(b), "%.3f", r.score);        f << "score=" << b << "\n";
    std::snprintf(b, sizeof(b), "%.3f", r.voicedFrac);   f << "voiced_frac=" << b << "\n";
    std::snprintf(b, sizeof(b), "%.2f", r.levelSpreadDb); f << "level_spread_db=" << b << "\n";
    std::snprintf(b, sizeof(b), "%.3f", r.modDepth);     f << "mod_depth=" << b << "\n";
    f << "reason=" << r.reason << "\n";
    return (bool)f;
}

void RecordingStore::load()
{
    std::lock_guard<std::mutex> lk(mtx_);
    items_.clear();
    std::error_code ec;
    if (!fs::exists(root_, ec))
    {
        ++version_;
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root_, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file() || it->path().extension() != ".meta") continue;
        std::ifstream f(it->path());
        std::map<std::string, std::string> kv;
        std::string line;
        while (std::getline(f, line))
        {
            size_t eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        RecordingInfo r;
        r.id = kv["id"];
        if (!validId(r.id)) continue;
        auto num = [&](const char* k) { return std::atof(kv[k].c_str()); };
        r.startMs = std::atoll(kv["start_ms"].c_str());
        r.durationMs = (int)num("duration_ms");
        r.freqHz = num("freq_hz");
        r.peakDb = num("peak_db");
        r.levelDbfs = num("level_dbfs");
        r.voice = kv["voice"] == "1";
        r.manual = kv["manual"] == "1";
        r.score = num("score");
        r.voicedFrac = num("voiced_frac");
        r.levelSpreadDb = num("level_spread_db");
        r.modDepth = num("mod_depth");
        r.reason = kv["reason"];
        fs::path base = it->path().parent_path() / r.id;
        r.hasMp3 = fs::exists(base.string() + ".mp3");
        r.hasSpg = fs::exists(base.string() + ".spg");
        items_.push_back(std::move(r));
    }
    std::sort(items_.begin(), items_.end(),
              [](const RecordingInfo& a, const RecordingInfo& b) { return a.startMs > b.startMs; });
    ++version_;
    logWrite("[store] %d recordings indexed in %s", (int)items_.size(), root_.c_str());
}

void RecordingStore::add(const RecordingInfo& r)
{
    std::lock_guard<std::mutex> lk(mtx_);
    writeMeta(r);
    auto pos = std::find_if(items_.begin(), items_.end(),
                            [&](const RecordingInfo& x) { return x.startMs <= r.startMs; });
    items_.insert(pos, r);
    ++version_;
}

bool RecordingStore::get(const std::string& id, RecordingInfo& out)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& r : items_)
        if (r.id == id) { out = r; return true; }
    return false;
}

std::vector<RecordingInfo> RecordingStore::list(const Query& q, int& total)
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<RecordingInfo> out;
    total = 0;
    for (auto& r : items_)
    {
        if (q.kind == 1 && !r.voice) continue;
        if (q.kind == 2 && r.voice) continue;
        if (q.freqHz > 0 && std::fabs(r.freqHz - q.freqHz) > 3000.0) continue;
        if (q.sinceMs > 0 && r.startMs < q.sinceMs) continue;
        if (total >= q.offset && (int)out.size() < q.limit) out.push_back(r);
        ++total;
    }
    return out;
}

bool RecordingStore::removeLocked(const std::string& id)
{
    auto it = std::find_if(items_.begin(), items_.end(), [&](const RecordingInfo& r) { return r.id == id; });
    if (it == items_.end()) return false;
    fs::path dir = fs::path(root_) / dayDir(id);
    std::error_code ec;
    for (const char* ext : {".wav", ".mp3", ".spg", ".meta"})
        fs::remove(dir / (id + ext), ec);
    if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
    items_.erase(it);
    ++version_;
    return true;
}

bool RecordingStore::remove(const std::string& id)
{
    if (!validId(id)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return removeLocked(id);
}

bool RecordingStore::setVoice(const std::string& id, bool voice)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& r : items_)
        if (r.id == id)
        {
            r.voice = voice;
            r.manual = true;
            r.reason = voice ? "marked as voice by user" : "marked as not voice by user";
            writeMeta(r);
            ++version_;
            return true;
        }
    return false;
}

bool RecordingStore::ensureMp3(const std::string& id, int kbps, std::string& err)
{
    std::string wav = path(id, ".wav"), mp3 = path(id, ".mp3");
    if (wav.empty()) { err = "bad id"; return false; }
    if (fs::exists(mp3)) return true;
    if (!wavToMp3(wav, mp3, kbps, err)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& r : items_)
        if (r.id == id) r.hasMp3 = true;
    ++version_;
    return true;
}

int RecordingStore::cleanup(int retentionDays)
{
    if (retentionDays <= 0) return 0;
    int64_t cutoff = nowMs() - (int64_t)retentionDays * 86400000LL;
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> old;
    for (auto& r : items_)
        if (r.startMs < cutoff) old.push_back(r.id);
    for (auto& id : old) removeLocked(id);
    if (!old.empty()) logWrite("[store] retention: removed %d old recordings", (int)old.size());
    return (int)old.size();
}

void RecordingStore::counts(int& voice, int& other)
{
    std::lock_guard<std::mutex> lk(mtx_);
    voice = other = 0;
    for (auto& r : items_) (r.voice ? voice : other)++;
}

std::vector<std::pair<double, int>> RecordingStore::frequencies(int kind)
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::map<long long, int> m;
    for (auto& r : items_)
    {
        if (kind == 1 && !r.voice) continue;
        if (kind == 2 && r.voice) continue;
        m[std::llround(r.freqHz / 100.0)]++;
    }
    std::vector<std::pair<double, int>> out;
    for (auto& [k, n] : m) out.push_back({k * 100.0, n});
    return out;
}
