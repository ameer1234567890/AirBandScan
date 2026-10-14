// Channelizer: pulls one narrow AM channel out of the wideband IQ stream.
//
//   wideband IQ --NCO mix--> multi-stage FIR decimation --> ~16 kHz IQ
//               --> sharp channel low-pass (+-bw/2)
//
// Same idea as AirScope's Ddc (dsp/ddc.h) but in float, with each FIR stage
// only evaluated on the samples it keeps, so a channel costs a handful of
// multiply-adds per input sample and several can run side by side.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using cf = std::complex<float>;

// Windowed-sinc low-pass taps (Blackman), unity DC gain.
inline std::vector<float> designLowPass(double inRate, double cutoffHz, int taps)
{
    if ((taps & 1) == 0) ++taps;
    std::vector<float> h((size_t)taps);
    double fc = cutoffHz / inRate;
    int M = taps - 1;
    double sum = 0.0;
    for (int n = 0; n < taps; ++n)
    {
        double x = n - M / 2.0;
        double sinc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
        double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * n / M) + 0.08 * std::cos(4.0 * M_PI * n / M);
        h[(size_t)n] = (float)(sinc * w);
        sum += sinc * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// Decimating FIR on complex floats. The delay line is stored twice so every
// convolution reads one contiguous span (no modulo in the inner loop).
class FirDecim
{
public:
    void init(const std::vector<float>& taps, int factor)
    {
        h_ = taps;
        factor_ = factor < 1 ? 1 : factor;
        L_ = (int)h_.size();
        line_.assign((size_t)L_ * 2, cf(0, 0));
        pos_ = 0;
        phase_ = 0;
    }

    // Returns true when an output sample is produced.
    inline bool push(cf x, cf& out)
    {
        line_[(size_t)pos_] = x;
        line_[(size_t)(pos_ + L_)] = x;
        if (++pos_ >= L_) pos_ = 0;
        if (++phase_ < factor_) return false;
        phase_ = 0;
        // line_[pos_ .. pos_+L_) is oldest..newest.
        const cf* p = &line_[(size_t)pos_];
        float re = 0.f, im = 0.f;
        for (int k = 0; k < L_; ++k)
        {
            float t = h_[(size_t)(L_ - 1 - k)];
            re += p[k].real() * t;
            im += p[k].imag() * t;
        }
        out = cf(re, im);
        return true;
    }

private:
    std::vector<float> h_;
    std::vector<cf> line_;
    int L_ = 1, factor_ = 1, pos_ = 0, phase_ = 0;
};

class Channelizer
{
public:
    // fs: wideband rate. offsetHz: channel - SDR centre. bwHz: total channel width.
    Channelizer(double fs, double offsetHz, double bwHz, double targetRate = 16000.0)
    {
        fs_ = fs;
        double inc = -2.0 * M_PI * offsetHz / fs;
        rot_ = std::complex<double>(std::cos(inc), std::sin(inc));

        int total = (int)std::lround(fs / targetRate);
        if (total < 1) total = 1;
        // Prefer a decimation made of small primes so it splits into stages.
        auto smooth = [](int n) {
            for (int p : {2, 3, 5, 7}) while (n % p == 0) n /= p;
            return n == 1;
        };
        for (int d = 0; d < total && !smooth(total); ++d)
        {
            if (smooth(total - d)) { total -= d; break; }
            if (smooth(total + d)) { total += d; break; }
        }

        // Group prime factors (largest first) into stages of at most 16.
        std::vector<int> primes;
        for (int n = total, p = 2; n > 1;)
        {
            if (n % p == 0) { primes.push_back(p); n /= p; }
            else if (++p > n) { primes.push_back(n); break; }
        }
        std::vector<int> stages;
        for (auto it = primes.rbegin(); it != primes.rend(); ++it)
        {
            if (!stages.empty() && stages.back() * *it <= 16) stages.back() *= *it;
            else stages.push_back(*it);
        }

        double pass = bwHz * 0.5;
        double rate = fs;
        for (int f : stages)
        {
            double out = rate / f;
            // Only the band that would alias onto +-pass must be rejected.
            double stop = out - pass;
            double trans = std::max(stop - pass, out * 0.05);
            int taps = (int)std::ceil(5.5 * rate / trans) | 1;
            if (taps < 2 * f + 1) taps = 2 * f + 1;
            if (taps > 1023) taps = 1023;
            FirDecim d;
            d.init(designLowPass(rate, (pass + stop) * 0.5, taps), f);
            stages_.push_back(std::move(d));
            rate = out;
        }
        outRate_ = rate;

        // Final sharp channel filter (rejects the neighbouring 8.33 kHz channel).
        int taps = (int)std::ceil(5.5 * outRate_ / 1500.0) | 1;
        chan_.init(designLowPass(outRate_, pass, taps), 1);
    }

    double outputRate() const { return outRate_; }

    // Interleaved float IQ in; appends channel samples to out.
    void process(const float* iq, int n, std::vector<cf>& out)
    {
        for (int i = 0; i < n; ++i)
        {
            std::complex<double> s(iq[2 * i], iq[2 * i + 1]);
            s *= osc_;
            osc_ *= rot_;
            if ((++oscN_ & 1023) == 0) osc_ /= std::abs(osc_);
            cf x((float)s.real(), (float)s.imag());
            size_t k = 0;
            for (; k < stages_.size(); ++k)
            {
                cf y;
                if (!stages_[k].push(x, y)) break;
                x = y;
            }
            if (k < stages_.size()) continue;
            cf y;
            chan_.push(x, y);
            out.push_back(y);
        }
    }

private:
    double fs_ = 0, outRate_ = 0;
    std::complex<double> osc_{1.0, 0.0}, rot_{1.0, 0.0};
    uint64_t oscN_ = 0;
    std::vector<FirDecim> stages_;
    FirDecim chan_;
};

// Biquad (RBJ cookbook), direct form I, float.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad lowpass(double fs, double f0, double q = 0.7071)
    {
        double w = 2 * M_PI * f0 / fs, c = std::cos(w), a = std::sin(w) / (2 * q);
        double a0 = 1 + a;
        Biquad b;
        b.b0 = (float)((1 - c) / 2 / a0); b.b1 = (float)((1 - c) / a0); b.b2 = b.b0;
        b.a1 = (float)(-2 * c / a0); b.a2 = (float)((1 - a) / a0);
        return b;
    }
    static Biquad highpass(double fs, double f0, double q = 0.7071)
    {
        double w = 2 * M_PI * f0 / fs, c = std::cos(w), a = std::sin(w) / (2 * q);
        double a0 = 1 + a;
        Biquad b;
        b.b0 = (float)((1 + c) / 2 / a0); b.b1 = (float)(-(1 + c) / a0); b.b2 = b.b0;
        b.a1 = (float)(-2 * c / a0); b.a2 = (float)((1 - a) / a0);
        return b;
    }
    inline float run(float x)
    {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// AM envelope demodulator: channel IQ (~16 kHz) -> 8 kHz audio.
//
// The audio is divided by the (slowly smoothed) carrier level, so its
// amplitude is the modulation index: a natural, pump-free AGC for AM where
// the carrier stays up for the whole push-to-talk.
class AmDemod
{
public:
    static constexpr double kAudioRate = 8000.0;

    explicit AmDemod(double inRate) : inRate_(inRate)
    {
        carrierA_ = (float)(1.0 - std::exp(-1.0 / (0.040 * inRate))); // ~40 ms
        hp1_ = Biquad::highpass(inRate, 250.0);
        hp2_ = Biquad::highpass(inRate, 250.0);
        lp1_ = Biquad::lowpass(inRate, 3200.0);
        lp2_ = Biquad::lowpass(inRate, 3200.0);
        step_ = kAudioRate / inRate;
    }

    // audio: modulation-normalised 8 kHz samples; carrier: matching carrier
    // amplitude per output sample (for level metering / classification).
    void process(const std::vector<cf>& in, std::vector<float>& audio, std::vector<float>& carrier)
    {
        for (const cf& s : in)
        {
            float env = std::abs(s);
            carrier_ += (env - carrier_) * carrierA_;
            float a = env - carrier_;
            a = lp2_.run(lp1_.run(hp2_.run(hp1_.run(a))));
            float norm = a / std::max(carrier_, 1e-7f);

            // Linear-interpolating resampler to exactly 8 kHz.
            phase_ += step_;
            if (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                double frac = 1.0 - phase_ / step_; // position between prev and now
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                audio.push_back((float)(prev_ + (norm - prev_) * frac));
                carrier.push_back(carrier_);
            }
            prev_ = norm;
        }
    }

private:
    double inRate_;
    float carrier_ = 0.f, carrierA_ = 0.01f;
    Biquad hp1_, hp2_, lp1_, lp2_;
    double step_ = 0.5, phase_ = 0.0;
    float prev_ = 0.f;
};
