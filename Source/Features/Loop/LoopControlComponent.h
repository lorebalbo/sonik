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

    enum class BtnType { LoopIn, LoopOut, Toggle, Halve, Double, AutoLoop };

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

    static constexpr int buttonGap  = 2;
    static constexpr int numButtons = 12;

    static inline const ButtonDef buttons[] = {
        { "IN",   BtnType::LoopIn,   0.0f },
        { "OUT",  BtnType::LoopOut,  0.0f },
        { "LOOP", BtnType::Toggle,   0.0f },
        { "/2",   BtnType::Halve,    0.0f },
        { "x2",   BtnType::Double,   0.0f },
        { "1/2",  BtnType::AutoLoop, 0.5f },
        { "1",    BtnType::AutoLoop, 1.0f },
        { "2",    BtnType::AutoLoop, 2.0f },
        { "4",    BtnType::AutoLoop, 4.0f },
        { "8",    BtnType::AutoLoop, 8.0f },
        { "16",   BtnType::AutoLoop, 16.0f },
        { "32",   BtnType::AutoLoop, 32.0f },
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopControlComponent)
};
