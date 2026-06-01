#pragma once
//==============================================================================
// PRD-0078: RecordPlayhead atom — the DAW record playhead.
//
// A second timeline cursor that marks where capture is writing, drawn ONLY while
// a recording session is armed or recording. It must be unmistakably distinct
// from EPIC-0008's hairline now-line (the 1px solid read-only playback cursor),
// without introducing colour (DESIGN.md forbids it).
//
// Per PRD-0078 §1.5.3 the record playhead is a *thicker, dithered* vertical
// band — a 3px-wide stripe filled with a 50% checkerboard of ink (#2d2d2d),
// capped with a small solid square "record head" glyph at the top. The density
// and width difference makes it read heavier than the now-line at a glance while
// staying strictly monochrome. It owns no model state and never intercepts mouse
// events (read-only, like the now-line).
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw
{

class RecordPlayhead final : public juce::Component
{
public:
    RecordPlayhead()
    {
        setInterceptsMouseClicks (false, false);
    }

    // The x (within this component) of the record playhead. A value outside the
    // local bounds, or a negative value, hides the band (disarmed or scrolled
    // out of view).
    void setLineX (int x)
    {
        if (x == lineX_)
            return;
        lineX_ = x;
        repaint();
    }

    int getLineX() const noexcept { return lineX_; }

    static bool isLineVisible (int x, int width) noexcept
    {
        return x >= 0 && x < width;
    }

    void paint (juce::Graphics& g) override
    {
        if (! isLineVisible (lineX_, getWidth()))
            return;

        const int x = lineX_;
        const int h = getHeight();

        // Dithered band: 50% checkerboard of ink over the (transparent) ground.
        const juce::Rectangle<int> band (x, 0, kBandWidth, h);
        g.fillCheckerBoard (band.toFloat(),
                            2.0f, 2.0f,           // 2px checker cells
                            kInk, juce::Colours::transparentBlack);

        // Solid square "record head" glyph at the very top.
        const int head = kHeadSize;
        g.setColour (kInk);
        g.fillRect (juce::Rectangle<int> (x - (head - kBandWidth) / 2, 0, head, head));
    }

private:
    static constexpr int kBandWidth = 3;
    static constexpr int kHeadSize  = 7;
    static inline const juce::Colour kInk { 0xFF2D2D2D };

    int lineX_ { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordPlayhead)
};

} // namespace Daw
