#pragma once
//==============================================================================
// PRD-0087: AutomationModel — the lane container + (owner, parameterId) keying,
// backed by the `automation` child node of the `daw` ValueTree (PRD-0063).
//
// Responsibilities:
//   - Ensure / expose the `automation` container under the supplied `daw` branch.
//   - Key lanes by the structured pair (owner, parameterId); guarantee at most
//     one lane per pair.
//   - Create continuous / boolean lanes, enforcing kind-consistency for in-scope
//     parameter ids while accepting novel ids with no migration.
//   - Distinguish an EMPTY lane (node exists, zero points) from an ABSENT lane
//     (no node).
//
// Observation is plain JUCE: clients attach a juce::ValueTree::Listener to the
// `automation` node (or a specific `lane` node). No bespoke listener interface.
//
// THREADING: message thread only. No audio-thread state. No serialization here
// (EPIC-0012 persists the tree directly).
//==============================================================================

#include "AutomationIds.h"
#include "ContinuousLane.h"
#include "BooleanLane.h"

#include <vector>

namespace Daw
{

class AutomationModel
{
public:
    //--------------------------------------------------------------------------
    // Construct over the `daw` branch (the "Daw" node). Idempotently ensures an
    // `automation` container child exists.
    //--------------------------------------------------------------------------
    explicit AutomationModel (juce::ValueTree dawBranch, juce::UndoManager* undo = nullptr)
        : daw_ (std::move (dawBranch)), undo_ (undo)
    {
        jassert (daw_.isValid());
        automation_ = daw_.getChildWithName (AutomationIDs::automation);
        if (! automation_.isValid())
        {
            automation_ = juce::ValueTree (AutomationIDs::automation);
            daw_.appendChild (automation_, undo_);
        }
    }

    juce::ValueTree getAutomationTree() const noexcept { return automation_; }

    //--------------------------------------------------------------------------
    // Lane lookup. Returns an invalid tree when no lane exists for the pair
    // (ABSENT). A present-but-empty lane returns a valid tree.
    //--------------------------------------------------------------------------
    juce::ValueTree getLaneNode (const juce::String& owner, const juce::String& parameterId) const
    {
        for (int i = 0; i < automation_.getNumChildren(); ++i)
        {
            auto lane = automation_.getChild (i);
            if (! lane.hasType (AutomationIDs::lane)) continue;
            if (lane.getProperty (AutomationIDs::owner).toString()       == owner
             && lane.getProperty (AutomationIDs::parameterId).toString() == parameterId)
                return lane;
        }
        return {};
    }

    bool hasLane (const juce::String& owner, const juce::String& parameterId) const
    {
        return getLaneNode (owner, parameterId).isValid();
    }

    LaneKind getLaneKind (const juce::ValueTree& lane) const
    {
        return laneKindFromString (lane.getProperty (AutomationIDs::kind).toString());
    }

    //--------------------------------------------------------------------------
    // Continuous lane get-or-create. Returns an invalid ContinuousLane if a lane
    // already exists for the pair with a conflicting (boolean) kind, or if the
    // parameter id is in-scope boolean (kind-consistency, §1.5.7).
    //--------------------------------------------------------------------------
    ContinuousLane getOrCreateContinuousLane (const juce::String& owner,
                                              const juce::String& parameterId)
    {
        if (! AutomationParams::kindIsConsistent (parameterId, LaneKind::Continuous))
            return ContinuousLane {}; // requested continuous for an in-scope boolean id

        auto existing = getLaneNode (owner, parameterId);
        if (existing.isValid())
        {
            if (getLaneKind (existing) != LaneKind::Continuous)
                return ContinuousLane {}; // existing lane has a conflicting kind
            return ContinuousLane { existing };
        }

        return ContinuousLane { createLaneNode (owner, parameterId, LaneKind::Continuous) };
    }

    //--------------------------------------------------------------------------
    // Boolean lane get-or-create. Mirrors the continuous path.
    //--------------------------------------------------------------------------
    BooleanLane getOrCreateBooleanLane (const juce::String& owner,
                                        const juce::String& parameterId)
    {
        if (! AutomationParams::kindIsConsistent (parameterId, LaneKind::Boolean))
            return BooleanLane {}; // requested boolean for an in-scope continuous id

        auto existing = getLaneNode (owner, parameterId);
        if (existing.isValid())
        {
            if (getLaneKind (existing) != LaneKind::Boolean)
                return BooleanLane {}; // existing lane has a conflicting kind
            return BooleanLane { existing };
        }

        return BooleanLane { createLaneNode (owner, parameterId, LaneKind::Boolean) };
    }

    //--------------------------------------------------------------------------
    // Wrap an existing lane node (returns an invalid wrapper if absent / wrong
    // kind).
    //--------------------------------------------------------------------------
    ContinuousLane getContinuousLane (const juce::String& owner, const juce::String& parameterId) const
    {
        auto lane = getLaneNode (owner, parameterId);
        if (lane.isValid() && getLaneKind (lane) == LaneKind::Continuous)
            return ContinuousLane { lane };
        return ContinuousLane {};
    }

    BooleanLane getBooleanLane (const juce::String& owner, const juce::String& parameterId) const
    {
        auto lane = getLaneNode (owner, parameterId);
        if (lane.isValid() && getLaneKind (lane) == LaneKind::Boolean)
            return BooleanLane { lane };
        return BooleanLane {};
    }

    void removeLane (const juce::String& owner, const juce::String& parameterId)
    {
        auto lane = getLaneNode (owner, parameterId);
        if (lane.isValid())
            automation_.removeChild (lane, undo_);
    }

    int getNumLanes() const { return automation_.getNumChildren(); }

    std::vector<juce::ValueTree> getAllLanes() const
    {
        std::vector<juce::ValueTree> out;
        for (int i = 0; i < automation_.getNumChildren(); ++i)
        {
            auto lane = automation_.getChild (i);
            if (lane.hasType (AutomationIDs::lane))
                out.push_back (lane);
        }
        return out;
    }

    //--------------------------------------------------------------------------
    // Per-lane enable / bypass flag (PRD-0092 honours it; PRD-0093 drives it).
    // A lane with no explicit flag is enabled by default.
    //--------------------------------------------------------------------------
    static bool isLaneEnabled (const juce::ValueTree& lane)
    {
        return (bool) lane.getProperty (AutomationIDs::enabled, true);
    }

    void setLaneEnabled (juce::ValueTree lane, bool enabled)
    {
        if (lane.isValid())
            lane.setProperty (AutomationIDs::enabled, enabled, undo_);
    }

private:
    juce::ValueTree createLaneNode (const juce::String& owner,
                                    const juce::String& parameterId,
                                    LaneKind            kind)
    {
        juce::ValueTree lane (AutomationIDs::lane);
        lane.setProperty (AutomationIDs::owner,       owner,                  nullptr);
        lane.setProperty (AutomationIDs::parameterId, parameterId,            nullptr);
        lane.setProperty (AutomationIDs::kind,        laneKindToString (kind), nullptr);
        lane.setProperty (AutomationIDs::enabled,     true,                   nullptr);
        automation_.appendChild (lane, undo_);
        return lane;
    }

    juce::ValueTree    daw_;
    juce::ValueTree    automation_;
    juce::UndoManager* undo_ { nullptr };
};

} // namespace Daw
