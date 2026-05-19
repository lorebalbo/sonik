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
#include <cstdint>

namespace sonik::midi { struct MidiMessageEvent; class SoftTakeoverManager; }

class BeatJumpEngine;
class LoopEngine;
class HotCueManager;
class AudioEngine;

class DeckMidiHandler final
{
public:
    DeckMidiHandler (DeckStateManager& deckState,
                     sonik::midi::SoftTakeoverManager& softTakeover);

    /** Returns true if the event was consumed (a per-deck category we recognise),
        false otherwise so the composite can try another sub-handler. */
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);

    /** Set the audio engine — needed for TransportCue and PositionSeek.
        Called once from SonikApplication after both objects are constructed. */
    void setAudioEngine (AudioEngine* engine) noexcept { audioEngine = engine; }

    /** Register per-deck feature engines once DeckShellComponent is ready.
        deckIndex is 0..3, matching MidiMessageEvent::deckIndex. */
    void registerDeckEngines (std::uint8_t deckIndex,
                               BeatJumpEngine* beatJump,
                               LoopEngine*     loop,
                               HotCueManager*  hotCue) noexcept;

    /** Deregister engines when a DeckShellComponent is destroyed. */
    void deregisterDeckEngines (std::uint8_t deckIndex) noexcept;

private:
    juce::ValueTree deckTreeFor (std::uint8_t deckIndex) const;
    static bool isPress (float normalisedValue) noexcept { return normalisedValue >= 0.5f; }

    void toggleBool (juce::ValueTree& deckTree, const juce::Identifier& id);

    DeckStateManager&                 deckState;
    sonik::midi::SoftTakeoverManager& softTakeover;
    AudioEngine*                      audioEngine = nullptr;

    BeatJumpEngine* beatJumpEngines[4] {};
    LoopEngine*     loopEngines[4]     {};
    HotCueManager*  hotCueManagers[4]  {};
};
