#pragma once
//==============================================================================
// PRD-0044: MixerMidiHandler — routes global mixer MIDI events to the state
// tree. No Mixer feature DSP exists yet; values are written to ValueTree and
// will be picked up by the audio engine once the Mixer feature lands.
//==============================================================================

#include "../Features/Midi/MidiMessageEvent.h"
#include <juce_data_structures/juce_data_structures.h>

class MixerMidiHandler final
{
public:
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);

    /** Set the root state tree so mixer properties can be written.
        Call once from SonikApplication after the state tree exists. */
    void setStateTree (juce::ValueTree rootState) noexcept { stateTree = std::move (rootState); }

private:
    juce::ValueTree stateTree;
};
