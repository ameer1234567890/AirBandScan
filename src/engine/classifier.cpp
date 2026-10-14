#include "engine/classifier.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kRate = 8000;
constexpr int kHop = 160;     // 20 ms
constexpr int kWin = 320;     // 40 ms analysis window (two pitch periods @ 50 Hz)
constexpr int kLagMin = 20;   // 400 Hz
constexpr int kLagMax = 100;  //  80 Hz

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

} // namespace

VoiceVerdict classifyVoice(const std::vector<float>& a, const std::vector<float>& carrier,
                           double threshold)
{
    VoiceVerdict v;
    const int n = (int)std::min(a.size(), carrier.size());
    if (n < kWin + kLagMax)
    {
        v.reason = "too short to analyse";
        return v;
    }

    // Reference carrier level: median over the recording.
    std::vector<float> cs(carrier.begin(), carrier.begin() + n);
    std::nth_element(cs.begin(), cs.begin() + cs.size() * 7 / 10, cs.end());
    const float carrierRef = cs[cs.size() * 7 / 10];

    std::vector<double> frameDb, pitches;
    int frames = 0, voiced = 0;
    double modSum = 0.0;

    for (int start = 0; start + kWin + kLagMax <= n; start += kHop)
    {
        if (carrier[(size_t)start + kWin / 2] < 0.5f * carrierRef)
            continue; // carrier not up: noise between/around the transmission
        const float* x = &a[(size_t)start];
        double e = 0.0;
        int zc = 0;
        for (int i = 0; i < kWin; ++i)
        {
            e += (double)x[i] * x[i];
            if (i && ((x[i] >= 0) != (x[i - 1] >= 0))) ++zc;
        }
        double rms = std::sqrt(e / kWin);
        ++frames;
        modSum += rms;
        frameDb.push_back(20.0 * std::log10(rms + 1e-6));

        if (rms < 0.02) // essentially unmodulated
            continue;

        // Normalised autocorrelation over the pitch lag range.
        double r[kLagMax + 1] = {};
        double best = 0.0;
        for (int lag = kLagMin; lag <= kLagMax; ++lag)
        {
            double xy = 0, yy = 0;
            const float* y = x + lag;
            for (int i = 0; i < kWin; ++i)
            {
                xy += (double)x[i] * y[i];
                yy += (double)y[i] * y[i];
            }
            r[lag] = xy / std::sqrt(e * yy + 1e-12);
            best = std::max(best, r[lag]);
        }
        // Pitch period = first lag close to the best peak (not a multiple).
        int period = kLagMax;
        for (int lag = kLagMin + 1; lag < kLagMax; ++lag)
            if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { period = lag; break; }
        double zcr = (double)zc / kWin;
        // Voiced speech: strong periodicity, ZCR well under data-tone rates
        // (a 1200/2400 Hz ACARS tone has zcr >= 0.3 at 8 kHz).
        if (best > 0.45 && zcr < 0.22)
        {
            ++voiced;
            pitches.push_back((double)kRate / period);
        }
    }

    if (frames < 5)
    {
        v.reason = "no stable carrier";
        return v;
    }
    v.voicedFrac = (double)voiced / frames;
    v.modDepth = frames ? modSum / frames : 0.0;

    // Spread between loud (90th pct) and quiet (20th pct) frames.
    std::vector<double> s = frameDb;
    std::sort(s.begin(), s.end());
    double p20 = s[(size_t)(0.20 * (s.size() - 1))];
    double p90 = s[(size_t)(0.90 * (s.size() - 1))];
    v.levelSpreadDb = p90 - p20;

    // Speech intonation moves the pitch around; a tone (test tone, ELT
    // sweep step, carrier with hum) keeps it fixed.
    double pitchVar = 1.0;
    if (pitches.size() >= 5)
    {
        double m = 0, q = 0;
        for (double p : pitches) m += p;
        m /= pitches.size();
        for (double p : pitches) q += (p - m) * (p - m);
        pitchVar = std::sqrt(q / pitches.size()) / m;
    }
    const bool tonal = pitchVar < 0.04;
    if (tonal) v.voicedFrac *= pitchVar / 0.04;

    v.score = 0.55 * clamp01(v.voicedFrac / 0.20) +
              0.30 * clamp01((v.levelSpreadDb - 3.0) / 6.0) +
              0.15 * clamp01(v.modDepth / 0.08);
    if (tonal) v.score *= 0.5;
    v.voice = v.score >= threshold && v.voicedFrac >= 0.05;

    char buf[160];
    if (v.voice)
        std::snprintf(buf, sizeof(buf), "speech (score %.2f)", v.score);
    else if (v.modDepth < 0.03)
        std::snprintf(buf, sizeof(buf), "unmodulated carrier / no audio (score %.2f)", v.score);
    else if (tonal)
        std::snprintf(buf, sizeof(buf), "tone with constant pitch (score %.2f)", v.score);
    else if (v.voicedFrac < 0.05 && v.levelSpreadDb < 4.0)
        std::snprintf(buf, sizeof(buf), "steady signal: tone, data or noise (score %.2f)", v.score);
    else if (v.voicedFrac < 0.05)
        std::snprintf(buf, sizeof(buf), "noise, no pitch detected (score %.2f)", v.score);
    else
        std::snprintf(buf, sizeof(buf), "weak or garbled audio (score %.2f)", v.score);
    v.reason = buf;
    return v;
}
