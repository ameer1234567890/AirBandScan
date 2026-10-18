#include "sat/offline.h"

#include "sat/lrpt_chain.h"
#include "sat/sstv.h"
#include "sat/wav_reader.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool saveLrpt(const LrptDecoder& dec, const std::string& outDir, const std::string& stem)
{
    const MsuMrImager& im = dec.imager();
    if (im.segments() == 0) return false;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    for (int c : im.channels())
    {
        std::string p = (fs::path(outDir) / (stem + "_ch" + std::to_string(c) + ".png")).string();
        if (writePng(p, im.channel(c))) std::printf("  wrote %s\n", p.c_str());
    }
    std::string p = (fs::path(outDir) / (stem + "_rgb.png")).string();
    Image rgb = im.composite();
    if (rgb.ch == 3 && writePng(p, rgb)) std::printf("  wrote %s\n", p.c_str());
    return true;
}

} // namespace

int decodeLrptFile(const std::string& in, const std::string& outDir, const std::string& variant)
{
    bool m2x = lrptIsM2x(variant);
    std::string ext = lower(fs::path(in).extension().string());
    std::string stem = fs::path(in).stem().string();
    auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<LrptChain> chain;
    LrptDecoder raw(m2x);
    LrptDecoder* dec = &raw;

    if (ext == ".cadu")
    {
        std::FILE* f = std::fopen(in.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> b(1024 * 256);
        size_t n;
        while ((n = std::fread(b.data(), 1, b.size(), f)) >= 1024)
            raw.pushCadu(b.data(), (int)(n / 1024), true);
        std::fclose(f);
    }
    else if (ext == ".s" || ext == ".soft")
    {
        std::FILE* f = std::fopen(in.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<int8_t> b(1 << 16);
        size_t n;
        while ((n = std::fread(b.data(), 1, b.size(), f)) > 0) raw.pushSoft(b.data(), (int)n);
        std::fclose(f);
    }
    else
    {
        WavReader wr;
        std::string err;
        if (!wr.open(in, err)) { std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str()); return 1; }
        if (wr.channels() != 2) { std::fprintf(stderr, "%s: an IQ (2 channel) WAV is needed\n", in.c_str()); return 1; }
        double rate = wr.rate();
        std::unique_ptr<Channelizer> ch;
        double chRate = rate;
        if (rate > 400000)
        {
            ch = std::make_unique<Channelizer>(rate, 0.0, 170000.0, 288000.0, 20000.0);
            chRate = ch->outputRate();
        }
        if (chRate < 2.0 * LrptChain::kSymbolRate)
        {
            std::fprintf(stderr, "%s: sample rate %.0f is too low (>= 144000 needed)\n", in.c_str(), rate);
            return 1;
        }
        std::printf("%s: %d S/s, %.1f s, demodulating at %.0f S/s (%s)\n", in.c_str(), wr.rate(),
                    wr.frames() / rate, chRate, m2x ? "OQPSK, NRZ-M" : "QPSK");
        chain = std::make_unique<LrptChain>(chRate, m2x);
        dec = &chain->decoder();
        std::vector<float> buf(2 * 65536);
        std::vector<cf> iq;
        uint64_t done = 0;
        int lastPct = -1;
        int n;
        while ((n = wr.read(buf.data(), 65536)) > 0)
        {
            iq.clear();
            if (ch) ch->process(buf.data(), n, iq);
            else
                for (int i = 0; i < n; ++i) iq.emplace_back(buf[2 * i], buf[2 * i + 1]);
            chain->process(iq.data(), (int)iq.size());
            done += (uint64_t)n;
            int pct = (int)(100 * done / std::max<uint64_t>(1, wr.frames()));
            if (pct / 10 != lastPct / 10)
            {
                lastPct = pct;
                std::printf("  %3d%%  carrier %s (%.2f)  offset %+.0f Hz  frames %llu ok / %llu bad\n", pct,
                            chain->demod().locked() ? "locked" : "searching", chain->demod().lockQuality(),
                            chain->demod().freqOffsetHz(), (unsigned long long)dec->frames(),
                            (unsigned long long)dec->framesBad());
            }
        }
    }

    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("frames: %llu ok, %llu failed RS; packets %llu; image %d x %d, channels:",
                (unsigned long long)dec->frames(), (unsigned long long)dec->framesBad(),
                (unsigned long long)dec->packets(), MsuMrImager::kWidth, dec->imager().rows());
    for (int c : dec->imager().channels()) std::printf(" %d", c);
    std::printf("  (%.1f s)\n", sec);
    if (!saveLrpt(*dec, outDir, stem))
    {
        std::printf("no imagery decoded\n");
        return 2;
    }
    return 0;
}

int decodeSstvFile(const std::string& in, const std::string& outDir)
{
    WavReader wr;
    std::string err;
    if (!wr.open(in, err)) { std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str()); return 1; }
    if (wr.rate() < 8000) { std::fprintf(stderr, "%s: sample rate too low\n", in.c_str()); return 1; }
    std::printf("%s: %d Hz, %d channel(s), %.1f s\n", in.c_str(), wr.rate(), wr.channels(),
                (double)wr.frames() / wr.rate());
    SstvDecoder dec(wr.rate());
    std::string stem = fs::path(in).stem().string();
    std::error_code ec;
    fs::create_directories(outDir, ec);
    int count = 0;
    auto drain = [&]() {
        SstvDecoder::Result r;
        while (dec.take(r))
        {
            ++count;
            std::string mode = r.mode;
            for (auto& c : mode) if (c == ' ') c = '_';
            char name[64];
            std::snprintf(name, sizeof(name), "_%03d_%s.png", count, mode.c_str());
            std::string p = (fs::path(outDir) / (stem + name)).string();
            writePng(p, r.img);
            std::printf("  %s at %.1f s: %d/%d lines%s -> %s\n", r.mode.c_str(), r.startSec, r.rows, r.totalRows,
                        r.complete ? "" : " (partial)", p.c_str());
        }
    };
    const int ch = wr.channels();
    std::vector<float> buf((size_t)ch * 16384), mono(16384);
    int n;
    while ((n = wr.read(buf.data(), 16384)) > 0)
    {
        for (int i = 0; i < n; ++i) mono[(size_t)i] = buf[(size_t)i * ch]; // first channel
        dec.process(mono.data(), n);
        drain();
    }
    dec.flush();
    drain();
    if (count == 0)
    {
        std::printf("no SSTV image found (no VIS header)\n");
        return 2;
    }
    return 0;
}
