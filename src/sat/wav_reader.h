// Streaming WAV reader for offline decoding: PCM 8 / 16 / 24 / 32 bit and
// IEEE float, any channel count, converted to interleaved float [-1, 1].
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

class WavReader
{
public:
    ~WavReader() { close(); }
    bool open(const std::string& path, std::string& err);
    void close();

    int channels() const { return channels_; }
    int rate() const { return rate_; }
    uint64_t frames() const { return frames_; }
    // Reads up to maxFrames frames (channels() floats each); 0 at the end.
    int read(float* out, int maxFrames);

private:
    std::FILE* f_ = nullptr;
    int channels_ = 0, rate_ = 0, bits_ = 0;
    bool float_ = false;
    uint64_t frames_ = 0, left_ = 0;
};
