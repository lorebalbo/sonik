#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Deck/DeckIdentifiers.h"

class StemSeparationManager;
class AudioEngine;

class StemSeparateButton : public juce::Component,
                           public juce::ValueTree::Listener,
                           public juce::SettableTooltipClient,
                           private juce::Timer
{
public:
    StemSeparateButton (juce::ValueTree deckTree, StemSeparationManager& mgr,
                        AudioEngine& engine, const juce::String& deckId);
    ~StemSeparateButton() override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    void refreshState();
    void timerCallback() override;

    juce::ValueTree tree;
    juce::ValueTree stemsNode;
    StemSeparationManager& stemManager;
    AudioEngine& audioEngine;
    juce::String deckId;

    juce::String currentStatus = "none";
    float currentProgress = 0.0f;
    bool isEmpty = true;
    bool isShortTrack = false;
    int consecutiveErrors = 0;

    // ── Animated progress state ──────────────────────────────────────────
    // animatedProgress is what we actually display.  It races ahead of the
    // real progress to ~85 % quickly, then idles until the process finishes.
    float  animatedProgress    = 0.0f;
    double separationStartMs   = 0.0;
    int    currentLabelIndex   = 0;
    // Cycle through these every 3 seconds while separating
    static const juce::StringArray& phaseLabels() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparateButton)
};
