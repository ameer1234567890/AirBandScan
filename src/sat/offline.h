// Command line decoding of recordings (no SDR, no web server).
#pragma once

#include <string>

// in: IQ baseband WAV centred on the signal (any rate >= 150 kS/s),
//     8-bit signed soft symbols (.s / .soft) or decoded CADUs (.cadu).
// variant: "m2x" (Meteor-M N2-3 / N2-4: OQPSK, NRZ-M) or "m2" (QPSK).
int decodeLrptFile(const std::string& in, const std::string& outDir, const std::string& variant);

// in: audio WAV (FM receiver output, any rate >= 8 kHz).
int decodeSstvFile(const std::string& in, const std::string& outDir);
