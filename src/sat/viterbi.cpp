#include "sat/viterbi.h"

#include <algorithm>

namespace {

struct Table
{
    // Expected output pair for the 7-bit register value (bit0 = newest).
    uint8_t c0[128], c1[128];
    Table()
    {
        for (int sr = 0; sr < 128; ++sr)
        {
            c0[sr] = (uint8_t)(__builtin_popcount(sr & 0x4F) & 1);
            c1[sr] = (uint8_t)(__builtin_popcount(sr & 0x6D) & 1);
        }
    }
};
const Table kT;

} // namespace

void Viterbi27::reset()
{
    for (int s = 0; s < 64; ++s) pm_[s] = 0;
    dec_.clear();
}

void Viterbi27::encode(const uint8_t* bits, int n, uint32_t& state, std::vector<uint8_t>& out)
{
    for (int i = 0; i < n; ++i)
    {
        state = ((state << 1) | (bits[i] & 1)) & 127;
        out.push_back(kT.c0[state]);
        out.push_back(kT.c1[state]);
    }
}

void Viterbi27::decode(const int8_t* soft, int pairs, std::vector<uint8_t>& bits)
{
    int32_t nm[64];
    for (int k = 0; k < pairs; ++k)
    {
        int s0 = soft[2 * k], s1 = soft[2 * k + 1];
        // Branch metric (correlation) for each of the 4 output pairs.
        int bm[4] = {s0 + s1, s0 - s1, -s0 + s1, -s0 - s1};
        uint64_t d = 0;
        int32_t best = INT32_MIN;
        for (int n = 0; n < 64; ++n)
        {
            int p0 = n >> 1, p1 = p0 | 32;
            int sr0 = n, sr1 = n | 64;
            int32_t m0 = pm_[p0] + bm[(kT.c0[sr0] << 1) | kT.c1[sr0]];
            int32_t m1 = pm_[p1] + bm[(kT.c0[sr1] << 1) | kT.c1[sr1]];
            if (m1 > m0) { nm[n] = m1; d |= 1ull << n; }
            else nm[n] = m0;
            best = std::max(best, nm[n]);
        }
        for (int n = 0; n < 64; ++n) pm_[n] = nm[n] - best;
        dec_.push_back(d);
        if ((int)dec_.size() >= kTraceback + kChunk) traceback(kChunk, bits);
    }
}

void Viterbi27::flush(std::vector<uint8_t>& bits)
{
    traceback((int)dec_.size(), bits);
}

void Viterbi27::traceback(int count, std::vector<uint8_t>& bits)
{
    int n = (int)dec_.size();
    if (count > n) count = n;
    int s = (int)(std::max_element(pm_, pm_ + 64) - pm_);
    std::vector<uint8_t> out((size_t)count);
    for (int t = n - 1; t >= 0; --t)
    {
        if (t < count) out[(size_t)t] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)(((dec_[(size_t)t] >> s) & 1) << 5);
    }
    bits.insert(bits.end(), out.begin(), out.end());
    dec_.erase(dec_.begin(), dec_.begin() + count);
}
