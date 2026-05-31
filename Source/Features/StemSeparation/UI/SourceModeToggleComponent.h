#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Deck/DeckIdentifiers.h"

class AudioEngine;

// =============================================================================
// SourceModeToggleComponent (PRD-0062)
//
// A tactile two-segment ORIG / STEMS selector that chooses which source the
// deck plays. The deck's `sourceMode` ValueTree property is the single source
// of truth (§1.5.7); clicking writes that property AND calls
// AudioEngine::setDeckSourceMode for an immediate, click-free audio response.
//
// The STEMS segment is locked (greyed, inert) until the deck has a ready stem
// set (Stems.status == "ready"); until then the deck is pinned to ORIG.
// =============================================================================
class SourceModeToggleComponent : public juce::Component,
                                  public juce::ValueTree::Listener,
                                  public juce::SettableTooltipClient
{
public:
    SourceModeToggleComponent (juce::ValueTree deckTree,
                               AudioEngine& engine,
                               const juce::String& deckId);
    ~SourceModeToggleComponent() override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    void refreshState();
    void selectMode (bool useStems);

    juce::ValueTree tree;
    juce::ValueTree stemsNode;
    AudioEngine& audioEngine;
    juce::String deckId;

    bool stemsReady = false;   // Stems.status == "ready"
    bool onStems    = false;   // sourceMode == "stems"

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SourceModeToggleComponent)
};
