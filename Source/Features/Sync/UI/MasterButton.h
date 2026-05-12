#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../../Deck/DeckIdentifiers.h"
#include "../MasterClockManager.h"

/// MASTER assignment button (PRD-0027).
///
/// Visual states (DESIGN.md — strict monochrome, zero border-radius):
///   Active   (isMaster=true):  fill #2d2d2d, text #fdfdfd, 2px #2d2d2d border
///   Inactive (isMaster=false): fill #fdfdfd, text #2d2d2d, 2px #2d2d2d border
///
/// clicked(): calls MasterClockManager::setMaster(deckIndex) on the message thread.
/// No-op when this deck already has isMaster=true (pressing active MASTER button does nothing).
class MasterButton final : public juce::Button,
                            private juce::ValueTree::Listener
{
public:
    MasterButton (juce::ValueTree deckState, MasterClockManager& manager, int deckIndex);
    ~MasterButton() override;

    void paintButton (juce::Graphics& g,
                      bool isMouseOver,
                      bool isButtonDown) override;

    void clicked() override;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    juce::ValueTree     deckState_;
    MasterClockManager& manager_;
    int                 deckIndex_;
    bool                isMaster_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterButton)
};
