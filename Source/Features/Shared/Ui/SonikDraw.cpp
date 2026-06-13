#include "SonikDraw.h"

namespace sonik::ui::draw
{
namespace
{
    // 2x2 ordered-dither thresholds shared by every dithered fill in the app
    // (VU meters, progress fills, hatches) so the whole interface shares one
    // dithering vocabulary.
    constexpr float kThresholds[4] = { 0.20f, 0.60f, 0.80f, 0.40f };
}

void fillDithered (juce::Graphics& g, juce::Rectangle<int> area, float density)
{
    if (area.isEmpty())
        return;

    density = juce::jlimit (0.0f, 1.0f, density);

    const int x0 = area.getX();
    const int y0 = area.getY();
    const int w  = area.getWidth();
    const int h  = area.getHeight();

    g.setColour (theme::ink());
    for (int row = 0; row < h; ++row)
    {
        const int y = y0 + row;
        for (int col = 0; col < w; ++col)
        {
            const int x = x0 + col;
            const int cellIdx = ((x & 1) << 1) | (y & 1);
            if (density >= kThresholds[cellIdx])
                g.fillRect (x, y, 1, 1);
        }
    }
}

void fillDitheredMeter (juce::Graphics& g, juce::Rectangle<int> litArea, float density)
{
    if (litArea.isEmpty())
        return;

    density = juce::jlimit (0.0f, 1.0f, density);

    const int x0 = litArea.getX();
    const int y0 = litArea.getY();
    const int w  = litArea.getWidth();
    const int h  = litArea.getHeight();

    const int totalRows = juce::jmax (1, h);
    const int litRows   = juce::roundToInt (density * static_cast<float> (totalRows));

    // Bottom-up: lower rows stay solid, the top of the column speckles out.
    g.setColour (theme::ink());
    for (int row = 0; row < litRows; ++row)
    {
        const int y = y0 + h - 1 - row;
        const float rowDensity = juce::jlimit (0.0f, 1.0f,
                                               density - row / static_cast<float> (totalRows));
        for (int col = 0; col < w; ++col)
        {
            const int x = x0 + col;
            const int cellIdx = ((x & 1) << 1) | (y & 1);
            if (rowDensity >= kThresholds[cellIdx])
                g.fillRect (x, y, 1, 1);
        }
    }
}

void drawDitheredShadow (juce::Graphics& g, juce::Rectangle<int> panel, int offsetPx)
{
    auto shadow = panel.translated (offsetPx, offsetPx);
    g.saveState();
    g.reduceClipRegion (shadow);
    g.setColour (theme::ink());
    for (int y = shadow.getY(); y < shadow.getBottom(); y += 2)
        for (int x = shadow.getX() + ((y / 2) % 2) * 2; x < shadow.getRight(); x += 4)
            g.fillRect (x, y, 2, 2); // 50% checkerboard, zero blur
    g.restoreState();
}

void paintLatchButton (juce::Graphics& g, juce::Rectangle<int> bounds,
                       const juce::String& label, const ButtonState& state,
                       float fontHeight)
{
    const float alpha = juce::jlimit (0.0f, 1.0f, state.alpha)
                      * (state.enabled ? 1.0f : theme::kDisabledAlpha);

    // Fill: instant inversion when latched or pressed; tonal step on hover.
    const bool inverted = state.enabled && (state.active || state.pressed);
    juce::Colour fill = theme::surface();
    if (inverted)
        fill = theme::ink().withAlpha (alpha);
    else if (state.enabled && state.hover)
        fill = theme::containerHighest();

    g.setColour (fill);
    g.fillRect (bounds);

    // Mandatory 2px solid ink border, zero radius (DESIGN.md §5).
    g.setColour (theme::ink().withAlpha (alpha));
    g.drawRect (bounds, theme::kBorderPx);

    // Centred Space Mono label, inverted with the fill.
    g.setColour ((inverted ? theme::surface() : theme::ink()).withAlpha (alpha));
    g.setFont (theme::mono (fontHeight));
    g.drawText (label, bounds, juce::Justification::centred, false);
}
} // namespace sonik::ui::draw
