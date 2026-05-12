#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../../Deck/DeckIdentifiers.h"

/// SYNC latch button (PRD-0027).
///
/// Visual states (DESIGN.md — strict monochrome, zero border-radius):
///   Active   (isSynced=true):  fill #2d2d2d, text #fdfdfd, 2px #2d2d2d border
///   Inactive (isSynced=false): fill #fdfdfd, text #2d2d2d, 2px #2d2d2d border
///
/// clicked(): toggles IDs::isSynced on the deck's ValueTree (message thread only).
/// Exception: if IDs::isMaster is true on this deck, click is a no-op.
class SyncButton final : public juce::Button,
                          private juce::ValueTree::Listener
{
public:
    explicit SyncButton (juce::ValueTree deckState);
    ~SyncButton() override;

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

    juce::ValueTree deckState_;
    bool isSynced_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncButton)
};
