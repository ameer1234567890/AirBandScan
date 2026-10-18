// Meteor-M LRPT link layer: soft QPSK symbols -> images.
//
//   soft symbols --(phase / IQ ambiguity search)--> Viterbi r=1/2 K=7
//   --> NRZ-M decode (M2-3 / M2-4) --> ASM 1ACFFC1D frame sync (1024 byte CADU)
//   --> CCSDS derandomise --> RS(255,223) x4 --> VCDU / M_PDU packet demux
//   --> MSU-MR imager
#pragma once

#include "sat/msumr.h"
#include "sat/reed_solomon.h"
#include "sat/viterbi.h"

#include <cstdint>
#include <vector>

class LrptDecoder
{
public:
    explicit LrptDecoder(bool nrzm = true) : nrzm_(nrzm) {}

    void reset();
    // Interleaved soft symbols (I, Q, I, Q...), positive = bit 0.
    void pushSoft(const int8_t* soft, int n);
    // Already decoded CADUs (1024 bytes each, derandomised, e.g. SatDump .cadu).
    void pushCadu(const uint8_t* cadu, int n, bool rsCheck = true);
    // Demodulator carrier lock: the (costly) ambiguity search only runs with it.
    void setCarrierLock(bool on) { carrier_ = on; }

    bool locked() const { return locked_; }
    bool frameSync() const { return syncState_ == 2; }
    double ber() const { return ber_; }
    uint64_t frames() const { return framesOk_; }
    uint64_t framesBad() const { return framesBad_; }
    uint64_t packets() const { return packets_; }
    int lastScid() const { return scid_; }
    MsuMrImager& imager() { return img_; }
    const MsuMrImager& imager() const { return img_; }

private:
    double testBer(const int8_t* soft, int h) const;
    int searchLock(const int8_t* soft, double& ber) const;
    void transform(const int8_t* in, int pairs, int8_t* out, int h) const;
    void bits(const std::vector<uint8_t>& b);
    void frame(uint8_t* f);              // 1024 byte CADU after sync
    void vcdu(const uint8_t* v);         // 892 bytes, RS checked
    void packet(const uint8_t* p, size_t len);

    bool nrzm_;
    std::vector<int8_t> pend_;
    bool locked_ = false;
    bool carrier_ = true;
    unsigned checkN_ = 0;
    int hyp_ = 0;                        // ambiguity hypothesis (shift, swap, rotation)
    double ber_ = 0.5;
    uint64_t sinceFrame_ = 0;            // soft symbols since the last good frame
    Viterbi27 vit_;
    std::vector<uint8_t> vbits_;
    uint8_t lastBit_ = 0;

    // frame sync
    uint32_t sr_ = 0;
    int syncState_ = 0;                  // 0 search, 1/2 locked (2 = confirmed)
    bool invert_ = false;
    int misses_ = 0;
    std::vector<uint8_t> fbits_;         // collected frame bits
    uint8_t cadu_[1024];

    ReedSolomon rs_;
    uint64_t framesOk_ = 0, framesBad_ = 0, packets_ = 0;
    int scid_ = -1;

    // demux (VCID 5 carries MSU-MR imagery; others are demuxed the same way)
    struct Vc
    {
        std::vector<uint8_t> buf;
        uint32_t counter = 0;
        bool have = false;
    };
    Vc vc_[64];
    MsuMrImager img_;
};
