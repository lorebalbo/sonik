#pragma once
//==============================================================================
// PRD-0070: Playhead atom — the DAW live now-line.
//
// A pure-rendering atom that draws a single vertical now-line spanning its full
// height, fully DESIGN.md compliant: solid ink (#2d2d2d), 2 px wide, zero
// border-radius, zero glow, zero gradient, pixel-aligned so it never blurs.
//
// It owns NO tempo / transport / model state: the parent computes the timeline
// x (via PRD-0065 TimelineTransform) and sets it. The line is read-only with
// respect to deck / mixer / master-clock state — it cannot be dragged, clicked,
// or used to seek (PRD-0070 §1.5.3). It never intercepts mouse events so the
// gestures pass through to the timeline beneath it.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw
{

class Playhead final : public juce::Component
{
public:
    Playhead()
    {
        setInterceptsMouseClicks (false, false);
    }

    // The x (within this component) of the now-line. A value outside the local
    // bounds hides the line (the live position is scrolled out of view).
    void setLineX (int x)
    {
        if (x == lineX_)
            return;
        lineX_ = x;
        repaint();
    }

    int getLineX() const noexcept { return lineX_; }

    // Whether the line is within the visible content region [0, width).
    static bool isLineVisible (int x, int width) noexcept
    {
        return x >= 0 && x < width;
    }

    void paint (juce::Graphics& g) override
    {
        if (! isLineVisible (lineX_, getWidth()))
            return;

        // Pixel-align the 2-px line so it stays crisp at every zoom level.
        const float x = (float) lineX_;
        g.setColour (kInk);
        g.fillRect (juce::Rectangle<float> (x, 0.0f, kLineWidth, (float) getHeight()));

        // Logic-style head marker: a stepped downward triangle sitting at the
        // top of the line (inside the ruler band), pure pixel rects — no AA.
        const int cx = lineX_;
        for (int row = 0; row < 5; ++row)
        {
            const int half = 5 - row;
            g.fillRect (cx - half + 1, row, half * 2, 1);
        }
    }

private:
    static constexpr float kLineWidth = 2.0f;
    static inline const juce::Colour kInk { 0xFF2D2D2D };

    int lineX_ { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playhead)
};

} // namespace Daw
