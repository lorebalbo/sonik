#pragma once
//==============================================================================
// PRD-0063: ChannelGroup — pure model helper describing a per-deck group of
// exactly three lanes. Builds the canonical lane sub-tree (each lane carrying a
// stable laneId Uuid minted once + a laneKind property) and looks up a lane by
// kind. Depends only on juce_core / juce_data_structures. No UI, no audio thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../State/DawState.h"   // DawIDs::lanes / lane / clips, laneId, laneKind

namespace ChannelGroup
{
    /** The three fixed, universal lanes of every channel group (EPIC-0008
        §1.3.3). The integer values are stable and used for iteration order. */
    enum class LaneKind
    {
        Original     = 0,
        Instrumental = 1,
        Vocal        = 2
    };

    static constexpr int kLaneCount = 3;

    inline juce::String laneKindToString (LaneKind kind)
    {
        switch (kind)
        {
            case LaneKind::Original:     return "Original";
            case LaneKind::Instrumental: return "Instrumental";
            case LaneKind::Vocal:        return "Vocal";
        }
        jassertfalse;
        return "Original";
    }

    inline LaneKind laneKindFromString (const juce::String& s)
    {
        if (s == "Instrumental") return LaneKind::Instrumental;
        if (s == "Vocal")        return LaneKind::Vocal;
        return LaneKind::Original;
    }

    /** Builds a single lane node of the given kind, with a freshly minted
        stable laneId Uuid (stored canonically) and an empty clips container. */
    inline juce::ValueTree createLane (LaneKind kind)
    {
        juce::ValueTree lane (DawIDs::lane);
        lane.setProperty (DawIDs::laneId,   juce::Uuid().toString(),   nullptr);
        lane.setProperty (DawIDs::laneKind, laneKindToString (kind),   nullptr);
        lane.addChild (juce::ValueTree (DawIDs::clips), -1, nullptr);
        return lane;
    }

    /** Builds the canonical "lanes" container with all three lanes pre-created
        (eager, §1.5.5), in Original / Instrumental / Vocal order. Each lane gets
        a distinct laneId Uuid. */
    inline juce::ValueTree createLanesContainer()
    {
        juce::ValueTree lanes (DawIDs::lanes);
        lanes.addChild (createLane (LaneKind::Original),     -1, nullptr);
        lanes.addChild (createLane (LaneKind::Instrumental), -1, nullptr);
        lanes.addChild (createLane (LaneKind::Vocal),        -1, nullptr);
        return lanes;
    }

    /** Looks up a lane node by kind within a "lanes" container (or within a
        "track" node, whose lanes container is resolved automatically). Returns
        an invalid tree if not found. */
    inline juce::ValueTree findLane (juce::ValueTree lanesOrTrack, LaneKind kind)
    {
        auto lanes = lanesOrTrack.hasType (DawIDs::lanes)
                       ? lanesOrTrack
                       : lanesOrTrack.getChildWithName (DawIDs::lanes);

        const auto wanted = laneKindToString (kind);
        for (int i = 0; i < lanes.getNumChildren(); ++i)
        {
            auto l = lanes.getChild (i);
            if (l.hasType (DawIDs::lane)
                && l.getProperty (DawIDs::laneKind).toString() == wanted)
                return l;
        }
        return {};
    }
}
