#include "sat/image_store.h"

#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool validId(const std::string& id)
{
    // YYYYMMDD-HHMMSS-<kind>[-n]
    if (id.size() < 20 || id.size() > 40) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-')) return false;
    return id[8] == '-' && id[15] == '-';
}

std::string dayDir(const std::string& id)
{
    return id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2);
}

bool validSuffix(const std::string& s)
{
    if (s.empty() || s == "_thumb") return true;
    return s.size() == 4 && s.compare(0, 3, "_ch") == 0 && s[3] >= '1' && s[3] <= '6';
}

} // namespace

void ImageStore::setRoot(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(mtx_);
    root_ = dir;
}

std::string ImageStore::root() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return root_;
}

std::string ImageStore::makeId(int64_t ms, const std::string& kind)
{
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    std::string base = std::string(ts) + "-" + kind, out = base;
    for (int n = 1; fs::exists(fs::path(root_) / dayDir(out) / (out + ".imeta")); ++n)
        out = base + "-" + std::to_string(n);
    return out;
}

void ImageStore::load()
{
    std::lock_guard<std::mutex> lk(mtx_);
    items_.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file() || it->path().extension() != ".imeta") continue;
        std::ifstream f(it->path());
        std::map<std::string, std::string> kv;
        std::string line;
        while (std::getline(f, line))
        {
            size_t eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        ImageInfo r;
        r.id = kv["id"];
        if (!validId(r.id)) continue;
        r.kind = kv["kind"];
        r.mode = kv["mode"];
        r.startMs = std::atoll(kv["start_ms"].c_str());
        r.endMs = std::atoll(kv["end_ms"].c_str());
        r.freqHz = std::atof(kv["freq_hz"].c_str());
        r.w = std::atoi(kv["width"].c_str());
        r.h = std::atoi(kv["height"].c_str());
        r.complete = kv["complete"] != "0";
        r.note = kv["note"];
        std::istringstream fl(kv["files"]);
        std::string s;
        while (fl >> s)
            if (validSuffix(s)) r.files.push_back(s);
        items_.push_back(r);
    }
    std::sort(items_.begin(), items_.end(), [](const ImageInfo& a, const ImageInfo& b) { return a.startMs > b.startMs; });
    ++version_;
}

bool ImageStore::add(ImageInfo& info, const Image& main, const std::vector<std::pair<std::string, Image>>& extra)
{
    std::lock_guard<std::mutex> lk(mtx_);
    info.id = makeId(info.startMs, info.kind);
    fs::path dir = fs::path(root_) / dayDir(info.id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string base = (dir / info.id).string();
    if (!writePng(base + ".png", main))
    {
        logWrite("[images] cannot write %s.png", base.c_str());
        return false;
    }
    writePng(base + "_thumb.png", scaleToWidth(main, 480));
    info.w = main.w;
    info.h = main.h;
    info.files.clear();
    for (auto& [suffix, img] : extra)
        if (validSuffix(suffix) && writePng(base + suffix + ".png", img)) info.files.push_back(suffix);

    std::ofstream m(base + ".imeta");
    m << "id=" << info.id << "\nkind=" << info.kind << "\nmode=" << info.mode << "\nstart_ms=" << info.startMs
      << "\nend_ms=" << info.endMs << "\nfreq_hz=" << (long long)info.freqHz << "\nwidth=" << info.w
      << "\nheight=" << info.h << "\ncomplete=" << (info.complete ? 1 : 0) << "\nnote=" << info.note << "\nfiles=";
    for (size_t i = 0; i < info.files.size(); ++i) m << (i ? " " : "") << info.files[i];
    m << "\n";
    items_.insert(items_.begin(), info);
    ++version_;
    return true;
}

std::vector<ImageInfo> ImageStore::list(const std::string& kind, int offset, int limit, int& total)
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ImageInfo> out;
    total = 0;
    for (auto& r : items_)
    {
        if (!kind.empty() && r.kind != kind) continue;
        if (total >= offset && (int)out.size() < limit) out.push_back(r);
        ++total;
    }
    return out;
}

int ImageStore::count()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return (int)items_.size();
}

std::string ImageStore::file(const std::string& name) const
{
    // name = <id><suffix>.png
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) return "";
    std::string stem = name.substr(0, name.size() - 4);
    std::string id = stem, suffix;
    size_t us = stem.find('_');
    if (us != std::string::npos)
    {
        id = stem.substr(0, us);
        suffix = stem.substr(us);
    }
    if (!validId(id) || !validSuffix(suffix)) return "";
    std::lock_guard<std::mutex> lk(mtx_);
    return (fs::path(root_) / dayDir(id) / name).string();
}

bool ImageStore::remove(const std::string& id)
{
    if (!validId(id)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = std::find_if(items_.begin(), items_.end(), [&](const ImageInfo& r) { return r.id == id; });
    if (it == items_.end()) return false;
    fs::path dir = fs::path(root_) / dayDir(id);
    std::error_code ec;
    fs::remove(dir / (id + ".png"), ec);
    fs::remove(dir / (id + "_thumb.png"), ec);
    for (auto& s : it->files) fs::remove(dir / (id + s + ".png"), ec);
    fs::remove(dir / (id + ".imeta"), ec);
    items_.erase(it);
    ++version_;
    return true;
}
