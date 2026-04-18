#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Deck/DeckIdentifiers.h"
#include "../Deck/AudioThreadState.h"

class AudioEngine;

// ---------------------------------------------------------------------------
// SlipButtonComponent (PRD-0017)
//
// Toggle slip mode on/off.  Dual-interaction:
//   - Short press (<300 ms) while displaced → snap-back (slip return)
//   - Long press (≥300 ms) or any press when not displaced → toggle
//
// Polls DeckAudioState::slipDisplaced on a 30 Hz timer for pulsing
// displaced indicator.
// ---------------------------------------------------------------------------
class SlipButtonComponent : public juce::Component,
                            public juce::ValueTree::Listener,
                            public juce::SettableTooltipClient,
                            private juce::Timer
{
public:
    SlipButtonComponent (juce::ValueTree deckTree,
                         DeckAudioState* audioState,
                         AudioEngine& engine,
                         const juce::String& deckId);
    ~SlipButtonComponent() override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& t,
                                   const juce::Identifier& p) override;

private:
    void timerCallback() override;

    juce::ValueTree tree;
    DeckAudioState* audioState = nullptr;
    AudioEngine& audioEngine;
    juce::String deckId;

    bool slipEnabled  = false;
    bool slipDisplaced = false;
    bool isEmpty       = false;

    juce::int64 mouseDownTime = 0;

    // Pulsing animation state
    float pulsePhase  = 0.0f;
    static constexpr float kPulseSpeed = 0.15f; // radians per tick (~30 Hz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlipButtonComponent)
};
