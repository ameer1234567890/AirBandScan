#include "sat/reed_solomon.h"

#include <cstring>

namespace {
constexpr int kRoots = 32;
constexpr int kFcr = 112;
constexpr int kPrim = 11;
}

ReedSolomon::ReedSolomon()
{
    int x = 1;
    for (int i = 0; i < 255; ++i)
    {
        exp_[i] = (uint8_t)x;
        log_[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x187;
    }
    for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
    log_[0] = -1;

    // g(x) = prod (x - alpha^(prim*(fcr+i))), highest coefficient first.
    uint8_t g[kRoots + 1] = {1};
    int deg = 0;
    for (int i = 0; i < kRoots; ++i)
    {
        uint8_t r = pw(kPrim * (kFcr + i));
        uint8_t n[kRoots + 1] = {0};
        for (int k = 0; k <= deg; ++k)
        {
            n[k] ^= g[k];                 // * x
            n[k + 1] ^= mul(g[k], r);     // * r
        }
        ++deg;
        std::memcpy(g, n, sizeof(g));
    }
    std::memcpy(gen_, g, sizeof(gen_));
}

void ReedSolomon::encode(uint8_t* cw) const
{
    uint8_t rem[kRoots] = {0};
    for (int i = 0; i < 255 - kRoots; ++i)
    {
        uint8_t fb = cw[i] ^ rem[0];
        std::memmove(rem, rem + 1, kRoots - 1);
        rem[kRoots - 1] = 0;
        if (fb)
            for (int k = 0; k < kRoots; ++k) rem[k] ^= mul(fb, gen_[k + 1]);
    }
    std::memcpy(cw + 255 - kRoots, rem, kRoots);
}

int ReedSolomon::decode(uint8_t* cw) const
{
    // Syndromes S_i = c(alpha^(prim*(fcr+i))), cw[0] is the x^254 coefficient.
    uint8_t S[kRoots];
    bool clean = true;
    for (int i = 0; i < kRoots; ++i)
    {
        uint8_t r = pw(kPrim * (kFcr + i)), s = 0;
        for (int j = 0; j < 255; ++j) s = mul(s, r) ^ cw[j];
        S[i] = s;
        clean = clean && s == 0;
    }
    if (clean) return 0;

    // Berlekamp-Massey: error locator L(x) = prod (1 - X_k x).
    uint8_t L[kRoots + 1] = {1}, B[kRoots + 1] = {1}, T[kRoots + 1];
    int l = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < kRoots; ++n)
    {
        uint8_t d = S[n];
        for (int i = 1; i <= l; ++i) d ^= mul(L[i], S[n - i]);
        if (d == 0) { ++m; continue; }
        uint8_t coef = mul(d, exp_[255 - log_[b]]); // d / b
        std::memcpy(T, L, sizeof(L));
        for (int i = 0; i + m <= kRoots; ++i) L[i + m] ^= mul(coef, B[i]);
        if (2 * l <= n)
        {
            l = n + 1 - l;
            std::memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        }
        else
            ++m;
    }
    if (l > kRoots / 2) return -1;

    // Omega(x) = S(x) L(x) mod x^32.
    uint8_t O[kRoots] = {0};
    for (int i = 0; i < kRoots; ++i)
        for (int k = 0; k <= i && k <= l; ++k) O[i] ^= mul(L[k], S[i - k]);

    // Chien search over every byte position p (coefficient of x^p), X = alpha^(prim*p).
    int found = 0;
    int pos[kRoots];
    uint8_t val[kRoots];
    for (int p = 0; p < 255; ++p)
    {
        int xl = (kPrim * p) % 255;     // log X
        int xinv = (255 - xl) % 255;    // log X^-1
        uint8_t sum = 0;
        for (int i = 0; i <= l; ++i) sum ^= mul(L[i], pw(xinv * i));
        if (sum) continue;
        // Forney: e = X^(1-fcr) * Omega(X^-1) / L'(X^-1).
        uint8_t om = 0, dl = 0;
        for (int i = 0; i < kRoots; ++i) om ^= mul(O[i], pw(xinv * i));
        for (int i = 1; i <= l; i += 2) dl ^= mul(L[i], pw(xinv * (i - 1)));
        if (dl == 0) return -1;
        uint8_t e = mul(mul(om, pw(xl * (1 - kFcr))), exp_[255 - log_[dl]]);
        if (found >= kRoots) return -1;
        pos[found] = 254 - p;
        val[found] = e;
        ++found;
    }
    if (found != l) return -1;
    for (int k = 0; k < found; ++k) cw[pos[k]] ^= val[k];
    return found;
}

int ReedSolomon::decodeInterleaved(uint8_t* block, int depth) const
{
    int total = 0;
    bool fail = false;
    uint8_t cw[255];
    for (int n = 0; n < depth; ++n)
    {
        for (int k = 0; k < 255; ++k) cw[k] = block[k * depth + n];
        int r = decode(cw);
        if (r < 0) { fail = true; continue; }
        total += r;
        if (r > 0)
            for (int k = 0; k < 255; ++k) block[k * depth + n] = cw[k];
    }
    return fail ? -1 : total;
}
