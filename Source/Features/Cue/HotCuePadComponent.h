#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "HotCueData.h"
#include "../Deck/DeckIdentifiers.h"
#include <functional>

class HotCuePadComponent final : public juce::Component,
                                  private juce::ValueTree::Listener
{
public:
    explicit HotCuePadComponent (juce::ValueTree deckTree);
    ~HotCuePadComponent() override;

    // Callbacks wired by DeckShellComponent
    std::function<void (int)>                      onSetCue;
    std::function<void (int)>                      onTriggerCue;
    std::function<void (int)>                      onDeleteCue;
    std::function<void()>                          onUndoDelete;
    std::function<void (int, int)>                 onColorChange;
    std::function<void (int, const juce::String&)> onLabelChange;

    void paint (juce::Graphics& g) override;
    void resized() override {}
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    int getPadIndexAt (int x, int y) const;
    juce::Rectangle<int> getPadBounds (int padIndex) const;
    void showColorLabelPopup (int padIndex);

    bool isDeckEmpty() const;
    bool isCueActive (int padIndex) const;
    int  getCueColorIndex (int padIndex) const;
    juce::String getCueLabel (int padIndex) const;

    juce::ValueTree deckTree;
    juce::ValueTree cuePointsNode;

    int hoveredPad = -1;
    int pressedPad = -1;

    static constexpr int numPads    = 8;

    // Fixed pad sizes — match GRID buttons (50 × 46 px)
    static constexpr int kPadW     = 50;
    static constexpr int kPadH     = 46;
    static constexpr int padBorderW = 2;   // shared border between adjacent pads
    // Total strip width: 8×50 − 7×2 = 386 px
    static constexpr int kTotalW   = numPads * kPadW - (numPads - 1) * padBorderW;
    static constexpr char padLetters[] = "ABCDEFGH";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotCuePadComponent)
};
