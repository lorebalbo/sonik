#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cstdint>

namespace HotCueColors
{

// 16-color Rekordbox-compatible palette (ARGB)
static constexpr juce::uint32 palette[16] =
{
    0xFFF870A0, //  0: Pink
    0xFFE8302A, //  1: Red
    0xFFF08828, //  2: Orange
    0xFFE8D820, //  3: Yellow
    0xFF10B020, //  4: Green
    0xFF18C8C8, //  5: Aqua
    0xFF1870F0, //  6: Blue
    0xFFA028E8, //  7: Purple
    0xFFE840A0, //  8: Magenta
    0xFFF06850, //  9: Salmon
    0xFFF0A060, // 10: Peach
    0xFFA0E828, // 11: Lime
    0xFF28B898, // 12: Teal
    0xFF50B8F0, // 13: Sky
    0xFF9878F0, // 14: Lavender
    0xFFE868A8  // 15: Rose
};

// Default color index for each pad A(0) through H(7)
static constexpr int defaultColorForPad[8] =
{
    1,  // A: Red
    3,  // B: Yellow
    4,  // C: Green
    6,  // D: Blue
    7,  // E: Purple
    2,  // F: Orange
    5,  // G: Aqua
    0   // H: Pink
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
