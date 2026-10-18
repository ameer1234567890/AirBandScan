#include "sat/lrpt_decoder.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t kAsm = 0x1ACFFC1Du;
constexpr int kFrameBits = 1020 * 8;     // after the ASM
constexpr int kTestPairs = 1024;         // lock check window (symbols)
constexpr double kLockBer = 0.12;
constexpr uint64_t kLoseLockSymbols = 72000 * 3;

struct Pn
{
    uint8_t seq[1020];
    Pn()
    {
        // CCSDS pseudo-randomiser: x^8 + x^7 + x^5 + x^3 + 1, all ones seed.
        uint8_t sr = 0xFF;
        for (int i = 0; i < 1020; ++i)
        {
            uint8_t byte = 0;
            for (int b = 0; b < 8; ++b)
            {
                int out = sr & 1;
                byte = (uint8_t)((byte << 1) | out);
                int fb = ((sr >> 0) ^ (sr >> 3) ^ (sr >> 5) ^ (sr >> 7)) & 1;
                sr = (uint8_t)((sr >> 1) | (fb << 7));
            }
            seq[i] = byte;
        }
    }
};
const Pn kPn;

inline int8_t neg(int8_t v) { return (int8_t)(v == -128 ? 127 : -v); }

} // namespace

void LrptDecoder::reset()
{
    pend_.clear();
    locked_ = false;
    ber_ = 0.5;
    sinceFrame_ = 0;
    vit_.reset();
    vbits_.clear();
    lastBit_ = 0;
    sr_ = 0;
    syncState_ = 0;
    misses_ = 0;
    fbits_.clear();
    framesOk_ = framesBad_ = packets_ = 0;
    scid_ = -1;
    for (auto& v : vc_) v = Vc{};
    img_.reset();
}

// Hypothesis h: bit 0 = drop one soft value (I/Q pairing), bit 1 = swap I/Q,
// bits 2-3 = rotation by 90 degree steps.
void LrptDecoder::transform(const int8_t* in, int pairs, int8_t* out, int h) const
{
    bool swap = (h >> 1) & 1;
    int rot = (h >> 2) & 3;
    for (int k = 0; k < pairs; ++k)
    {
        int8_t i = in[2 * k], q = in[2 * k + 1];
        if (swap) std::swap(i, q);
        int8_t a, b;
        switch (rot)
        {
        case 0: a = i; b = q; break;
        case 1: a = neg(q); b = i; break;
        case 2: a = neg(i); b = neg(q); break;
        default: a = q; b = neg(i); break;
        }
        out[2 * k] = a;
        out[2 * k + 1] = b;
    }
}

double LrptDecoder::testBer(const int8_t* soft, int h) const
{
    // Decode under hypothesis h, re-encode and count hard decisions that
    // disagree: ~0 for the right hypothesis, ~50 % for the wrong ones.
    std::vector<int8_t> t((size_t)kTestPairs * 2);
    transform(soft + (h & 1), kTestPairs, t.data(), h);
    Viterbi27 v;
    std::vector<uint8_t> dec, enc;
    dec.reserve(kTestPairs);
    enc.reserve(kTestPairs * 2);
    v.decode(t.data(), kTestPairs, dec);
    v.flush(dec);
    uint32_t st = 0;
    Viterbi27::encode(dec.data(), (int)dec.size(), st, enc);
    int err = 0, tot = 0;
    for (int k = 64; k < kTestPairs - 32; ++k)
        for (int j = 0; j < 2; ++j)
        {
            int8_t s = t[(size_t)(2 * k + j)];
            if (s == 0) continue;
            err += ((s < 0) ? 1 : 0) != enc[(size_t)(2 * k + j)];
            ++tot;
        }
    return tot ? (double)err / tot : 1.0;
}

int LrptDecoder::searchLock(const int8_t* soft, double& ber) const
{
    ber = 1.0;
    int best = 0;
    for (int h = 0; h < 16; ++h)
    {
        double r = testBer(soft, h);
        if (r < ber) { ber = r; best = h; }
    }
    return best;
}

void LrptDecoder::pushSoft(const int8_t* soft, int n)
{
    pend_.insert(pend_.end(), soft, soft + n);
    const size_t win = (size_t)kTestPairs * 2 + 2;
    size_t pos = 0;
    while (pend_.size() - pos >= win)
    {
        const int8_t* w = pend_.data() + pos;
        if (!locked_)
        {
            if (!carrier_)
            {
                // No carrier lock: nothing to search for, save the CPU.
                pos += (size_t)kTestPairs * 2;
                continue;
            }
            double b;
            int h = searchLock(w, b);
            ber_ = b;
            if (b > kLockBer)
            {
                // Slide by half a window so a signal that starts mid-window is caught.
                pos += (size_t)kTestPairs;
                continue;
            }
            // The pairing shift is absorbed by dropping one soft value, so a
            // locked hypothesis never has bit 0 set.
            hyp_ = h & ~1;
            locked_ = true;
            sinceFrame_ = 0;
            vit_.reset();
            if (h & 1) ++pos;
            continue;
        }

        // Locked: keep checking the hypothesis. A carrier cycle slip rotates
        // the constellation by 90 degrees; find the new one right away. While
        // frames keep arriving a check every 8th window is enough.
        double b = ber_;
        if (syncState_ != 2 || (++checkN_ & 7) == 0)
        {
            b = testBer(w, hyp_);
            ber_ += (b - ber_) * 0.3;
        }
        if (b > kLockBer)
        {
            double nb;
            int h = searchLock(w, nb);
            if (nb <= kLockBer && h != hyp_)
            {
                if (h & 1) ++pos;
                hyp_ = h & ~1;
                ber_ = nb;
                w = pend_.data() + pos;
            }
        }
        std::vector<int8_t> t((size_t)kTestPairs * 2);
        transform(w, kTestPairs, t.data(), hyp_);
        pos += (size_t)kTestPairs * 2;
        vbits_.clear();
        vit_.decode(t.data(), kTestPairs, vbits_);
        bits(vbits_);

        sinceFrame_ += (uint64_t)kTestPairs;
        if (sinceFrame_ > kLoseLockSymbols)
        {
            // No frame for 3 s: the pass ended or the signal faded; search again.
            locked_ = false;
            syncState_ = 0;
            fbits_.clear();
        }
    }
    pend_.erase(pend_.begin(), pend_.begin() + (long)pos);
}

void LrptDecoder::bits(const std::vector<uint8_t>& b)
{
    for (uint8_t x : b)
    {
        if (nrzm_)
        {
            uint8_t d = x ^ lastBit_;
            lastBit_ = x;
            x = d;
        }
        if (syncState_ == 0)
        {
            sr_ = (sr_ << 1) | x;
            int e = __builtin_popcount(sr_ ^ kAsm), ei = __builtin_popcount(~sr_ ^ kAsm);
            if (e <= 2 || ei <= 2)
            {
                invert_ = ei < e;
                syncState_ = 1;
                misses_ = 0;
                fbits_.clear();
            }
            continue;
        }
        fbits_.push_back(x);
        if ((int)fbits_.size() < kFrameBits + 32) continue;

        // Frame complete, followed by the next ASM.
        std::memcpy(cadu_, "\x1a\xcf\xfc\x1d", 4);
        for (int i = 0; i < 1020; ++i)
        {
            uint8_t v = 0;
            for (int k = 0; k < 8; ++k) v = (uint8_t)((v << 1) | fbits_[(size_t)(i * 8 + k)]);
            cadu_[4 + i] = invert_ ? (uint8_t)~v : v;
        }
        uint32_t next = 0;
        for (int k = 0; k < 32; ++k) next = (next << 1) | fbits_[(size_t)(kFrameBits + k)];
        if (invert_) next = ~next;
        fbits_.clear();
        frame(cadu_);
        if (__builtin_popcount(next ^ kAsm) <= 6)
        {
            syncState_ = 2;
            misses_ = 0;
        }
        else if (++misses_ > 3)
        {
            syncState_ = 0;
            sr_ = 0;
        }
    }
}

void LrptDecoder::frame(uint8_t* f)
{
    for (int i = 0; i < 1020; ++i) f[4 + i] ^= kPn.seq[i];
    pushCadu(f, 1, true);
}

void LrptDecoder::pushCadu(const uint8_t* cadu, int n, bool rsCheck)
{
    uint8_t buf[1020];
    for (int i = 0; i < n; ++i)
    {
        std::memcpy(buf, cadu + (size_t)i * 1024 + 4, 1020);
        if (rsCheck && rs_.decodeInterleaved(buf, 4) < 0)
        {
            ++framesBad_;
            continue;
        }
        ++framesOk_;
        sinceFrame_ = 0;
        vcdu(buf);
    }
}

void LrptDecoder::vcdu(const uint8_t* v)
{
    int version = v[0] >> 6;
    if (version != 1) return;
    scid_ = ((v[0] & 0x3F) << 2) | (v[1] >> 6);
    int vcid = v[1] & 0x3F;
    if (vcid == 63) return; // fill
    uint32_t counter = ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 8) | v[4];
    Vc& c = vc_[vcid];
    if (c.have && counter != ((c.counter + 1) & 0xFFFFFF))
        c.buf.clear(); // lost frame(s): the packet in progress is incomplete
    c.counter = counter;
    c.have = true;

    // 6 byte header + 2 byte insert zone + 2 byte M_PDU header + 882 data.
    int fhp = ((v[8] & 7) << 8) | v[9];
    const uint8_t* z = v + 10;
    const int zlen = 882;
    auto drain = [&]() {
        while (c.buf.size() >= 6)
        {
            size_t total = 7 + (size_t)((c.buf[4] << 8) | c.buf[5]);
            if (c.buf.size() < total) break;
            packet(c.buf.data(), total);
            c.buf.erase(c.buf.begin(), c.buf.begin() + (long)total);
        }
    };
    if (fhp == 2047)
    {
        if (!c.buf.empty()) c.buf.insert(c.buf.end(), z, z + zlen);
        drain();
        return;
    }
    if (fhp >= zlen) { c.buf.clear(); return; }
    if (!c.buf.empty())
    {
        c.buf.insert(c.buf.end(), z, z + fhp);
        drain();
    }
    c.buf.assign(z + fhp, z + zlen); // a new packet starts at fhp
    drain();
}

void LrptDecoder::packet(const uint8_t* p, size_t len)
{
    int apid = ((p[0] & 7) << 8) | p[1];
    if (apid == 2047) return;
    ++packets_;
    img_.addPacket(apid, p + 6, (int)len - 6);
}
