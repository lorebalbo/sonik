#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// Strict monochrome palette from DESIGN.md. Only these five colours may
/// appear anywhere in the Library UI component tree.
namespace LibraryPalette
{
    inline juce::Colour primary()          { return juce::Colour (0xff000000); } // #000000
    inline juce::Colour surface()          { return juce::Colour (0xfff9f9f9); } // #f9f9f9
    inline juce::Colour containerLow()     { return juce::Colour (0xfff3f3f4); } // #f3f3f4
    inline juce::Colour containerHighest() { return juce::Colour (0xffe2e2e2); } // #e2e2e2
    inline juce::Colour containerLowest()  { return juce::Colour (0xffffffff); } // #ffffff

    inline juce::FontOptions bodyFont (float size = 13.0f, int style = juce::Font::plain)
    {
        return juce::FontOptions ("Space Grotesk", size, style);
    }

    inline juce::FontOptions boldLabelFont (float size = 11.0f)
    {
        return juce::FontOptions ("Space Grotesk", size, juce::Font::bold);
    }
} // namespace LibraryPalette
