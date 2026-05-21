#pragma once
//==============================================================================
// PRD-0052: MixerMidiHandler — routes global mixer MIDI events to the mixer
// ValueTree sub-tree managed by MixerStateSchema.
//
// Handles both the pre-existing global controls (crossfader, master gain,
// headphones) and the new per-channel strip controls introduced in PRD-0052.
//==============================================================================

#include "../Features/Midi/MidiMessageEvent.h"
#include "../Features/Midi/SoftTakeoverManager.h"
#include "../Features/Mixer/State/MixerStateSchema.h"
#include "../Features/Mixer/State/MixerParam.h"
#include "../Features/Mixer/State/MixerIdentifiers.h"
#include <juce_data_structures/juce_data_structures.h>

class MixerMidiHandler final
{
public:
    /** PRD-0061: soft-takeover is enforced for every continuous mixer
        control. The handler queries the manager on each inbound continuous
        event; absent injection (legacy call-sites in tests pre-dating
        PRD-0061) the handler still works but writes go through directly. */
    explicit MixerMidiHandler (sonik::midi::SoftTakeoverManager* softTakeoverIn = nullptr) noexcept
        : softTakeover (softTakeoverIn) {}

    bool tryHandle (const sonik::midi::MidiMessageEvent& event);

    /** Supply the mixer schema so channel-strip events can be written.
        Call once from SonikApplication after MixerStateSchema exists. */
    void setMixerStateSchema (MixerStateSchema* schema) noexcept { mixerSchema = schema; }

    /** Supply the root state tree for legacy global-mixer writes (crossfader,
        master gain, headphones gain) that are stored on the root tree or a
        child managed outside MixerStateSchema.
        Call once from SonikApplication after the state tree exists. */
    void setStateTree (juce::ValueTree rootState) noexcept { stateTree = std::move (rootState); }

    /** Injects the SoftTakeoverManager post-construction (used by
        SonikApplication when both objects exist). */
    void setSoftTakeoverManager (sonik::midi::SoftTakeoverManager* mgr) noexcept
    {
        softTakeover = mgr;
    }

private:
    MixerStateSchema* mixerSchema { nullptr };
    juce::ValueTree   stateTree;
    sonik::midi::SoftTakeoverManager* softTakeover { nullptr };

    /** Returns true if the event should pass through soft-takeover (or if
        soft-takeover is not configured / not engaged for this category).
        `currentSoftwareNorm` must be the on-screen value mapped into [0,1]
        in the same space as `event.normalisedValue`. */
    bool passSoftTakeover (const sonik::midi::MidiMessageEvent& event,
                            sonik::midi::MidiTargetCategory category,
                            float currentSoftwareNorm) noexcept;

    void applyChannelContinuous (int channelIdx,
                                  const juce::Identifier& prop,
                                  float normalisedValue) noexcept;
    void applyChannelToggle     (int channelIdx,
                                  const juce::Identifier& prop,
                                  float normalisedValue) noexcept;
    void applyChannelEqContinuous (int channelIdx,
                                    const juce::Identifier& prop,
                                    float nativeValue) noexcept;
    void applyChannelEqToggle     (int channelIdx,
                                    const juce::Identifier& prop,
                                    float normalisedValue) noexcept;
};
