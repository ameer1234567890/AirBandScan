// Meteor-M MSU-MR imagery from LRPT CCSDS packets (APID 64..69 = channels
// 1..6). Each packet carries 14 JPEG-like 8x8 blocks of one 8-pixel image
// strip; 14 packets make a 1568 pixel wide strip per channel. Strips of
// different channels are matched by their on-board timestamp (identical for
// all channels of one strip, ~1.23 s apart).
#pragma once

#include "sat/image.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

class MsuMrImager
{
public:
    static constexpr int kWidth = 1568;
    static constexpr int kChannels = 6;

    void reset();
    // payload: packet data after the 6 byte primary header.
    // Returns false when the packet is not valid MSU-MR imagery.
    bool addPacket(int apid, const uint8_t* payload, int len);

    int strips() const { return (int)lines_.size(); }
    int rows() const;                   // image height in pixels
    std::vector<int> channels() const;  // 1-based channel numbers present
    uint64_t segments() const { return segments_; }

    Image channel(int ch) const;        // ch 1..6, grey
    // "221" false colour (channels 2, 2, 1) when available, otherwise the
    // best single channel. Empty if there is nothing yet.
    Image composite() const;

private:
    struct Strip
    {
        std::array<std::unique_ptr<uint8_t[]>, kChannels> px; // 8 x kWidth each
    };
    std::vector<std::pair<int64_t, int>> layout() const; // (time key, row strip index)

    std::map<int64_t, Strip> lines_;
    uint64_t segments_ = 0;
    std::array<bool, kChannels> seen_{};
};
