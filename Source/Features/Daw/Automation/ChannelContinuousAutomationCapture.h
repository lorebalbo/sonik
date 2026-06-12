#pragma once
//==============================================================================
// PRD-0090: Per-Channel Continuous Automation Lanes — Volume / Filter / High /
// Mid / Low / Gain.
//
// Wires the six authoritative per-channel mixer parameters of all four channels
// (twenty-four continuous lanes) into the generic PRD-0088 capture taps. This is pure
// registration glue: it binds each lane key (channel letter, parameter id) to
// its authoritative mixer ValueTree node + property and applies the per-parameter
// capture-time decimation threshold (§1.5.1). Seeding the initial breakpoint at
// record start and flushing the resting value at record stop are handled by the
// shared AutomationCaptureTaps (captureInitialValues / flush).
//
// No audio-thread code. The mixer parameters are observed on the message thread
// exactly as PRD-0088 prescribes; values are stored verbatim in native units
// (filter bipolar [-1,+1] post-detent, EQ/gain in dB) per §1.5.5 / §1.5.7.
//==============================================================================

#include "AutomationCaptureTaps.h"
#include "AutomationIds.h"

#include "../../Mixer/State/MixerStateSchema.h"
#include "../../Mixer/State/MixerIdentifiers.h"

namespace Daw
{

struct ChannelContinuousAutomationCapture
{
    // Per-parameter capture-time decimation thresholds, in the parameter's own
    // units (§1.5.1). Kept in one block so they can be tuned without touching
    // capture logic.
    static constexpr double kFilterDeadband = 0.01;  // bipolar [-1,+1]
    static constexpr double kEqDeadbandDb   = 0.25;  // dB per band
    static constexpr double kGainDeadbandDb = 0.25;  // dB
    static constexpr double kVolumeDeadband = 0.01;  // linear fader [0,1]

    // Registers the twenty-four continuous taps into `taps`, bound to the
    // authoritative mixer ValueTree nodes owned by `mixer`. Channel-id → channel
    // group is the identity mapping A→A … D→D (§1.5.4).
    static void registerTaps (AutomationCaptureTaps& taps, MixerStateSchema& mixer)
    {
        static const char* const kChannelOwners[4] = { "A", "B", "C", "D" };

        for (int ch = 0; ch < 4; ++ch)
        {
            const juce::String owner (kChannelOwners[ch]);
            auto channelTree = mixer.getChannelTree (ch);
            auto eqTree      = mixer.getChannelEqTree (ch);

            // Filter (bipolar), gain (dB) and the volume fader (linear [0,1])
            // live directly on the channel node.
            taps.registerContinuousTap (channelTree, MixerIDs::filter, owner, "filter",
                                        Interpolation::Linear, { kFilterDeadband });
            taps.registerContinuousTap (channelTree, MixerIDs::gain, owner, "gain",
                                        Interpolation::Linear, { kGainDeadbandDb });
            taps.registerContinuousTap (channelTree, MixerIDs::fader, owner, "volume",
                                        Interpolation::Linear, { kVolumeDeadband });

            // The three EQ bands live on the channel's nested "eq" node, each as
            // its own separate lane (§1.5.3).
            taps.registerContinuousTap (eqTree, MixerIDs::high, owner, "eq.high",
                                        Interpolation::Linear, { kEqDeadbandDb });
            taps.registerContinuousTap (eqTree, MixerIDs::mid, owner, "eq.mid",
                                        Interpolation::Linear, { kEqDeadbandDb });
            taps.registerContinuousTap (eqTree, MixerIDs::low, owner, "eq.low",
                                        Interpolation::Linear, { kEqDeadbandDb });
        }
    }
};

} // namespace Daw
