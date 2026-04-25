#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../Deck/DeckIdentifiers.h"
#include <functional>

/// Beat jump UI strip: [◄] [2] [4] [8] [16] [►]
/// Clicking a size button selects it (writes beatJumpSize to the ValueTree).
/// Arrows trigger onJumpBackward / onJumpForward.
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

    // 6 buttons: idx 0=Backward, 1-4=Size(2/4/8/16), 5=Forward
    enum class Region { None, Backward, Size0, Size1, Size2, Size3, Forward };

    Region  getRegionAt (int x, int y) const;
    juce::Rectangle<int> getButtonBounds (int idx) const;
    static int regionToSizeIndex (Region r) noexcept;

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

    // Fixed button sizes per DESIGN.md (50px standard, 25px arrow)
    static constexpr int kArrowW   = 25;   // ◄ and ► buttons
    static constexpr int kBtnW     = 50;   // size buttons
    static constexpr int kBtnH     = 46;
    static constexpr int kBorderW  = 2;
    // Total: 2×25 + 4×50 − 5×2 = 240 px
    static constexpr int kTotalW   = 2 * kArrowW + 4 * kBtnW - 5 * kBorderW;

    static constexpr int kNumSizes = 4;
    static constexpr double kSizes[kNumSizes] = { 2.0, 4.0, 8.0, 16.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatJumpComponent)
};
