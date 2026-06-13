#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SonikTheme.h"

//==============================================================================
// SonikDraw — shared paint vocabulary for the DESIGN.md "1-Bit Deck" language.
//
// Every dither pattern, drop shadow and tactile button in the app must be
// painted through these helpers so the whole interface speaks one visual
// dialect. Per-component reimplementations are a defect (they drift: at one
// point the app had four dither loops and three different whites).
//==============================================================================
namespace sonik::ui::draw
{
    //--------------------------------------------------------------------------
    // Dithering (DESIGN.md §2 "Dithered Gradients")
    //--------------------------------------------------------------------------

    /// Uniform 2x2 ordered (Bayer-ish) dither of ink over `area`.
    /// density 0 = empty, 1 = solid. Used for progress fills and hatching.
    void fillDithered (juce::Graphics&, juce::Rectangle<int> area, float density);

    /// VU-meter variant: fills `litArea` bottom-up with a per-row density
    /// falloff so the top of the lit region speckles out instead of ending
    /// on a hard edge (the meter reads as "denser toward the peak").
    void fillDitheredMeter (juce::Graphics&, juce::Rectangle<int> litArea, float density);

    /// 50% checkerboard drop shadow for floating panels (DESIGN.md §4 "The
    /// Dithered Drop"): `panel` translated by `offsetPx`, zero blur. Paint it
    /// BEFORE the panel so only the exposed L-strip remains visible.
    void drawDitheredShadow (juce::Graphics&, juce::Rectangle<int> panel,
                             int offsetPx = theme::kShadowOffsetPx);

    //--------------------------------------------------------------------------
    // The canonical tactile button (DESIGN.md §5 "Decks & Transport")
    //--------------------------------------------------------------------------

    /// Interaction state for paintLatchButton. All state changes are INSTANT
    /// (no fades): hover steps the fill to the tonal containerHighest, press
    /// and active invert to ink fill / surface text.
    struct ButtonState
    {
        bool  active  = false;  // latched on -> full inversion
        bool  hover   = false;  // mouse over  -> tonal fill step
        bool  pressed = false;  // mouse down  -> immediate inversion preview
        bool  enabled = true;   // false       -> ink at kDisabledAlpha
        float alpha   = 1.0f;   // extra modulation (pulse/partial states)
    };

    /// Fill + mandatory 2px ink border + centred all-caps Space Mono label.
    void paintLatchButton (juce::Graphics&, juce::Rectangle<int> bounds,
                           const juce::String& label, const ButtonState& state,
                           float fontHeight = theme::kFontBody);
} // namespace sonik::ui::draw
