#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cstdint>

namespace HotCueColors
{

// 16-color palette — first 8 are the fixed per-pad colors (A–H), rest kept for custom selection
static constexpr juce::uint32 palette[16] =
{
    0xFF18FFFF, //  0: A — Cyan
    0xFF00B0FF, //  1: B — Sky Blue
    0xFF2979FF, //  2: C — Blue
    0xFF651FFF, //  3: D — Deep Violet
    0xFFD500F9, //  4: E — Violet
    0xFFF50057, //  5: F — Magenta
    0xFFFF4081, //  6: G — Pink
    0xFFFF8A80, //  7: H — Salmon
    0xFFF870A0, //  8: Pink (extra)
    0xFFE8302A, //  9: Red (extra)
    0xFFF08828, // 10: Orange (extra)
    0xFFE8D820, // 11: Yellow (extra)
    0xFF10B020, // 12: Green (extra)
    0xFF18C8C8, // 13: Aqua (extra)
    0xFF1870F0, // 14: Blue (extra)
    0xFFA028E8  // 15: Purple (extra)
};

// Default color index for each pad A(0) through H(7)
static constexpr int defaultColorForPad[8] =
{
    0,  // A: Cyan
    1,  // B: Sky Blue
    2,  // C: Blue
    3,  // D: Deep Violet
    4,  // E: Violet
    5,  // F: Magenta
    6,  // G: Pink
    7   // H: Salmon
};

inline juce::Colour getColour (int colorIndex)
{
    if (colorIndex < 0 || colorIndex >= 16)
        colorIndex = 0;
    return juce::Colour (palette[static_cast<size_t> (colorIndex)]);
}

} // namespace HotCueColors

struct HotCueInfo
{
    int          padIndex        = 0;
    int64_t      positionSamples = -1;
    int          colorIndex      = 0;
    juce::String label;
    bool         active          = false;
};
