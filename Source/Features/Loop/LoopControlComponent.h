#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../Deck/DeckIdentifiers.h"
#include <functional>

class LoopControlComponent final : public juce::Component,
                                    private juce::ValueTree::Listener
{
public:
    explicit LoopControlComponent (juce::ValueTree deckTree);
    ~LoopControlComponent() override;

    // Callbacks wired by DeckShellComponent
    std::function<void (float)> onAutoLoop;
    std::function<void()> onLoopIn;
    std::function<void()> onLoopOut;
    std::function<void()> onToggleLoop;
    std::function<void()> onReLoop;
    std::function<void()> onLoopHalve;
    std::function<void()> onLoopDouble;

    void setActiveAutoLoopBeats (float beats);
    void setPendingLoopIn (bool pending);

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

    bool isDeckEmpty() const;

    enum class BtnType { LoopIn, LoopOut, Toggle, ArrowLeft, ArrowRight, AutoLoop };

    struct ButtonDef
    {
        juce::String label;
        BtnType      type;
        float        autoLoopBeats = 0.0f;
    };

    juce::Rectangle<int> getButtonBounds (int index) const;
    int getButtonAt (int x, int y) const;

    juce::ValueTree deckTree;
    juce::ValueTree loopNode;

    float activeAutoBeats = 0.0f;
    bool  pendingIn       = false;
    bool  loopIsActive    = false;
    bool  loopIsDefined   = false;
    int   hoveredButton   = -1;

    // Visible buttons: [IN|OUT|LOOP] gap [<|2|4|8|16|>]
    // Buttons /2, x2, 1/2, 1, 32 are hidden from the UI but their callbacks
    // remain wired (< maps to halve, > maps to double).
    static constexpr int numButtons = 9;

    // Fixed button sizes — matches GRID section (50 × 46 px).
    // Arrow buttons (< and >) are half-width.
    static constexpr int kBtnW    = 50;
    static constexpr int kArrowW  = 25;   // half of kBtnW
    static constexpr int kBtnH    = 46;
    static constexpr int kBorderW = 2;    // shared border between adjacent buttons
    static constexpr int kGroupGap = 8;   // gap between IN/OUT/LOOP group and beat group

    // Pre-computed group widths (accounting for shared 2-px borders)
    static constexpr int kGroupAW = 3 * kBtnW  - 2 * kBorderW;              // 146
    static constexpr int kGroupBW = 2 * kArrowW + 4 * kBtnW - 5 * kBorderW; // 240
    static constexpr int kTotalW  = kGroupAW + kGroupGap + kGroupBW;         // 394

    static inline const ButtonDef buttons[] = {
        { "IN",   BtnType::LoopIn,     0.0f },  // 0
        { "OUT",  BtnType::LoopOut,    0.0f },  // 1
        { "LOOP", BtnType::Toggle,     0.0f },  // 2
        { "<",    BtnType::ArrowLeft,  0.0f },  // 3  (halve)
        { "2",    BtnType::AutoLoop,   2.0f },  // 4
        { "4",    BtnType::AutoLoop,   4.0f },  // 5
        { "8",    BtnType::AutoLoop,   8.0f },  // 6
        { "16",   BtnType::AutoLoop,  16.0f },  // 7
        { ">",    BtnType::ArrowRight, 0.0f },  // 8  (double)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopControlComponent)
};
