// Audio file output: 16-bit PCM WAV and MP3.
//
// MP3 uses LAME, loaded at run time (libmp3lame.so.0 / libmp3lame-0.dll /
// libmp3lame.dylib), so the program builds and runs without it; when the
// library is missing the `lame` or `ffmpeg` command line tool is tried.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool writeWav16(const std::string& path, const std::vector<int16_t>& pcm, int rate);
bool readWav16(const std::string& path, std::vector<int16_t>& pcm, int& rate);

// Short description of the MP3 backend ("libmp3lame", "lame cli", "none").
std::string mp3Backend();
bool mp3Available();
bool encodeMp3(const std::vector<int16_t>& pcm, int rate, int kbps,
               const std::string& outPath, std::string& err);
// Convert an existing WAV file.
bool wavToMp3(const std::string& wavPath, const std::string& mp3Path, int kbps, std::string& err);
