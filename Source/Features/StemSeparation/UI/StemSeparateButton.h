#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Deck/DeckIdentifiers.h"

class StemSeparationManager;
class AudioEngine;

class StemSeparateButton : public juce::Component,
                           public juce::ValueTree::Listener,
                           public juce::SettableTooltipClient
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparateButton)
};
