// Channel IQ -> (O)QPSK demodulator -> LRPT decoder.
#pragma once

#include "sat/lrpt_decoder.h"
#include "sat/psk_demod.h"

#include <string>

// Meteor-M N2-3 / N2-4 (and later): OQPSK + NRZ-M. Meteor-M N2 (retired): QPSK.
inline bool lrptIsM2x(const std::string& variant) { return variant != "m2"; }

class LrptChain
{
public:
    static constexpr double kSymbolRate = 72000.0;

    LrptChain(double fs, bool m2x) : demod_(fs, kSymbolRate, m2x), dec_(m2x) {}

    void process(const cf* in, int n)
    {
        soft_.clear();
        demod_.process(in, n, soft_);
        dec_.setCarrierLock(demod_.locked());
        if (!soft_.empty()) dec_.pushSoft(soft_.data(), (int)soft_.size());
    }

    PskDemod& demod() { return demod_; }
    LrptDecoder& decoder() { return dec_; }

private:
    PskDemod demod_;
    LrptDecoder dec_;
    std::vector<int8_t> soft_;
};
