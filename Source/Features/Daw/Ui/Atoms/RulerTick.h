#pragma once
//==============================================================================
// PRD-0066: RulerTick atom.
//
// A pure paint component drawing a single vertical tick of the DAW time ruler.
// It knows only its "kind" (Bar vs Beat) and renders the two with different
// tick heights and tonal weight (longer/heavier for bars, shorter/lighter for
// beats) — never a colour difference, per DESIGN.md's strict-monochrome rule.
//
// Derived from the Figma DAW ruler (file 3bmQVcRbY9JSaJqTCPH9AQ, frame 86):
// solid #2d2d2d ticks on a light tick band; bar boundaries are taller and 2-px
// wide, beats are shorter and 1-px wide.
//
// It performs no layout logic and reads no state; the TimeRuler molecule owns
// it, positions it, and decides its kind. Message/UI thread only.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw
{

class RulerTick final : public juce::Component
{
public:
    enum class Kind
    {
        Bar,    // bar boundary — tall, 2-px heavy tick
        Beat    // whole beat within a bar — short, 1-px light tick
    };

    explicit RulerTick (Kind kind = Kind::Beat) : kind_ (kind)
    {
        setInterceptsMouseClicks (false, false);
    }

    void setKind (Kind kind)
    {
        if (kind_ == kind)
            return;
        kind_ = kind;
        repaint();
    }

    Kind getKind() const noexcept { return kind_; }

    // Tick line width in logical pixels for a given kind (DESIGN.md tonal weight).
    static int lineWidthForKind (Kind kind) noexcept
    {
        return kind == Kind::Bar ? 2 : 1;
    }

    // Tick height (from the bottom of the band upward) for a given kind and
    // band height. Bars span the full band; beats span ~55 % of it.
    static int tickHeightForKind (Kind kind, int bandHeight) noexcept
    {
        if (bandHeight <= 0)
            return 0;
        return kind == Kind::Bar
                   ? bandHeight
                   : juce::jmax (1, juce::roundToInt (bandHeight * 0.55f));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        const int  bandH  = bounds.getHeight();
        const int  tickH  = tickHeightForKind (kind_, bandH);
        const int  lineW  = lineWidthForKind (kind_);

        // Tick grows up from the bottom edge of the band (bars touch the top,
        // beats stop short). Left-aligned within the 2-px-wide component bounds.
        const int x = bounds.getX();
        const int y = bounds.getBottom() - tickH;

        g.setColour (kInk);
        g.fillRect (x, y, lineW, tickH);
    }

private:
    static inline const juce::Colour kInk { 0xFF2D2D2D };

    Kind kind_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RulerTick)
};

} // namespace Daw
