#pragma once
//==============================================================================
// PRD-0093: Automation Lane UI metrics + per-parameter value-range abstraction.
//
// Owns the fixed automation-lane row height (shorter than a PRD-0067 source lane)
// and the value-axis gutter geometry shared by the continuous / boolean lane
// views, plus the per-parameterId value range used to map a raw breakpoint value
// to a y position inside the lane body.
//
// The horizontal axis is NOT owned here — that is the shared PRD-0065
// TimelineTransform. Only vertical/value mapping and the left gutter live here.
//
// Message/UI thread only; no audio-thread code; no allocation on the mapping
// path.
//==============================================================================

#include <juce_graphics/juce_graphics.h>

#include "../AutomationIds.h"

namespace Daw
{

struct AutomationLaneMetrics
{
    // A modest fixed lane height (DESIGN.md 2-px grid aligned). Shorter than a
    // source lane (kLaneHeight = 36) so several lanes can be revealed without
    // dominating the panel (§1.5.5).
    static constexpr int kAutomationLaneHeight = 28;

    // The value-axis gutter occupies the LEFT part of the shared track-header
    // gutter (DawLayout::kTrackHeaderWidth = 120). The bypass button sits in the
    // rightmost slice of that gutter so the curve content still starts at
    // x = kTrackHeaderWidth, identical to the ruler/clip origin.
    static constexpr int kBypassButtonWidth  = 26;
    static constexpr int kBypassButtonInset  = 3;

    // Vertical padding inside the lane body so the curve/steps never touch the
    // 2-px lane frame (keeps the frame legible — §1.5.3).
    static constexpr int kBodyInsetY = 3;
};

//==============================================================================
// AutomationValueRange — maps a raw breakpoint value (native parameter units) to
// a normalised [0,1] position (0 = bottom/min, 1 = top/max), with the labels the
// value axis prints. Provided per parameterId.
//
// Documented ranges (§1.5.6):
//   filter           bipolar  [-1, +1]   (post-detent normalised; 0 = no filter)
//   high/mid/low/gain dB      [-26, +6]  (a sensible display window over the
//                                          mixer's full [-60,+12] capture range;
//                                          values are clamped into the window for
//                                          display only — the model is untouched)
//   volume           linear   [0, 1]     (the channel fader's full native range)
//   tempo            BPM      [60, 200]  (a sensible default window; the screenshot
//                                          tool / callers may pass data-derived
//                                          min/max instead)
//==============================================================================
struct AutomationValueRange
{
    double      minValue { 0.0 };
    double      maxValue { 1.0 };
    juce::String minLabel { "0" };
    juce::String maxLabel { "1" };
    juce::String paramLabel { "" };

    // Normalised position in [0,1] (clamped). 1 maps to the TOP of the lane.
    double normalise (double rawValue) const noexcept
    {
        const double span = maxValue - minValue;
        if (span == 0.0)
            return 0.5;
        const double n = (rawValue - minValue) / span;
        return juce::jlimit (0.0, 1.0, n);
    }

    // PRD-0094 inverse of normalise(): map a normalised [0,1] position (0 = min,
    // 1 = top/max) back to a raw value, clamped to the display window. Used by the
    // lane-view editing seams to turn a click/drag y into a candidate value. The
    // COMMAND layer re-clamps to the full native DSP range (which may be wider).
    double denormalise (double norm) const noexcept
    {
        const double n = juce::jlimit (0.0, 1.0, norm);
        return minValue + n * (maxValue - minValue);
    }

    // Clamp a raw value to the display window [minValue, maxValue].
    double clampToWindow (double rawValue) const noexcept
    {
        return juce::jlimit (juce::jmin (minValue, maxValue),
                             juce::jmax (minValue, maxValue),
                             rawValue);
    }

    // Resolve the canonical range for an in-scope continuous parameterId. The
    // all-caps parameter label follows DESIGN.md hardware-label style.
    static AutomationValueRange forContinuousParameter (const juce::String& parameterId)
    {
        AutomationValueRange r;

        if (parameterId == "filter")
        {
            r.minValue = -1.0; r.maxValue = 1.0;
            r.minLabel = "-1";  r.maxLabel = "+1";
            r.paramLabel = "FILTER";
        }
        else if (parameterId == "volume")
        {
            r.minValue = 0.0; r.maxValue = 1.0;
            r.minLabel = "0"; r.maxLabel = "1";
            r.paramLabel = "VOLUME";
        }
        else if (parameterId == "gain")
        {
            r.minValue = -26.0; r.maxValue = 6.0;
            r.minLabel = "-26"; r.maxLabel = "+6";
            r.paramLabel = "GAIN";
        }
        else if (parameterId == "high" || parameterId == "eq.high")
        {
            r.minValue = -26.0; r.maxValue = 6.0;
            r.minLabel = "-26"; r.maxLabel = "+6";
            r.paramLabel = "HIGH";
        }
        else if (parameterId == "mid" || parameterId == "eq.mid")
        {
            r.minValue = -26.0; r.maxValue = 6.0;
            r.minLabel = "-26"; r.maxLabel = "+6";
            r.paramLabel = "MID";
        }
        else if (parameterId == "low" || parameterId == "eq.low")
        {
            r.minValue = -26.0; r.maxValue = 6.0;
            r.minLabel = "-26"; r.maxLabel = "+6";
            r.paramLabel = "LOW";
        }
        else if (parameterId == "tempo")
        {
            r.minValue = 60.0;  r.maxValue = 200.0;
            r.minLabel = "60";  r.maxLabel = "200";
            r.paramLabel = "TEMPO";
        }
        else
        {
            // Unknown / future continuous parameter: normalised [0,1] window.
            r.minValue = 0.0; r.maxValue = 1.0;
            r.minLabel = "0"; r.maxLabel = "1";
            r.paramLabel = parameterId.toUpperCase();
        }

        return r;
    }

    // The all-caps label for an in-scope boolean parameterId (§1.5.6).
    static juce::String booleanParamLabel (const juce::String& parameterId)
    {
        if (parameterId == "keyLock")      return "KEYLOCK";
        if (parameterId == "pitchStretch") return "STRETCH";
        if (parameterId == "keyStepper")   return "KEYSTEP";
        return parameterId.toUpperCase();
    }
};

} // namespace Daw
