#pragma once
//==============================================================================
// PRD-0044: DeckMidiHandler — translates resolved per-deck MIDI events into
// ValueTree writes (and, where wiring exists, Feature-module method calls).
//
// THREAD: Message thread only.
//==============================================================================

#include "../Features/Deck/DeckStateManager.h"
#include "../Features/Midi/MidiCommandHandler.h"
#include "../Features/Midi/MidiMessageEvent.h"

#include <bitset>

namespace sonik::midi { struct MidiMessageEvent; class SoftTakeoverManager; }

class DeckMidiHandler final
{
public:
    DeckMidiHandler (DeckStateManager& deckState,
                     sonik::midi::SoftTakeoverManager& softTakeover);

    /** Returns true if the event was consumed (a per-deck category we recognise),
        false otherwise so the composite can try another sub-handler. */
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);

private:
    juce::ValueTree deckTreeFor (std::uint8_t deckIndex) const;
    static bool isPress (float normalisedValue) noexcept { return normalisedValue >= 0.5f; }

    void toggleBool (juce::ValueTree& deckTree, const juce::Identifier& id);

    DeckStateManager&                 deckState;
    sonik::midi::SoftTakeoverManager& softTakeover;
};
