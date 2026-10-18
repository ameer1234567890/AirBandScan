// 8-bit grey / RGB raster and PNG output (stb_image_write).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Image
{
    int w = 0, h = 0, ch = 1; // ch: 1 = grey, 3 = RGB
    std::vector<uint8_t> px;

    Image() = default;
    Image(int w_, int h_, int ch_, uint8_t fill = 0) : w(w_), h(h_), ch(ch_), px((size_t)w_ * h_ * ch_, fill) {}
    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t* row(int y) { return px.data() + (size_t)y * w * ch; }
    const uint8_t* row(int y) const { return px.data() + (size_t)y * w * ch; }
};

bool writePng(const std::string& path, const Image& img);
std::string encodePng(const Image& img);

// Box-filter downscale to the given width (keeps the aspect ratio).
Image scaleToWidth(const Image& src, int w);
