#include "sat/msumr.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kStripMs = 1231.4; // time between two 8-pixel strips
constexpr int kMaxGapStrips = 200;

const int kQTable[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,     12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,     14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,   24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

// Natural (row-major) position -> zigzag index.
const int kZigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28,    2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43,  9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

// JPEG standard luminance tables (ITU T.81 Annex K.3).
const uint8_t kDcBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kAcBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

struct Huffman
{
    int maxcode[17], valptr[17], mincode[17];
    const uint8_t* vals;
    Huffman(const uint8_t* bits, const uint8_t* v) : vals(v)
    {
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l)
        {
            valptr[l] = k;
            mincode[l] = code;
            code += bits[l - 1];
            k += bits[l - 1];
            maxcode[l] = bits[l - 1] ? code - 1 : -1;
            code <<= 1;
        }
    }
};
const Huffman kDc(kDcBits, kDcVals);
const Huffman kAc(kAcBits, kAcVals);

struct BitReader
{
    const uint8_t* d;
    size_t nbits, pos = 0;
    BitReader(const uint8_t* data, size_t len) : d(data), nbits(len * 8) {}
    int bit()
    {
        if (pos >= nbits) return -1;
        int b = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
        ++pos;
        return b;
    }
    // Next Huffman symbol, -1 at end of data / invalid code.
    int symbol(const Huffman& h)
    {
        int code = 0;
        for (int l = 1; l <= 16; ++l)
        {
            int b = bit();
            if (b < 0) return -1;
            code = (code << 1) | b;
            if (h.maxcode[l] >= 0 && code <= h.maxcode[l] && code >= h.mincode[l])
                return h.vals[h.valptr[l] + code - h.mincode[l]];
        }
        return -1;
    }
    // JPEG "receive + extend" of an n-bit magnitude; false at end of data.
    bool value(int n, int& v)
    {
        int x = 0;
        for (int i = 0; i < n; ++i)
        {
            int b = bit();
            if (b < 0) return false;
            x = (x << 1) | b;
        }
        v = (n && x < (1 << (n - 1))) ? x - (1 << n) + 1 : x;
        return true;
    }
};

struct Idct
{
    float t[8][8]; // t[x][u] = C(u)/2 cos((2x+1) u pi / 16)
    Idct()
    {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                t[x][u] = (float)((u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * M_PI / 16.0));
    }
    void run(const float* F, uint8_t* out, int stride) const
    {
        float tmp[64];
        for (int v = 0; v < 8; ++v)         // rows: horizontal transform
            for (int x = 0; x < 8; ++x)
            {
                float s = 0;
                for (int u = 0; u < 8; ++u) s += t[x][u] * F[v * 8 + u];
                tmp[v * 8 + x] = s;
            }
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                float s = 0;
                for (int v = 0; v < 8; ++v) s += t[y][v] * tmp[v * 8 + x];
                int p = (int)std::lround(s + 128.0f);
                out[y * stride + x] = (uint8_t)std::clamp(p, 0, 255);
            }
    }
};
const Idct kIdct;

void quantTable(int qf, float* q)
{
    // Meteor's quality factor scaling (differs from libjpeg below 20).
    float f = (qf >= 20 && qf < 50) ? 5000.0f / qf : 200.0f - 2.0f * qf;
    for (int i = 0; i < 64; ++i)
        q[i] = std::max(1.0f, std::floor(f / 100.0f * kQTable[i] + 0.5f));
}

} // namespace

void MsuMrImager::reset()
{
    lines_.clear();
    segments_ = 0;
    seen_ = {};
}

bool MsuMrImager::addPacket(int apid, const uint8_t* p, int len)
{
    if (apid < 64 || apid > 69 || len < 15) return false;
    int ch = apid - 64;
    int day = (p[0] << 8) | p[1];
    uint32_t ms = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
    int mcun = p[8], qt = p[9], dcac = p[10], qfm = (p[11] << 8) | p[12], qf = p[13];
    if (qt != 0 || dcac != 0 || qfm != 0xFFF0 || ms >= 86400000u || mcun > 182 || mcun % 14) return false;

    float q[64];
    quantTable(qf, q);
    BitReader br(p + 14, (size_t)(len - 14));
    uint8_t px[8 * 112];
    int blocks = 0, lastDc = 0;
    for (; blocks < 14; ++blocks)
    {
        int s = br.symbol(kDc), v = 0;
        if (s < 0 || !br.value(s, v)) break;
        lastDc += v;
        int zz[64] = {0};
        zz[0] = lastDc;
        bool ok = true;
        for (int k = 1; k < 64;)
        {
            int rs = br.symbol(kAc);
            if (rs < 0) { ok = false; break; }
            if (rs == 0x00) break;            // EOB
            int run = rs >> 4, size = rs & 15;
            k += run;
            if (size == 0) { k += (run == 15) ? 1 : 0; continue; } // ZRL
            if (k > 63 || !br.value(size, v)) { ok = false; break; }
            zz[k++] = v;
        }
        if (!ok) break;
        float F[64];
        for (int i = 0; i < 64; ++i) F[i] = (float)zz[kZigzag[i]] * q[i];
        kIdct.run(F, px + blocks * 8, 112);
    }
    if (blocks == 0) return false;

    int64_t key = (int64_t)day * 86400000LL + ms;
    Strip& st = lines_[key];
    auto& buf = st.px[(size_t)ch];
    if (!buf)
    {
        buf.reset(new uint8_t[8 * kWidth]);
        std::memset(buf.get(), 0, 8 * kWidth);
    }
    for (int y = 0; y < 8; ++y)
        std::memcpy(buf.get() + y * kWidth + mcun * 8, px + y * 112, (size_t)blocks * 8);
    seen_[(size_t)ch] = true;
    ++segments_;
    return true;
}

std::vector<std::pair<int64_t, int>> MsuMrImager::layout() const
{
    std::vector<std::pair<int64_t, int>> out;
    int row = 0;
    int64_t prev = 0;
    for (auto& kv : lines_)
    {
        if (!out.empty())
        {
            int gap = (int)std::lround((double)(kv.first - prev) / kStripMs);
            row += (gap < 1 || gap > kMaxGapStrips) ? 1 : gap;
        }
        out.push_back({kv.first, row});
        prev = kv.first;
    }
    return out;
}

int MsuMrImager::rows() const
{
    auto l = layout();
    return l.empty() ? 0 : (l.back().second + 1) * 8;
}

std::vector<int> MsuMrImager::channels() const
{
    std::vector<int> c;
    for (int i = 0; i < kChannels; ++i)
        if (seen_[(size_t)i]) c.push_back(i + 1);
    return c;
}

Image MsuMrImager::channel(int ch) const
{
    if (ch < 1 || ch > kChannels || !seen_[(size_t)(ch - 1)]) return {};
    auto l = layout();
    Image img(kWidth, (l.back().second + 1) * 8, 1);
    for (auto& [key, r] : l)
    {
        auto& buf = lines_.at(key).px[(size_t)(ch - 1)];
        if (buf) std::memcpy(img.row(r * 8), buf.get(), 8 * kWidth);
    }
    return img;
}

Image MsuMrImager::composite() const
{
    if (seen_[0] && seen_[1])
    {
        Image a = channel(2), b = channel(1);
        Image rgb(a.w, a.h, 3);
        for (size_t i = 0; i < a.px.size(); ++i)
        {
            rgb.px[i * 3] = a.px[i];
            rgb.px[i * 3 + 1] = a.px[i];
            rgb.px[i * 3 + 2] = b.px[i];
        }
        return rgb;
    }
    for (int c : {1, 2, 3, 4, 5, 6})
        if (seen_[(size_t)(c - 1)]) return channel(c);
    return {};
}
