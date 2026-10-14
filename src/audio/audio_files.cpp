#include "audio/audio_files.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------
static void put32(unsigned char* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t get32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t get16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool writeWav16(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4); put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)rate * 2); put16(h + 32, 2); put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4); put32(h + 40, dataBytes);
    bool ok = std::fwrite(h, 1, 44, f) == 44;
    // Samples are little-endian on every platform we target.
    if (ok && !pcm.empty())
        ok = std::fwrite(pcm.data(), 2, pcm.size(), f) == pcm.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

bool readWav16(const std::string& path, std::vector<int16_t>& pcm, int& rate)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[12];
    bool ok = std::fread(h, 1, 12, f) == 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4);
    int ch = 0, bits = 0;
    while (ok)
    {
        unsigned char c[8];
        if (std::fread(c, 1, 8, f) != 8) { ok = false; break; }
        uint32_t sz = get32(c + 4);
        if (!std::memcmp(c, "fmt ", 4))
        {
            unsigned char fmt[64] = {};
            size_t rd = sz < sizeof(fmt) ? sz : sizeof(fmt);
            if (std::fread(fmt, 1, rd, f) != rd) { ok = false; break; }
            if (sz > rd) std::fseek(f, (long)(sz - rd), SEEK_CUR);
            ch = get16(fmt + 2); rate = (int)get32(fmt + 4); bits = get16(fmt + 14);
        }
        else if (!std::memcmp(c, "data", 4))
        {
            if (ch != 1 || bits != 16) { ok = false; break; }
            pcm.resize(sz / 2);
            ok = std::fread(pcm.data(), 2, pcm.size(), f) == pcm.size();
            break;
        }
        else
            std::fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    std::fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// LAME, resolved at run time
// ---------------------------------------------------------------------------
namespace {

typedef void* lame_t;
struct Lame
{
    lame_t (*init)();
    int (*set_in_samplerate)(lame_t, int);
    int (*set_out_samplerate)(lame_t, int);
    int (*set_num_channels)(lame_t, int);
    int (*set_mode)(lame_t, int);
    int (*set_brate)(lame_t, int);
    int (*set_quality)(lame_t, int);
    int (*init_params)(lame_t);
    int (*encode_buffer)(lame_t, const short*, const short*, int, unsigned char*, int);
    int (*encode_flush)(lame_t, unsigned char*, int);
    int (*close)(lame_t);
    bool ok = false;
};

std::once_flag g_once;
Lame g_lame;
std::string g_backend = "none";

void* sym(void* lib, const char* name)
{
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

bool haveTool(const char* probe)
{
#if defined(_WIN32)
    std::string cmd = std::string(probe) + " >NUL 2>&1";
#else
    std::string cmd = std::string(probe) + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

void loadLame()
{
#if defined(_WIN32)
    const char* names[] = {"libmp3lame-0.dll", "libmp3lame.dll", "mp3lame.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"libmp3lame.0.dylib", "libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.dylib"};
#else
    const char* names[] = {"libmp3lame.so.0", "libmp3lame.so", "libmp3lame.so.0.0.0"};
#endif
    void* lib = nullptr;
    for (const char* n : names)
    {
#if defined(_WIN32)
        lib = (void*)LoadLibraryA(n);
#else
        lib = dlopen(n, RTLD_NOW | RTLD_LOCAL);
#endif
        if (lib) break;
    }
    if (lib)
    {
        Lame& L = g_lame;
        *(void**)&L.init = sym(lib, "lame_init");
        *(void**)&L.set_in_samplerate = sym(lib, "lame_set_in_samplerate");
        *(void**)&L.set_out_samplerate = sym(lib, "lame_set_out_samplerate");
        *(void**)&L.set_num_channels = sym(lib, "lame_set_num_channels");
        *(void**)&L.set_mode = sym(lib, "lame_set_mode");
        *(void**)&L.set_brate = sym(lib, "lame_set_brate");
        *(void**)&L.set_quality = sym(lib, "lame_set_quality");
        *(void**)&L.init_params = sym(lib, "lame_init_params");
        *(void**)&L.encode_buffer = sym(lib, "lame_encode_buffer");
        *(void**)&L.encode_flush = sym(lib, "lame_encode_flush");
        *(void**)&L.close = sym(lib, "lame_close");
        L.ok = L.init && L.set_in_samplerate && L.set_out_samplerate && L.set_num_channels &&
               L.set_mode && L.set_brate && L.set_quality && L.init_params &&
               L.encode_buffer && L.encode_flush && L.close;
        if (L.ok)
        {
            g_backend = "libmp3lame";
            return;
        }
    }
    if (haveTool("lame --version"))
        g_backend = "lame";
    else if (haveTool("ffmpeg -version"))
        g_backend = "ffmpeg";
}

bool runTool(const std::string& wav, const std::string& mp3, int kbps, std::string& err)
{
    std::string q = "\"";
    std::string cmd;
    if (g_backend == "lame")
        cmd = "lame --quiet -m m -b " + std::to_string(kbps) + " " + q + wav + q + " " + q + mp3 + q;
    else
        cmd = "ffmpeg -nostdin -loglevel error -y -i " + q + wav + q +
              " -codec:a libmp3lame -b:a " + std::to_string(kbps) + "k " + q + mp3 + q;
    if (std::system(cmd.c_str()) != 0 || !fs::exists(mp3))
    {
        err = g_backend + " failed";
        return false;
    }
    return true;
}

} // namespace

std::string mp3Backend()
{
    std::call_once(g_once, loadLame);
    return g_backend;
}

bool mp3Available() { return mp3Backend() != "none"; }

bool encodeMp3(const std::vector<int16_t>& pcm, int rate, int kbps,
               const std::string& outPath, std::string& err)
{
    std::string backend = mp3Backend();
    if (backend == "none")
    {
        err = "no MP3 encoder (install LAME: libmp3lame)";
        return false;
    }
    if (backend != "libmp3lame")
    {
        std::string tmp = outPath + ".tmp.wav";
        if (!writeWav16(tmp, pcm, rate)) { err = "cannot write temp wav"; return false; }
        bool ok = runTool(tmp, outPath, kbps, err);
        std::error_code ec;
        fs::remove(tmp, ec);
        return ok;
    }

    Lame& L = g_lame;
    lame_t g = L.init();
    if (!g) { err = "lame_init failed"; return false; }
    L.set_in_samplerate(g, rate);
    L.set_out_samplerate(g, rate);
    L.set_num_channels(g, 1);
    L.set_mode(g, 3); // MONO
    L.set_brate(g, kbps);
    L.set_quality(g, 5);
    if (L.init_params(g) < 0)
    {
        L.close(g);
        err = "lame_init_params failed";
        return false;
    }

    std::vector<unsigned char> out;
    const int chunk = 8192;
    std::vector<unsigned char> buf((size_t)(chunk * 5 / 4 + 7200));
    for (size_t pos = 0; pos < pcm.size(); pos += chunk)
    {
        int n = (int)std::min<size_t>(chunk, pcm.size() - pos);
        int w = L.encode_buffer(g, &pcm[pos], &pcm[pos], n, buf.data(), (int)buf.size());
        if (w < 0) { L.close(g); err = "lame encode error"; return false; }
        out.insert(out.end(), buf.begin(), buf.begin() + w);
    }
    int w = L.encode_flush(g, buf.data(), (int)buf.size());
    if (w > 0) out.insert(out.end(), buf.begin(), buf.begin() + w);
    L.close(g);

    std::string tmp = outPath + ".part";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write " + outPath; return false; }
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) fs::rename(tmp, outPath, ec);
    if (!ok || ec) { fs::remove(tmp, ec); err = "write failed"; return false; }
    return true;
}

bool wavToMp3(const std::string& wavPath, const std::string& mp3Path, int kbps, std::string& err)
{
    std::vector<int16_t> pcm;
    int rate = 8000;
    if (!readWav16(wavPath, pcm, rate)) { err = "cannot read " + wavPath; return false; }
    return encodeMp3(pcm, rate, kbps, mp3Path, err);
}
