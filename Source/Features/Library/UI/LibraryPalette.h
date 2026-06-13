#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Shared/Ui/SonikTheme.h"

/// Library-local aliases over the app-wide SonikTheme tokens (DESIGN.md).
/// Kept so the Library UI keeps its established vocabulary, but every value
/// now comes from the one palette in Features/Shared/Ui/SonikTheme.h.
namespace LibraryPalette
{
    inline juce::Colour primary()          { return sonik::ui::theme::ink(); }
    inline juce::Colour surface()          { return sonik::ui::theme::surface(); }
    inline juce::Colour chassis()          { return sonik::ui::theme::containerHighest(); }
    inline juce::Colour containerLow()     { return sonik::ui::theme::containerLow(); }
    inline juce::Colour containerHighest() { return sonik::ui::theme::containerHighest(); }
    inline juce::Colour containerLowest()  { return sonik::ui::theme::surface(); }

    inline juce::FontOptions bodyFont (float size = sonik::ui::theme::kFontBody,
                                       int style = juce::Font::plain)
    {
        return juce::FontOptions ("Space Mono", size, style);
    }

    inline juce::FontOptions boldLabelFont (float size = sonik::ui::theme::kFontLabel)
    {
        return juce::FontOptions ("Space Mono", size, juce::Font::bold);
    }
} // namespace LibraryPalette
