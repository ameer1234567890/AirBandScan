#include "sat/image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstdio>

namespace {
void append(void* ctx, void* data, int size)
{
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), (size_t)size);
}
}

std::string encodePng(const Image& img)
{
    std::string out;
    if (img.empty()) return out;
    stbi_write_png_compression_level = 6;
    stbi_write_png_to_func(append, &out, img.w, img.h, img.ch, img.px.data(), img.w * img.ch);
    return out;
}

bool writePng(const std::string& path, const Image& img)
{
    std::string data = encodePng(img);
    if (data.empty()) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

Image scaleToWidth(const Image& src, int w)
{
    if (src.empty() || w >= src.w) return src;
    int h = std::max(1, (int)((int64_t)src.h * w / src.w));
    Image out(w, h, src.ch);
    for (int y = 0; y < h; ++y)
    {
        int y0 = (int)((int64_t)y * src.h / h), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * src.h / h));
        for (int x = 0; x < w; ++x)
        {
            int x0 = (int)((int64_t)x * src.w / w), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * src.w / w));
            for (int c = 0; c < src.ch; ++c)
            {
                unsigned sum = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) sum += src.row(yy)[xx * src.ch + c];
                out.row(y)[x * src.ch + c] = (uint8_t)(sum / (unsigned)((y1 - y0) * (x1 - x0)));
            }
        }
    }
    return out;
}
