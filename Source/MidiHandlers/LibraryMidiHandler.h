#pragma once
//==============================================================================
// PRD-0044: LibraryMidiHandler — placeholder for the library categories.
// Wiring to the LibraryComponent's selection/load APIs lands in PRD-0048
// alongside the MIDI Learn UI. For now this handler returns "unhandled" so
// the composite emits a one-shot warning per category.
//==============================================================================

#include "../Features/Midi/MidiMessageEvent.h"

class LibraryMidiHandler final
{
public:
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);
};
