#include "sat/wav_reader.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
}

bool WavReader::open(const std::string& path, std::string& err)
{
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) { err = "cannot open " + path; return false; }
    uint8_t h[12];
    if (std::fread(h, 1, 12, f_) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4))
    {
        err = "not a WAV file";
        close();
        return false;
    }
    bool fmt = false;
    for (;;)
    {
        uint8_t c[8];
        if (std::fread(c, 1, 8, f_) != 8) break;
        uint32_t len = le32(c + 4);
        if (!std::memcmp(c, "fmt ", 4))
        {
            std::vector<uint8_t> b(len);
            if (len < 16 || std::fread(b.data(), 1, len, f_) != len) break;
            int tag = le16(&b[0]);
            channels_ = le16(&b[2]);
            rate_ = (int)le32(&b[4]);
            bits_ = le16(&b[14]);
            if (tag == 0xFFFE && len >= 26) tag = le16(&b[24]); // WAVE_FORMAT_EXTENSIBLE
            float_ = tag == 3;
            fmt = (tag == 1 || tag == 3) && channels_ > 0 &&
                  (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32);
            if (len & 1) std::fgetc(f_);
        }
        else if (!std::memcmp(c, "data", 4))
        {
            if (!fmt) break;
            uint64_t bytes = len;
            if (len == 0 || len == 0xFFFFFFFFu) // streaming writers leave it unset
            {
                long pos = std::ftell(f_);
                std::fseek(f_, 0, SEEK_END);
                bytes = (uint64_t)(std::ftell(f_) - pos);
                std::fseek(f_, pos, SEEK_SET);
            }
            frames_ = left_ = bytes / ((uint64_t)channels_ * (bits_ / 8));
            return true;
        }
        else
            std::fseek(f_, (long)(len + (len & 1)), SEEK_CUR);
    }
    err = "unsupported WAV format (PCM 8/16/24/32 bit or float expected)";
    close();
    return false;
}

void WavReader::close()
{
    if (f_) std::fclose(f_);
    f_ = nullptr;
    frames_ = left_ = 0;
}

int WavReader::read(float* out, int maxFrames)
{
    if (!f_ || left_ == 0 || maxFrames <= 0) return 0;
    int n = (int)std::min<uint64_t>(left_, (uint64_t)maxFrames);
    int bps = bits_ / 8;
    std::vector<uint8_t> b((size_t)n * channels_ * bps);
    size_t got = std::fread(b.data(), 1, b.size(), f_) / ((size_t)channels_ * bps);
    n = (int)got;
    left_ = got ? left_ - got : 0;
    size_t cnt = (size_t)n * channels_;
    for (size_t i = 0; i < cnt; ++i)
    {
        const uint8_t* p = &b[i * bps];
        float v;
        if (float_) { float t; std::memcpy(&t, p, 4); v = t; }
        else if (bps == 1) v = (p[0] - 127.5f) / 128.f;
        else if (bps == 2) v = (int16_t)le16(p) / 32768.f;
        else if (bps == 3) v = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.f;
        else v = (float)(int32_t)le32(p) / 2147483648.f;
        out[i] = v;
    }
    return n;
}
