#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// SonikTheme — the single source of truth for every DESIGN.md "1-Bit Deck"
// design token. UI code must take its colours, metrics and fonts from here;
// hex literals in component files are a defect.
//
// The palette is strictly monochrome (DESIGN.md §2): depth and emphasis come
// from the tonal surface scale and from dithering (see SonikDraw), never from
// new greys, gradients or colour.
//==============================================================================
namespace sonik::ui::theme
{
    //--------------------------------------------------------------------------
    // Palette (DESIGN.md §2)
    //--------------------------------------------------------------------------
    inline juce::Colour ink()              { return juce::Colour (0xFF2D2D2D); } // data, borders, text
    inline juce::Colour surface()          { return juce::Colour (0xFFFDFDFD); } // the chassis
    inline juce::Colour containerLow()     { return juce::Colour (0xFFF3F3F4); } // recessed panels
    inline juce::Colour containerHighest() { return juce::Colour (0xFFE2E2E2); } // headers, hover fill

    // Disabled treatment: ink at reduced strength. Never introduce a new grey.
    constexpr float kDisabledAlpha = 0.35f;
    inline juce::Colour inkDisabled()      { return ink().withAlpha (kDisabledAlpha); }

    //--------------------------------------------------------------------------
    // Structure (DESIGN.md §4–5)
    //--------------------------------------------------------------------------
    constexpr int kBorderPx       = 2;  // mandatory solid border on buttons/panels
    constexpr int kShadowOffsetPx = 4;  // dithered drop shadow offset, zero blur

    //--------------------------------------------------------------------------
    // Type scale (DESIGN.md §3) — Space Mono only.
    //--------------------------------------------------------------------------
    constexpr float kFontSmall = 9.0f;   // tick captions, meter chassis labels
    constexpr float kFontLabel = 11.0f;  // all-caps printed hardware labels
    constexpr float kFontMenu  = 12.0f;  // menus, combo boxes, text inputs
    constexpr float kFontBody  = 13.0f;  // body text, library rows, buttons

    // The design system's one and only family. The name resolves to the
    // Space Mono typeface embedded in the binary via
    // SonikLookAndFeel::getTypefaceForFont (installed as the default
    // LookAndFeel in SonikApplication::initialise), so it renders identically
    // on machines without the font installed.
    inline juce::Font mono (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Space Mono", height,
                                              bold ? juce::Font::bold
                                                   : juce::Font::plain));
    }
} // namespace sonik::ui::theme
