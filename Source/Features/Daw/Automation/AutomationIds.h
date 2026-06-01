#pragma once
//==============================================================================
// PRD-0087: Automation Data Model — shared ValueTree identifiers and the
// string constants / classification helpers used by the automation lanes.
//
// The automation lives entirely inside the `daw` branch of the central
// juce::ValueTree (PRD-0063). Every type here is MESSAGE-THREAD only: the audio
// thread never walks lanes, it reads the EPIC-0010 published snapshot instead.
//
// SERIALIZATION BOUNDARY: this schema is shaped so the EPIC-0012 serializer can
// persist it directly from the ValueTree. No file I/O, no schema versioning, and
// no project save/load are implemented in EPIC-0011 — that is EPIC-0012's job.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace Daw
{

//==============================================================================
// ValueTree identifiers for the `automation` subtree under the `daw` branch.
//
//   Daw
//     └── automation                 ← container (type "automation")
//           └── lane                  ← one per (owner, parameterId) pair
//                 property: owner          "A"|"B"|"C"|"D"|"master"
//                 property: parameterId    opaque string (e.g. "filter","tempo")
//                 property: kind           "continuous"|"boolean"
//                 property: enabled        bool (per-lane bypass, default true)
//                 ── continuous lane ──
//                 └── breakpoint      ← ordered by timelineSample
//                       property: timelineSample  int64
//                       property: value           double (raw parameter units)
//                       property: interpolation    "linear"|"step"
//                 ── boolean lane ──
//                 └── step            ← ordered by timelineSample
//                       property: timelineSample  int64
//                       property: value           bool
//==============================================================================
namespace AutomationIDs
{
    #define DECLARE_AUTOMATION_ID(name) const juce::Identifier name (#name);

    DECLARE_AUTOMATION_ID (automation)      // container, child of Daw
    DECLARE_AUTOMATION_ID (lane)            // one lane per (owner, parameterId)
    DECLARE_AUTOMATION_ID (owner)           // "A".."D" | "master"
    DECLARE_AUTOMATION_ID (parameterId)     // opaque parameter name
    DECLARE_AUTOMATION_ID (kind)            // "continuous" | "boolean"
    DECLARE_AUTOMATION_ID (enabled)         // bool, per-lane bypass (default true)
    DECLARE_AUTOMATION_ID (breakpoint)      // continuous-lane child
    DECLARE_AUTOMATION_ID (step)            // boolean-lane child
    DECLARE_AUTOMATION_ID (timelineSample)  // int64 playhead position
    DECLARE_AUTOMATION_ID (value)           // double (continuous) / bool (boolean)
    DECLARE_AUTOMATION_ID (interpolation)   // "linear" | "step"

    #undef DECLARE_AUTOMATION_ID
}

//==============================================================================
// Canonical string values.
namespace AutomationStrings
{
    inline constexpr const char* kOwnerA      = "A";
    inline constexpr const char* kOwnerB      = "B";
    inline constexpr const char* kOwnerC      = "C";
    inline constexpr const char* kOwnerD      = "D";
    inline constexpr const char* kOwnerMaster = "master";

    inline constexpr const char* kKindContinuous = "continuous";
    inline constexpr const char* kKindBoolean    = "boolean";

    inline constexpr const char* kInterpLinear = "linear";
    inline constexpr const char* kInterpStep   = "step";
}

//==============================================================================
// Lane kind discriminator.
enum class LaneKind
{
    Continuous,
    Boolean
};

inline juce::String laneKindToString (LaneKind k)
{
    return k == LaneKind::Boolean ? AutomationStrings::kKindBoolean
                                  : AutomationStrings::kKindContinuous;
}

inline LaneKind laneKindFromString (const juce::String& s)
{
    return s == AutomationStrings::kKindBoolean ? LaneKind::Boolean
                                                : LaneKind::Continuous;
}

//==============================================================================
// Interpolation modes (continuous lanes). Per-breakpoint: describes the segment
// that STARTS at that breakpoint. Unknown strings fall back to linear.
enum class Interpolation
{
    Linear,
    Step
};

inline juce::String interpolationToString (Interpolation i)
{
    return i == Interpolation::Step ? AutomationStrings::kInterpStep
                                    : AutomationStrings::kInterpLinear;
}

inline Interpolation interpolationFromString (const juce::String& s)
{
    return s == AutomationStrings::kInterpStep ? Interpolation::Step
                                               : Interpolation::Linear;
}

//==============================================================================
// In-scope parameter-id classification (EPIC-0011 §1.3.1).
//
// These helpers enforce kind-consistency for the explicitly enumerated
// parameters only. Unknown / future ids (e.g. "crossfader") carry NO constraint:
// the caller declares the kind, which is what makes new parameters migration
// free. Both bare ("high") and namespaced ("eq.high") EQ forms are recognised so
// either naming convention round-trips through the model.
namespace AutomationParams
{
    inline bool isKnownContinuous (const juce::String& parameterId)
    {
        return parameterId == "tempo"
            || parameterId == "filter"
            || parameterId == "gain"
            || parameterId == "high"   || parameterId == "eq.high"
            || parameterId == "mid"    || parameterId == "eq.mid"
            || parameterId == "low"    || parameterId == "eq.low";
    }

    inline bool isKnownBoolean (const juce::String& parameterId)
    {
        return parameterId == "keyLock"
            || parameterId == "pitchStretch"
            || parameterId == "keyStepper";
    }

    // True when the id is enumerated as in-scope (either kind).
    inline bool isInScope (const juce::String& parameterId)
    {
        return isKnownContinuous (parameterId) || isKnownBoolean (parameterId);
    }

    // For an in-scope id, true if the requested kind matches the expected kind.
    // Unknown ids always return true (no constraint).
    inline bool kindIsConsistent (const juce::String& parameterId, LaneKind requested)
    {
        if (isKnownContinuous (parameterId)) return requested == LaneKind::Continuous;
        if (isKnownBoolean    (parameterId)) return requested == LaneKind::Boolean;
        return true; // unknown / future parameter: caller declares the kind
    }
}

} // namespace Daw
