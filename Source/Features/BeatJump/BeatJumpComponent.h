#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../Deck/DeckIdentifiers.h"
#include <functional>

/// Beat jump UI strip: [<] [size] [>]
/// Monochrome style per DESIGN.md: #000000/#F9F9F9, zero border-radius.
class BeatJumpComponent final : public juce::Component,
                                 public juce::SettableTooltipClient,
                                 private juce::ValueTree::Listener
{
public:
    explicit BeatJumpComponent (juce::ValueTree deckTree);
    ~BeatJumpComponent() override;

    std::function<void()> onJumpForward;
    std::function<void()> onJumpBackward;
    std::function<void (bool forward)> onCycleSize;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    enum class Region { None, Backward, Size, Forward };

    Region getRegionAt (int x, int y) const;
    juce::Rectangle<int> getBackwardBounds() const;
    juce::Rectangle<int> getSizeBounds() const;
    juce::Rectangle<int> getForwardBounds() const;

    juce::String formatSize (double beats) const;
    bool isDeckEmpty() const;
    bool hasBeatgrid() const;

    void triggerFlash (Region r);

    juce::ValueTree deckTree;
    double  currentSize   = 4.0;
    Region  hoveredRegion = Region::None;

    // Flash state for button press feedback (~100ms)
    Region  flashRegion    = Region::None;
    int64_t flashStartTime = 0;
    static constexpr int flashDurationMs = 100;

    static constexpr int buttonGap = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatJumpComponent)
};
