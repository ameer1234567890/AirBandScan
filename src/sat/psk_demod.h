// QPSK / OQPSK demodulator for Meteor-M LRPT (72 ksym/s), producing soft
// symbols for the Viterbi decoder.
//
//   AGC --> NCO (x^4 FFT acquisition + 2nd order Costas loop)
//       --> RRC matched filter --> cubic interpolator at half-symbol spacing
//       --> Gardner timing loop (on I and Q, half a symbol apart for OQPSK)
//       --> soft I/Q pairs
#pragma once

#include "dsp/channelizer.h"
#include "dsp/jfft.h"

#include <cstdint>
#include <vector>

class PskDemod
{
public:
    PskDemod(double fs, double symRate, bool oqpsk, double maxOffsetHz = 20000.0);

    void process(const cf* in, int n, std::vector<int8_t>& soft);

    bool   locked() const { return lockAvg_ > kLockOn || (locked_ && lockAvg_ > kLockOff); }
    double lockQuality() const { return lockAvg_; }        // ~1 locked, ~0 noise
    double freqOffsetHz() const;
    double symbolRate() const;
    // Recent symbols (normalised, ~unit amplitude) for a constellation plot.
    std::vector<cf> constellation(int n = 512) const;

private:
    // -E[cos 4 theta]: ~exp(-8 sigma^2) with noise, so ~0.3 at Es/N0 5 dB and ~0 unlocked.
    static constexpr double kLockOn = 0.20, kLockOff = 0.10;

    void acquire();
    void setLoop(double bw);
    void symbol(cf z);

    double fs_, sps_;
    bool oqpsk_;
    double maxOff_;

    float agcPow_ = 1.f, agcA_;
    // NCO
    double phase_ = 0, freq_ = 0;   // rad / sample
    double alpha_ = 0, beta_ = 0;
    // acquisition
    JFFT fft_;
    static constexpr int kAcqAvg = 8;
    int acqN_ = 1024;
    std::vector<JFFT::cpx_type> acq_;
    std::vector<double> acqPow_;
    int acqFill_ = 0, acqBlocks_ = 0;
    // matched filter + interpolator
    FirDecim rrc_;
    cf hist_[8];
    int64_t n_ = 0;                 // input samples seen (after the filter)
    double next_ = 4.0;             // time of the next half-symbol output (samples)
    double half_, halfNom_;         // half-symbol period (samples)
    cf h_[4];                       // last four half-symbol samples (h_[3] newest)
    int64_t m_ = 0;                 // half-symbol outputs so far
    // symbol level
    float amp_ = 0.5f;
    double lockAvg_ = 0;
    bool locked_ = false;
    std::vector<cf> ring_;
    size_t ringPos_ = 0;
    std::vector<int8_t>* out_ = nullptr;
};
