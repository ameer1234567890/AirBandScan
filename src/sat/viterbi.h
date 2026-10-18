// Soft-decision Viterbi decoder for the CCSDS / NASA rate 1/2, K = 7
// convolutional code (G1 = 171, G2 = 133 octal; 0x4F / 0x6D with the newest
// bit in the LSB). Streaming: bits come out ~kTraceback steps late.
#pragma once

#include <cstdint>
#include <vector>

class Viterbi27
{
public:
    static constexpr int kTraceback = 96;
    static constexpr int kChunk = 256;

    Viterbi27() { reset(); }
    void reset();

    // soft: interleaved symbol pairs, positive = bit 0, |v| <= 127.
    // Appends decoded bits (0/1 per byte).
    void decode(const int8_t* soft, int pairs, std::vector<uint8_t>& bits);
    // Emits the bits still held in the traceback window.
    void flush(std::vector<uint8_t>& bits);

    // Encoder (tests and the lock search's re-encoding). state: 7-bit register.
    static void encode(const uint8_t* bits, int n, uint32_t& state, std::vector<uint8_t>& out);

private:
    void traceback(int count, std::vector<uint8_t>& bits);

    int32_t pm_[64];
    std::vector<uint64_t> dec_;
};
