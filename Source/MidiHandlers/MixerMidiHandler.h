#pragma once
//==============================================================================
// PRD-0044: MixerMidiHandler — placeholder for the global mixer categories.
// No Mixer feature module exists yet; this handler logs a one-shot warning
// for every recognised mixer category and otherwise no-ops, exactly as the
// PRD specifies.
//==============================================================================

#include "../Features/Midi/MidiMessageEvent.h"

class MixerMidiHandler final
{
public:
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);
};
