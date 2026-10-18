// CCSDS Reed-Solomon (255,223): GF(2^8) with x^8+x^7+x^2+x+1, first
// consecutive root 112, primitive element alpha^11, conventional (not dual)
// basis, as used by Meteor-M LRPT. Corrects up to 16 byte errors.
#pragma once

#include <cstdint>

class ReedSolomon
{
public:
    ReedSolomon();

    // Decodes one 255 byte codeword in place (data[0] is the first byte
    // sent). Returns the number of corrected bytes, -1 if uncorrectable.
    int decode(uint8_t* cw) const;
    // Fills cw[223..254] with parity for cw[0..222] (used by the tests).
    void encode(uint8_t* cw) const;

    // Interleaved block of depth i (bytes k*i + n belong to codeword n).
    // Returns the total corrected count, -1 if any codeword failed.
    int decodeInterleaved(uint8_t* block, int depth) const;

private:
    uint8_t mul(uint8_t a, uint8_t b) const
    {
        return (a && b) ? exp_[log_[a] + log_[b]] : 0;
    }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; } // alpha^e

    uint8_t exp_[512];
    int log_[256];
    uint8_t gen_[33]; // generator polynomial, gen_[0] = x^32 coefficient
};
