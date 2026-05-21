#pragma once
//==============================================================================
// PRD-0053 + PRD-0057: ABBus — accumulates channel-strip outputs into the
// stereo A and B summing buses based on per-channel assign flags.
//
// Header-only stateless helper. The AudioEngine owns the bus scratch buffers
// (pre-zeroed once per block); this class merely encapsulates the
// per-channel accumulation logic.
//
// PRD-0057 §1.4 routing law (per channel):
//    assignA && !assignB  → channel sums into bus A only
//   !assignA &&  assignB  → channel sums into bus B only
//    assignA &&  assignB  → channel sums into both buses at unity (dual-
//                           assign / "thru" — see PRD-0057 §1.5.4 for the
//                           +3 dB headroom implication on the smooth curve)
//   !assignA && !assignB  → channel is silent at the crossfader output
//                           (PRD-0057 §1.5.3)
//
// Audio-thread contract: no allocation, no locks, no I/O.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

class ABBus
{
public:
    /// API-symmetry no-op (the bus is stateless and pre-zeroed by the engine).
    /// Provided so every mixer pipeline stage exposes a uniform `prepareToPlay`
    /// surface for downstream PRDs and tests, per PRD-0053 §1.4 AC3.
    static void prepareToPlay (double /*sampleRate*/, int /*blockSize*/, int /*numChannels*/) noexcept {}

    /// Accumulate one channel's output into bus A and/or bus B per the
    /// assign flags. See PRD-0057 §1.4 truth table above.
    static void accumulate (const float* channelL,
                            const float* channelR,
                            bool         assignA,
                            bool         assignB,
                            float*       busAL,
                            float*       busAR,
                            float*       busBL,
                            float*       busBR,
                            int          numSamples) noexcept
    {
        if (assignA)
        {
            juce::FloatVectorOperations::add (busAL, channelL, numSamples);
            juce::FloatVectorOperations::add (busAR, channelR, numSamples);
        }
        if (assignB)
        {
            juce::FloatVectorOperations::add (busBL, channelL, numSamples);
            juce::FloatVectorOperations::add (busBR, channelR, numSamples);
        }
        // Both flags false → channel intentionally silent at master (§1.5.3).
    }
};
