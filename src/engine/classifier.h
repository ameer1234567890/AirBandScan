// Speech / not-speech decision for a finished AM transmission.
//
// sdr-monitor does this with a TensorFlow (YAMNet) model in Python; here it is
// a small, dependency-free feature classifier that works on the demodulated
// 8 kHz audio (already normalised to modulation index by AmDemod):
//
//   voiced fraction  - share of 20 ms frames with a clear 80-400 Hz pitch
//                      (normalised autocorrelation) and a speech-like
//                      zero-crossing rate. Carriers, noise and data tones fail.
//   level variation  - spread (dB) of frame loudness; speech has syllables
//                      and pauses, a dead carrier or a data burst is steady.
//   modulation depth - mean modulation; an unmodulated carrier is ~0.
#pragma once

#include <string>
#include <vector>

struct VoiceVerdict
{
    bool   voice = false;
    double score = 0.0;       // 0..1
    double voicedFrac = 0.0;  // 0..1
    double levelSpreadDb = 0.0;
    double modDepth = 0.0;
    std::string reason;       // human readable, shown on the "not decoded" page
};

// carrier: carrier amplitude per audio sample; frames where the carrier is
// down (before/after the transmission) are left out of the analysis.
VoiceVerdict classifyVoice(const std::vector<float>& audio8k, const std::vector<float>& carrier,
                           double threshold);
