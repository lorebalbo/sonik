#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Deck/DeckIdentifiers.h"
#include <vector>

class StemToggleComponent : public juce::Component,
                            public juce::ValueTree::Listener,
                            public juce::SettableTooltipClient
{
public:
    StemToggleComponent (juce::ValueTree stemsNode, const juce::String& label,
                         std::vector<juce::Identifier> propertyIds,
                         const juce::String& tooltip = {});
    ~StemToggleComponent() override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    void refreshState();

    juce::ValueTree stemsNode;
    juce::ValueTree deckNode;   // parent of stemsNode; carries sourceMode (PRD-0062)
    juce::String labelText;
    std::vector<juce::Identifier> propIds;
    bool isMuted = false;
    bool isReady = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemToggleComponent)
};
