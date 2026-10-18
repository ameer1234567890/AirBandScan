#include "sat/psk_demod.h"

#include <algorithm>
#include <cmath>

namespace {

std::vector<float> rrcTaps(double sps, double alpha, int spanSym)
{
    int half = (int)std::ceil(spanSym * sps);
    std::vector<float> h((size_t)(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; ++i)
    {
        double t = i / sps, v;
        if (std::fabs(t) < 1e-9)
            v = 1 - alpha + 4 * alpha / M_PI;
        else if (std::fabs(std::fabs(t) - 1 / (4 * alpha)) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) +
                                          (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else
            v = (std::sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * std::cos(M_PI * t * (1 + alpha))) /
                (M_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h[(size_t)(i + half)] = (float)v;
        sum += v;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

inline cf cubic(const cf* p, float mu)
{
    // Lagrange through p[0..3] at -1, 0, 1, 2; evaluate at mu in [0, 1).
    float m2 = mu * mu, m3 = m2 * mu;
    float c0 = (-m3 + 3 * m2 - 2 * mu) / 6.f;
    float c1 = (m3 - 2 * m2 - mu + 2) / 2.f;
    float c2 = (-m3 + m2 + 2 * mu) / 2.f;
    float c3 = (m3 - mu) / 6.f;
    return p[0] * c0 + p[1] * c1 + p[2] * c2 + p[3] * c3;
}

inline float sgn(float v) { return v < 0 ? -1.f : 1.f; }

} // namespace

PskDemod::PskDemod(double fs, double symRate, bool oqpsk, double maxOffsetHz)
    : fs_(fs), sps_(fs / symRate), oqpsk_(oqpsk), maxOff_(std::min(maxOffsetHz, fs / 9.0))
{
    // (4x the offset must stay inside +-fs/2 for the acquisition FFT.)
    agcA_ = (float)(1.0 - std::exp(-1.0 / (0.02 * fs)));
    rrc_.init(rrcTaps(sps_, 0.6, 8), 1);
    halfNom_ = half_ = sps_ / 2.0;
    acqN_ = 1024;
    while (acqN_ < 65536 && fs / acqN_ > 32.0) acqN_ *= 2; // <= 32 Hz bins (8 Hz of offset)
    int n = acqN_;
    fft_.init(n);
    acq_.assign((size_t)acqN_, 0);
    acqPow_.assign((size_t)acqN_, 0.0);
    ring_.assign(1024, cf(0, 0));
    setLoop(0.006);
}

void PskDemod::setLoop(double bw)
{
    // 2nd order loop, damping 0.707, bandwidth bw (per symbol).
    double z = 0.707, th = bw / (z + 0.25 / z), d = 1 + 2 * z * th + th * th;
    alpha_ = 4 * z * th / d;
    beta_ = 4 * th * th / d;
}

double PskDemod::freqOffsetHz() const { return freq_ * fs_ / (2 * M_PI); }
double PskDemod::symbolRate() const { return fs_ / (2.0 * half_); }

std::vector<cf> PskDemod::constellation(int n) const
{
    std::vector<cf> out;
    n = std::min(n, (int)ring_.size());
    for (int i = 0; i < n; ++i) out.push_back(ring_[(ringPos_ + ring_.size() - n + i) % ring_.size()]);
    return out;
}

void PskDemod::acquire()
{
    // The 4th power strips the QPSK (and OQPSK) modulation, leaving a line at
    // 4x the carrier offset. Power spectra of kAcqAvg blocks are averaged.
    fft_.fft(acq_.data(), acqN_);
    for (int k = 0; k < acqN_; ++k) acqPow_[(size_t)k] += std::norm(acq_[(size_t)k]);
    if (++acqBlocks_ < kAcqAvg) return;
    acqBlocks_ = 0;

    int lim = std::min(acqN_ / 2 - 2, (int)(4 * maxOff_ / fs_ * acqN_));
    auto at = [&](int k) { return acqPow_[(size_t)((k + acqN_) % acqN_)]; };
    double best = 0, sum = 0;
    int bi = 0;
    for (int k = -lim; k <= lim; ++k)
    {
        double p = at(k);
        sum += p;
        if (p > best) { best = p; bi = k; }
    }
    double mean = sum / (2 * lim + 1);
    if (best > mean * 8.0) // averaged noise stays below ~3x the mean
    {
        double a = at(bi - 1), b = at(bi), c = at(bi + 1), den = a - 2 * b + c;
        double frac = den != 0 ? 0.5 * (a - c) / den : 0;
        double f = (bi + std::clamp(frac, -0.5, 0.5)) * fs_ / acqN_ / 4.0;
        if (std::fabs(f - freqOffsetHz()) > 50.0)
            freq_ = 2 * M_PI * f / fs_;
    }
    std::fill(acqPow_.begin(), acqPow_.end(), 0.0);
}

void PskDemod::process(const cf* in, int n, std::vector<int8_t>& soft)
{
    out_ = &soft;
    for (int i = 0; i < n; ++i)
    {
        cf x = in[i];
        agcPow_ += (std::norm(x) - agcPow_) * agcA_;
        x /= std::sqrt(std::max(agcPow_, 1e-20f));

        if (!locked())
        {
            cf x2 = x * x;
            acq_[(size_t)acqFill_] = JFFT::cpx_type(x2 * x2);
            if (++acqFill_ >= acqN_)
            {
                acqFill_ = 0;
                acquire();
            }
        }
        else if (acqFill_ || acqBlocks_)
        {
            acqFill_ = 0;
            acqBlocks_ = 0;
            std::fill(acqPow_.begin(), acqPow_.end(), 0.0);
        }

        cf y = x * cf((float)std::cos(-phase_), (float)std::sin(-phase_));
        phase_ += freq_;
        if (phase_ > M_PI) phase_ -= 2 * M_PI;
        else if (phase_ < -M_PI) phase_ += 2 * M_PI;

        cf f;
        rrc_.push(y, f);
        hist_[n_ & 7] = f;
        ++n_;

        // Interpolate every half-symbol whose 4 support samples have arrived.
        while (next_ + 2.0 < (double)n_)
        {
            int64_t k = (int64_t)std::floor(next_);
            float mu = (float)(next_ - (double)k);
            cf p[4] = {hist_[(k - 1) & 7], hist_[k & 7], hist_[(k + 1) & 7], hist_[(k + 2) & 7]};
            cf s = cubic(p, mu);
            h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = s;
            ++m_;
            next_ += half_;

            // QPSK: symbols on even outputs. OQPSK: I on even, Q on odd outputs;
            // a symbol completes on the odd one.
            bool strobe = oqpsk_ ? (m_ & 1) == 0 : (m_ & 1) == 1;
            if (!strobe || m_ < 4) continue;
            float e;
            cf z;
            if (!oqpsk_)
            {
                cf prev = h_[1], mid = h_[2], cur = h_[3];
                e = (prev.real() - cur.real()) * mid.real() + (prev.imag() - cur.imag()) * mid.imag();
                z = cur;
            }
            else
            {
                float pI = h_[0].real(), mI = h_[1].real(), cI = h_[2].real();
                float pQ = h_[1].imag(), mQ = h_[2].imag(), cQ = h_[3].imag();
                e = (pI - cI) * mI + (pQ - cQ) * mQ;
                z = cf(cI, cQ);
            }
            float a2 = std::max(amp_ * amp_, 1e-12f);
            e = std::clamp(e / a2, -1.f, 1.f);
            // Gardner: e < 0 when sampling late.
            next_ += 0.05 * e * halfNom_;
            half_ += 0.0002 * e * halfNom_;
            half_ = std::clamp(half_, halfNom_ * 0.995, halfNom_ * 1.005);
            symbol(z);
        }
    }
    out_ = nullptr;
}

void PskDemod::symbol(cf z)
{
    float a = (std::fabs(z.real()) + std::fabs(z.imag())) * 0.5f;
    amp_ += (a - amp_) * 0.002f;
    cf zn = z / std::max(amp_, 1e-9f) * 0.70710678f;

    // Costas error for QPSK points on the diagonals.
    float err = sgn(zn.real()) * zn.imag() - sgn(zn.imag()) * zn.real();
    err = std::clamp(err, -1.f, 1.f);
    phase_ += alpha_ * err;
    freq_ += beta_ * err / sps_;
    double maxF = 2 * M_PI * (maxOff_ * 1.5) / fs_;
    freq_ = std::clamp(freq_, -maxF, maxF);

    // cos(4 theta) lock metric: -1 on the QPSK diagonals.
    float r2 = std::norm(zn);
    if (r2 > 1e-12f)
    {
        cf z2 = zn * zn;
        float c4 = (z2 * z2).real() / (r2 * r2);
        lockAvg_ += (-c4 - lockAvg_) * 0.002;
    }
    bool now = locked();
    if (now != locked_)
    {
        locked_ = now;
        setLoop(now ? 0.0025 : 0.006);
    }

    ring_[ringPos_] = zn;
    ringPos_ = (ringPos_ + 1) % ring_.size();

    float scale = 48.f / 0.70710678f;
    out_->push_back((int8_t)std::clamp((int)std::lround(zn.real() * scale), -127, 127));
    out_->push_back((int8_t)std::clamp((int)std::lround(zn.imag() * scale), -127, 127));
}
