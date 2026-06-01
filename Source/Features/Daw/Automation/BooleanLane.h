#pragma once
//==============================================================================
// PRD-0087: BooleanLane — a message-thread wrapper over a `lane` ValueTree node
// of kind "boolean". Holds an ordered list of `step` child nodes, each carrying
// (timelineSample, value [bool]). A step's value is held until the next step.
//
// The key-stepper lane is boolean (EPIC-0011 §1.3.1): it records engaged/stepped
// transitions only; the signed semitone value remains owned by PRD-0025 and is
// never duplicated here.
//
// INVARIANT: steps are always sorted ascending by timelineSample (stable ties).
// THREADING: message thread only.
//==============================================================================

#include "AutomationIds.h"

namespace Daw
{

class BooleanLane
{
public:
    BooleanLane() = default;
    explicit BooleanLane (juce::ValueTree laneNode) : lane_ (std::move (laneNode)) {}

    bool isValid() const noexcept
    {
        return lane_.isValid()
            && lane_.hasType (AutomationIDs::lane)
            && laneKindFromString (lane_.getProperty (AutomationIDs::kind).toString())
                   == LaneKind::Boolean;
    }

    juce::ValueTree getState() const noexcept { return lane_; }

    int  getNumSteps() const { return lane_.getNumChildren(); }
    bool isEmpty()     const { return lane_.getNumChildren() == 0; }

    juce::ValueTree getStep (int index) const { return lane_.getChild (index); }

    //--------------------------------------------------------------------------
    // Add a step event at the correct sorted position. Stable on ties.
    //--------------------------------------------------------------------------
    juce::ValueTree addStep (std::int64_t timelineSample,
                             bool         value,
                             juce::UndoManager* undo = nullptr)
    {
        juce::ValueTree st (AutomationIDs::step);
        st.setProperty (AutomationIDs::timelineSample, (juce::int64) timelineSample, nullptr);
        st.setProperty (AutomationIDs::value,          value,                        nullptr);

        lane_.appendChild (st, undo);
        sort (undo);
        jassert (isSorted());
        return st;
    }

    void moveStep (juce::ValueTree step,
                   std::int64_t    newTimelineSample,
                   juce::UndoManager* undo = nullptr)
    {
        if (! step.isValid()) return;
        step.setProperty (AutomationIDs::timelineSample, (juce::int64) newTimelineSample, undo);
        sort (undo);
        jassert (isSorted());
    }

    void removeStep (juce::ValueTree step, juce::UndoManager* undo = nullptr)
    {
        if (! step.isValid()) return;
        lane_.removeChild (step, undo);
        jassert (isSorted());
    }

    //--------------------------------------------------------------------------
    // Evaluate: the value of the most recent step at or before the query sample.
    // Defaults to false before the first step (and for an empty lane).
    //--------------------------------------------------------------------------
    bool stateAt (std::int64_t timelineSample) const
    {
        bool state = false;
        for (int i = 0; i < lane_.getNumChildren(); ++i)
        {
            if (sampleOf (i) <= timelineSample)
                state = valueOf (i);
            else
                break;
        }
        return state;
    }

    static std::int64_t sampleOfNode (const juce::ValueTree& st)
    {
        return (std::int64_t) (juce::int64) st.getProperty (AutomationIDs::timelineSample);
    }
    static bool valueOfNode (const juce::ValueTree& st)
    {
        return (bool) st.getProperty (AutomationIDs::value);
    }

private:
    std::int64_t sampleOf (int i) const { return sampleOfNode (lane_.getChild (i)); }
    bool         valueOf (int i)  const { return valueOfNode  (lane_.getChild (i)); }

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
