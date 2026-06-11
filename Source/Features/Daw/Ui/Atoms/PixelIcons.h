#pragma once
//==============================================================================
// Pixel-art icon set for the DAW chrome (transport, metronome, view toggles).
//
// DESIGN.md mandates hand-crafted pixel icons on the grid — no anti-aliased
// vector paths. Every glyph here is composed exclusively of integer-aligned
// fillRect calls so it renders 1-bit crisp at any position. The caller sets the
// colour before calling (active/inactive fill inversion happens at the button
// level, not here).
//
// All glyphs draw inside `box` centred on a common visual grid so a row of
// transport buttons reads as one unit. Message/UI thread only.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw::PixelIcons
{

// The square icon cell centred inside an arbitrary button rect. Icons are drawn
// on a 14x14 design grid scaled to the cell (integer scale only, min 1).
inline juce::Rectangle<int> iconCell (juce::Rectangle<int> button, int size = 14)
{
    return juce::Rectangle<int> (size, size).withCentre (button.getCentre());
}

//------------------------------------------------------------------------------
// Transport glyphs
//------------------------------------------------------------------------------

// Solid right-pointing triangle built from stepped columns (1-bit, no AA).
inline void drawPlay (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 12);
    const int x0 = c.getX() + 1, y0 = c.getY(), h = c.getHeight();
    const int cols = 9;
    for (int i = 0; i < cols; ++i)
    {
        // Column height shrinks symmetrically toward the tip.
        const int inset = (i * h) / (2 * cols);
        g.fillRect (x0 + i, y0 + inset, 1, juce::jmax (1, h - 2 * inset));
    }
}

// Two solid vertical bars.
inline void drawPause (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 12);
    const int barW = 3, gap = 3;
    const int x = c.getCentreX() - (barW * 2 + gap) / 2;
    g.fillRect (x,              c.getY() + 1, barW, c.getHeight() - 2);
    g.fillRect (x + barW + gap, c.getY() + 1, barW, c.getHeight() - 2);
}

// Solid square.
inline void drawStop (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 10);
    g.fillRect (c);
}

// Filled pixel circle (record).
inline void drawRecord (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 11);
    const int r  = c.getWidth() / 2;
    const int cx = c.getCentreX(), cy = c.getCentreY();
    for (int dy = -r; dy <= r; ++dy)
    {
        // Pixel-circle row span: widest run with dx^2 + dy^2 <= r^2.
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
            ++dx;
        g.fillRect (cx - dx, cy + dy, dx * 2 + 1, 1);
    }
}

// Cycle/loop: a bold 2-px ring broken by two stepped arrowheads (Logic's cycle
// glyph, squared off for the pixel grid).
inline void drawLoop (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 14);
    const auto ring = c.reduced (0, 3);

    // Side edges.
    g.fillRect (ring.getX(), ring.getY() + 1, 2, ring.getHeight() - 2);
    g.fillRect (ring.getRight() - 2, ring.getY() + 1, 2, ring.getHeight() - 2);

    // Top edge with a right-pointing arrowhead at its end.
    g.fillRect (ring.getX() + 1, ring.getY(), ring.getWidth() - 7, 2);
    const int axTop = ring.getRight() - 6;
    g.fillRect (axTop,     ring.getY() - 2, 2, 6);   // head back
    g.fillRect (axTop + 2, ring.getY() - 1, 2, 4);
    g.fillRect (axTop + 4, ring.getY(),     2, 2);   // head tip

    // Bottom edge with a left-pointing arrowhead at its end.
    g.fillRect (ring.getX() + 6, ring.getBottom() - 2, ring.getWidth() - 7, 2);
    const int axBot = ring.getX() + 4;
    g.fillRect (axBot,     ring.getBottom() - 4, 2, 6);
    g.fillRect (axBot - 2, ring.getBottom() - 3, 2, 4);
    g.fillRect (axBot - 4, ring.getBottom() - 2, 2, 2);
}

// Metronome: solid 2-px-walled trapezoid body, full-width base block and a bold
// diagonal pendulum — heavy enough to read at 14 px without anti-aliasing.
inline void drawMetronome (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 14);
    const int baseY = c.getBottom() - 3;       // top of the base block
    const int topY  = c.getY();
    const int h     = baseY - topY;            // sloped-body height
    const int cx    = c.getCentreX();

    // Sloped 2-px side walls, narrowing 6 -> 2 half-width toward the top.
    for (int i = 0; i < h; ++i)
    {
        const int half = 2 + ((h - 1 - i) * 4 + h / 2) / h;
        const int y = topY + i;
        g.fillRect (cx - half, y, 2, 1);
        g.fillRect (cx + half - 1, y, 2, 1);
    }

    // Top cap and base block.
    g.fillRect (cx - 3, topY, 7, 2);
    g.fillRect (cx - 7, baseY, 15, 3);

    // Pendulum: 2-px diagonal from the base centre up to the right shoulder.
    for (int i = 0; i < h - 1; ++i)
        g.fillRect (cx - 1 + ((i * 5 + h / 2) / h), baseY - 1 - i, 2, 1);
}

//------------------------------------------------------------------------------
// View-control glyphs
//------------------------------------------------------------------------------

namespace detail
{
    // One diagonal arrow: a corner bracket plus a diagonal pixel run. `dx`/`dy`
    // are +/-1 selecting the quadrant the arrow POINTS toward.
    inline void diagonalArrow (juce::Graphics& g, int cornerX, int cornerY,
                               int dx, int dy, int armLen, int diagLen)
    {
        // Corner bracket (the arrowhead).
        g.fillRect (dx > 0 ? cornerX - armLen + 1 : cornerX, cornerY, armLen, 1);
        g.fillRect (cornerX, dy > 0 ? cornerY - armLen + 1 : cornerY, 1, armLen);

        // Diagonal shaft running back from the corner.
        for (int i = 1; i <= diagLen; ++i)
            g.fillRect (cornerX - dx * i, cornerY - dy * i, 1, 1);
    }
}

// Fullscreen-expand: four arrows pointing outward from the centre.
inline void drawExpand (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 14);
    const int arm = 4, diag = 4;
    detail::diagonalArrow (g, c.getX(),         c.getY(),          -1, -1, arm, diag);
    detail::diagonalArrow (g, c.getRight() - 1, c.getY(),           1, -1, arm, diag);
    detail::diagonalArrow (g, c.getX(),         c.getBottom() - 1, -1,  1, arm, diag);
    detail::diagonalArrow (g, c.getRight() - 1, c.getBottom() - 1,  1,  1, arm, diag);
}

// Fullscreen-shrink: four arrows pointing inward toward the centre.
inline void drawShrink (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 14);
    const int arm = 4, diag = 4;
    const int inset = 5; // arrowheads sit nearer the centre, shafts run outward
    detail::diagonalArrow (g, c.getX() + inset,         c.getY() + inset,          1,  1, arm, diag);
    detail::diagonalArrow (g, c.getRight() - 1 - inset, c.getY() + inset,         -1,  1, arm, diag);
    detail::diagonalArrow (g, c.getX() + inset,         c.getBottom() - 1 - inset,  1, -1, arm, diag);
    detail::diagonalArrow (g, c.getRight() - 1 - inset, c.getBottom() - 1 - inset, -1, -1, arm, diag);
}

// Collapse / expand chevrons for the panel fold toggle.
inline void drawChevronUp (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 12);
    const int cx = c.getCentreX(), top = c.getCentreY() - 2;
    for (int i = 0; i < 5; ++i)
    {
        g.fillRect (cx - i - 1, top + i, 2, 2);
        g.fillRect (cx + i,     top + i, 2, 2);
    }
}

inline void drawChevronDown (juce::Graphics& g, juce::Rectangle<int> button)
{
    const auto c = iconCell (button, 12);
    const int cx = c.getCentreX(), top = c.getCentreY() - 2;
    for (int i = 0; i < 5; ++i)
    {
        g.fillRect (cx - i - 1, top + 4 - i, 2, 2);
        g.fillRect (cx + i,     top + 4 - i, 2, 2);
    }
}

// Small down-pointing triangle used as the dropdown affordance.
inline void drawDropdownCaret (juce::Graphics& g, juce::Rectangle<int> area)
{
    const int cx = area.getCentreX(), cy = area.getCentreY() - 2;
    for (int i = 0; i < 4; ++i)
        g.fillRect (cx - 3 + i, cy + i, 7 - 2 * i, 1);
}

} // namespace Daw::PixelIcons
