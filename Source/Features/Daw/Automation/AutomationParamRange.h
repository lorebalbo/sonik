#pragma once
//==============================================================================
// PRD-0094: AutomationParamRange — the NATIVE valid range per parameterId, used
// by the EDIT COMMAND layer (EditCommandDispatcher) to clamp every breakpoint
// value before it is written into the PRD-0087 model.
//
// HARD INVARIANT (§1.5.6): the automation model must never hold an out-of-range
// breakpoint value. The applier (PRD-0092) relies on this. The command clamps to
// the FULL native DSP range here — NOT the narrower PRD-0093 display window
// (AutomationValueRange in the Ui layer), which is purely for on-screen mapping.
//
// This helper lives in the (non-Ui) Automation layer so the command layer never
// depends on Source/Features/Daw/Automation/Ui. It is a pure value type: no
// allocation, no JUCE Component, no audio-thread code, message thread only.
//
// Native ranges (documented, sourced from the real DSP constants):
//   filter            [-1, +1]    bipolar (EPIC-0007 / PRD-0056; the authoritative
//                                  mixer `filter` property is a clamped bipolar
//                                  value — MixerParam::normalisedToFilterBipolar).
//   gain / high /     [-60, +12]  dB (EPIC-0007: MixerParam::kMinDb / kMaxGainDb /
//   mid / low                     kMaxEqDb — the exact range MixerParam::clampGainDb
//                                  / the EQ band clamp enforce before the audio
//                                  thread sees a value, and the range the per-
//                                  channel capture taps record in native dB).
//   tempo             [20, 300]   BPM (EPIC-0003 / the app-wide BPM acceptance
//                                  range enforced by DeckShellComponent /
//                                  ControllerWidget / TrackInfoComponent edit
//                                  boxes: `bpm >= 20.0 && bpm <= 300.0`).
//   unknown / future  [-inf,+inf] no constraint (caller-declared parameter; the
//                                  model accepts the value verbatim, mirroring the
//                                  migration-free policy in AutomationParams).
//==============================================================================

#include <juce_core/juce_core.h>

#include <limits>

namespace Daw
{

struct AutomationParamRange
{
    double minValue { -std::numeric_limits<double>::infinity() };
    double maxValue {  std::numeric_limits<double>::infinity() };

    // Clamp a raw value into [minValue, maxValue]. An unconstrained (unknown)
    // parameter passes the value through unchanged.
    double clamp (double value) const noexcept
    {
        return juce::jlimit (minValue, maxValue, value);
    }

    //--------------------------------------------------------------------------
    // The native DSP range for an in-scope continuous parameterId. Both bare
    // ("high") and namespaced ("eq.high") EQ forms resolve, matching
    // AutomationParams::isKnownContinuous.
    //--------------------------------------------------------------------------
    static AutomationParamRange forContinuousParameter (const juce::String& parameterId)
    {
        AutomationParamRange r;

        if (parameterId == "filter")
        {
            // EPIC-0007 / PRD-0056 bipolar filter knob.
            r.minValue = -1.0;  r.maxValue = 1.0;
        }
        else if (parameterId == "gain"
              || parameterId == "high" || parameterId == "eq.high"
              || parameterId == "mid"  || parameterId == "eq.mid"
              || parameterId == "low"  || parameterId == "eq.low")
        {
            // EPIC-0007 dB range: MixerParam::kMinDb (-60) .. kMaxGainDb / kMaxEqDb (+12).
            r.minValue = -60.0; r.maxValue = 12.0;
        }
        else if (parameterId == "tempo")
        {
            // EPIC-0003 app-wide BPM acceptance range.
            r.minValue = 20.0;  r.maxValue = 300.0;
        }
        // else: unknown / future continuous parameter — no constraint.

        return r;
    }
};

} // namespace Daw
