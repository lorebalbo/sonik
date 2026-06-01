#pragma once
//==============================================================================
// PRD-0087: ContinuousLane — a message-thread wrapper over a `lane` ValueTree
// node of kind "continuous". Holds an ordered list of `breakpoint` child nodes,
// each carrying (timelineSample, value [raw parameter units], interpolation).
//
// INVARIANT: breakpoints are always sorted ascending by timelineSample. Every
// mutating operation re-establishes that order (stable on ties — a new point at
// an existing sample is placed AFTER the existing one). Callers never sort.
//
// THREADING: message thread only. The audio thread never touches this.
//==============================================================================

#include "AutomationIds.h"

#include <optional>

namespace Daw
{

class ContinuousLane
{
public:
    ContinuousLane() = default;
    explicit ContinuousLane (juce::ValueTree laneNode) : lane_ (std::move (laneNode)) {}

    bool isValid() const noexcept
    {
        return lane_.isValid()
            && lane_.hasType (AutomationIDs::lane)
            && laneKindFromString (lane_.getProperty (AutomationIDs::kind).toString())
                   == LaneKind::Continuous;
    }

    juce::ValueTree getState() const noexcept { return lane_; }

    int getNumBreakpoints() const { return lane_.getNumChildren(); }
    bool isEmpty() const          { return lane_.getNumChildren() == 0; }

    juce::ValueTree getBreakpoint (int index) const { return lane_.getChild (index); }

    //--------------------------------------------------------------------------
    // Add a breakpoint, inserted at the correct sorted position. Returns the new
    // breakpoint node. Stable on ties (placed after an existing equal sample).
    //--------------------------------------------------------------------------
    juce::ValueTree addBreakpoint (std::int64_t  timelineSample,
                                   double        value,
                                   Interpolation interpolation = Interpolation::Linear,
                                   juce::UndoManager* undo = nullptr)
    {
        juce::ValueTree bp (AutomationIDs::breakpoint);
        bp.setProperty (AutomationIDs::timelineSample, (juce::int64) timelineSample, nullptr);
        bp.setProperty (AutomationIDs::value,          value,                        nullptr);
        bp.setProperty (AutomationIDs::interpolation,  interpolationToString (interpolation), nullptr);

        lane_.appendChild (bp, undo);
        sort (undo);
        jassert (isSorted());
        return bp;
    }

    //--------------------------------------------------------------------------
    // Move a breakpoint to a new sample/value, preserving the sorted invariant.
    //--------------------------------------------------------------------------
    void moveBreakpoint (juce::ValueTree breakpoint,
                         std::int64_t    newTimelineSample,
                         double          newValue,
                         juce::UndoManager* undo = nullptr)
    {
        if (! breakpoint.isValid()) return;
        breakpoint.setProperty (AutomationIDs::timelineSample, (juce::int64) newTimelineSample, undo);
        breakpoint.setProperty (AutomationIDs::value,          newValue,                        undo);
        sort (undo);
        jassert (isSorted());
    }

    void setInterpolation (juce::ValueTree breakpoint,
                           Interpolation   interpolation,
                           juce::UndoManager* undo = nullptr)
    {
        if (! breakpoint.isValid()) return;
        breakpoint.setProperty (AutomationIDs::interpolation,
                                interpolationToString (interpolation), undo);
    }

    void removeBreakpoint (juce::ValueTree breakpoint, juce::UndoManager* undo = nullptr)
    {
        if (! breakpoint.isValid()) return;
        lane_.removeChild (breakpoint, undo);
        jassert (isSorted());
    }

    //--------------------------------------------------------------------------
    // Evaluate the curve at a timeline position. Returns nullopt for an empty
    // lane (the applier then leaves the live parameter untouched). Honours the
    // per-segment interpolation stored on the leading breakpoint.
    //--------------------------------------------------------------------------
    std::optional<double> evaluateAt (std::int64_t timelineSample) const
    {
        const int n = lane_.getNumChildren();
        if (n == 0) return std::nullopt;

        // Before the first breakpoint: hold the first value.
        if (timelineSample <= sampleOf (0))
            return valueOf (0);

        // At or after the last breakpoint: hold the last value.
        if (timelineSample >= sampleOf (n - 1))
            return valueOf (n - 1);

        // Find bracketing segment [i, i+1] with sample[i] <= t < sample[i+1].
        for (int i = 0; i < n - 1; ++i)
        {
            const std::int64_t s0 = sampleOf (i);
            const std::int64_t s1 = sampleOf (i + 1);
            if (timelineSample >= s0 && timelineSample < s1)
            {
                const double v0 = valueOf (i);

                if (interpolationOf (i) == Interpolation::Step)
                    return v0; // hold the leading value until the next breakpoint

                const double v1   = valueOf (i + 1);
                const double span = static_cast<double> (s1 - s0);
                if (span <= 0.0) return v0;
                const double frac = static_cast<double> (timelineSample - s0) / span;
                return v0 + (v1 - v0) * frac;
            }
        }
        return valueOf (n - 1);
    }

    //--------------------------------------------------------------------------
    // Property accessors for a breakpoint node.
    //--------------------------------------------------------------------------
    static std::int64_t sampleOfNode (const juce::ValueTree& bp)
    {
        return (std::int64_t) (juce::int64) bp.getProperty (AutomationIDs::timelineSample);
    }
    static double valueOfNode (const juce::ValueTree& bp)
    {
        return (double) bp.getProperty (AutomationIDs::value);
    }
    static Interpolation interpolationOfNode (const juce::ValueTree& bp)
    {
        return interpolationFromString (bp.getProperty (AutomationIDs::interpolation).toString());
    }

private:
    std::int64_t  sampleOf (int i)        const { return sampleOfNode (lane_.getChild (i)); }
    double        valueOf (int i)         const { return valueOfNode  (lane_.getChild (i)); }
    Interpolation interpolationOf (int i) const { return interpolationOfNode (lane_.getChild (i)); }

    struct Comparator
    {
        static int compareElements (const juce::ValueTree& a, const juce::ValueTree& b)
        {
            const std::int64_t sa = sampleOfNode (a);
            const std::int64_t sb = sampleOfNode (b);
            return sa < sb ? -1 : (sa > sb ? 1 : 0);
        }
    };

    void sort (juce::UndoManager* undo)
    {
        Comparator c;
        lane_.sort (c, undo, /*retainOrderOfEquivalentItems*/ true);
    }

    bool isSorted() const
    {
        for (int i = 1; i < lane_.getNumChildren(); ++i)
            if (sampleOf (i) < sampleOf (i - 1)) return false;
        return true;
    }

    juce::ValueTree lane_;
};

} // namespace Daw
