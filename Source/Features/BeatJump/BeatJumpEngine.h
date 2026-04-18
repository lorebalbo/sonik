#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "../AudioEngine/AudioEngine.h"
#include "../Deck/DeckIdentifiers.h"
#include "../Deck/AudioThreadState.h"
#include "../Quantize/QuantizeService.h"

class LoopEngine;

/// Per-deck beat jump engine (message thread only).
/// Pre-computes jump destination and issues seekDeck to the audio engine.
/// When a loop is active, delegates to LoopEngine::shiftLoop for loop shift.
class BeatJumpEngine final
{
public:
    BeatJumpEngine (juce::ValueTree deckTree,
                    AudioEngine& engine,
                    const juce::String& deckId);
    ~BeatJumpEngine() = default;

    void setAudioState (DeckAudioState* state);
    void setLoopEngine (LoopEngine* engine);

    void jumpForward();
    void jumpBackward();

    void setJumpSize (double beats);
    void cycleJumpSize (bool forward);

    /// Available jump sizes in beats.
    static constexpr double jumpSizes[] = { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    static constexpr int numJumpSizes = 7;

private:
    void executeJump (bool forward);
    double getBeatInterval() const;
    double getSampleRate() const;
    int64_t readPlayhead() const;

    juce::ValueTree   tree;
    juce::ValueTree   trackMetaNode;
    juce::ValueTree   beatGridNode;
    juce::ValueTree   loopNode;
    AudioEngine&      audioEngine;
    juce::String      deckId;
    DeckAudioState*   audioState   = nullptr;
    LoopEngine*       loopEngine   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatJumpEngine)
};
