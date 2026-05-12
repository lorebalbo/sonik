#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/// Strict monochrome palette from DESIGN.md. Only these five colours may
/// appear anywhere in the Library UI component tree.
namespace LibraryPalette
{
    inline juce::Colour primary()          { return juce::Colour (0xff2d2d2d); } // #2d2d2d (DESIGN.md ink)
    inline juce::Colour surface()          { return juce::Colour (0xfffdfdfd); } // #fdfdfd (DESIGN.md chassis)
    inline juce::Colour chassis()          { return juce::Colour (0xffe5e5e5); } // #e5e5e5 (outer Library chassis)
    inline juce::Colour containerLow()     { return juce::Colour (0xfff3f3f4); } // #f3f3f4
    inline juce::Colour containerHighest() { return juce::Colour (0xffe2e2e2); } // #e2e2e2
    inline juce::Colour containerLowest()  { return juce::Colour (0xfffdfdfd); } // #fdfdfd

    inline juce::FontOptions bodyFont (float size = 13.0f, int style = juce::Font::plain)
    {
        return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, style);
    }

    inline juce::FontOptions boldLabelFont (float size = 11.0f)
    {
        return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::bold);
    }
} // namespace LibraryPalette
