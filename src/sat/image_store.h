// Archive of decoded satellite images (SSTV pictures, Meteor LRPT passes).
//
//   <dir>/YYYY-MM-DD/<id>.png          main image (SSTV picture / LRPT composite)
//   <dir>/YYYY-MM-DD/<id>_thumb.png    small preview for the web gallery
//   <dir>/YYYY-MM-DD/<id>_chN.png      LRPT: each MSU-MR channel
//   <dir>/YYYY-MM-DD/<id>.imeta        key = value metadata
#pragma once

#include "sat/image.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ImageInfo
{
    std::string id;
    std::string kind;         // "sstv" | "lrpt"
    std::string mode;         // "PD120", "Meteor-M LRPT", ...
    int64_t startMs = 0, endMs = 0;
    double freqHz = 0;
    int w = 0, h = 0;
    bool complete = true;
    std::vector<std::string> files; // extra suffixes, e.g. "_ch1"
    std::string note;
};

class ImageStore
{
public:
    void setRoot(const std::string& dir);
    std::string root() const;
    void load();

    // Writes <id>.png (+ thumbnail) and the extra images ("_ch1", ...) and
    // indexes it. Fills info.id. False if nothing could be written.
    bool add(ImageInfo& info, const Image& main, const std::vector<std::pair<std::string, Image>>& extra);

    std::vector<ImageInfo> list(const std::string& kind, int offset, int limit, int& total);
    // Path of "<id><suffix>.png"; empty when the name is not one of ours.
    std::string file(const std::string& name) const;
    bool remove(const std::string& id);
    uint64_t version() const { return version_; }
    int count();

private:
    std::string makeId(int64_t ms, const std::string& kind);

    mutable std::mutex mtx_;
    std::string root_ = "recordings/images";
    std::vector<ImageInfo> items_; // newest first
    uint64_t version_ = 1;
};
