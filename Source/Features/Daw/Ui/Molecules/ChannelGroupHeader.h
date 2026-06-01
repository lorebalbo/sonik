#pragma once
//==============================================================================
// PRD-0067: ChannelGroupHeader molecule.
//
// The header row of a per-deck channel group: an all-caps Space Mono deck label
// (e.g. "DECK 1") in the left gutter and an always-visible collapse/expand
// toggle (DESIGN.md button: 2-px ink border, surface fill, monochrome glyph)
// that folds the group's three lanes to just this header row (PRD-0067 §1.5.2,
// group-level collapse).
//
// Message/UI thread only; no audio-thread code.
//==============================================================================

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DawLayoutMetrics.h"

namespace Daw
{

class ChannelGroupHeader final : public juce::Component
{
public:
    explicit ChannelGroupHeader (int deckIndex) : deckIndex_ (deckIndex) {}

    // Fired when the collapse toggle is clicked. The owner flips collapse state.
    std::function<void()> onToggleCollapsed;

    void setCollapsed (bool isCollapsed)
    {
        if (collapsed_ == isCollapsed)
            return;
        collapsed_ = isCollapsed;
        repaint();
    }

    bool isCollapsed() const noexcept { return collapsed_; }

    static juce::String labelForDeck (int deckIndex)
    {
        return "DECK " + juce::String (deckIndex + 1);
    }

    void resized() override
    {
        const int sz = DawLayout::kGroupHeaderHeight - 6;
        toggleBounds_ = juce::Rectangle<int> (DawLayout::kTrackHeaderWidth - sz - 4,
                                              3, sz, sz);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour (kHeaderFill);
        g.fillRect (bounds);
        g.setColour (kInk);
        g.drawRect (bounds, 2);

        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
        g.drawText (labelForDeck (deckIndex_),
                    bounds.withTrimmedLeft (6).withWidth (DawLayout::kTrackHeaderWidth - 28),
                    juce::Justification::centredLeft, false);

        // Collapse / expand toggle.
        g.setColour (kSurface);
        g.fillRect (toggleBounds_);
        g.setColour (kInk);
        g.drawRect (toggleBounds_, 2);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold));
        g.drawText (collapsed_ ? juce::String ("+") : juce::String ("-"),
                    toggleBounds_, juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (toggleBounds_.contains (event.getPosition()) && onToggleCollapsed)
            onToggleCollapsed();
    }

private:
    static inline const juce::Colour kInk        { 0xFF2D2D2D };
    static inline const juce::Colour kSurface    { 0xFFFDFDFD };
    static inline const juce::Colour kHeaderFill { 0xFFE5E5E5 };

    int                  deckIndex_;
    bool                 collapsed_ { false };
    juce::Rectangle<int> toggleBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupHeader)
};

} // namespace Daw
