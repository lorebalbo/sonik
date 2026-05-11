#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

/// Atom: a single sidebar navigation item. Renders all-caps SpaceGrotesk bold
/// text. Active state is full binary inversion (#000000 bg / #f9f9f9 text).
/// Fixed height kHeight px. Optionally indented 16 px for sub-items.
class SidebarItemAtom final : public juce::Component
{
public:
    juce::String          label;
    juce::String          secondaryLabel;
    bool                  isActive   = false;
    bool                  isIndented = false;
    std::function<void()> onClick;
    std::function<void()> onDoubleClick;
    std::function<void (juce::Point<int>)> onRightClick;

    static constexpr int kHeight = 28;

    SidebarItemAtom() = default;

    void paint  (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidebarItemAtom)
};
