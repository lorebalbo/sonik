#pragma once
//==============================================================================
// Grouped-tracks mute/solo: the ONE audibility rule shared by the arrangement
// compiler (audio), the lane views and the collapsed-group overview strip (UI),
// so what you hear is always exactly what the headers show.
//
// Both the track node (the deck group: DECK 1..4) and each lane node
// (Original / Instrumental / Vocal) carry boolean `muted` / `solo` properties
// (DawIDs::muted / DawIDs::solo, default false). The rules are Logic-style:
//   - Solo scope is GLOBAL across the whole arrangement: as soon as any track
//     or lane is soloed, everything that is not soloed (and not inside a
//     soloed group) falls silent.
//   - Mute always wins: a muted lane, or any lane inside a muted group, is
//     silent even when soloed.
//
// Pure ValueTree reads on the message thread; no UI, no audio-thread access
// (the audio thread only ever sees the compiled ArrangementSnapshot).
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../State/DawState.h"

namespace Daw::MuteSolo
{
    inline bool readFlag (const juce::ValueTree& node, const juce::Identifier& id)
    {
        return static_cast<bool> (node.getProperty (id, false));
    }

    /** True when any track or lane anywhere in the daw branch is soloed.
        Accepts the "Daw" branch, its "tracks" container, or a single track. */
    inline bool anySoloActive (const juce::ValueTree& dawBranchOrTracks)
    {
        auto tracks = dawBranchOrTracks.hasType (DawIDs::tracks)
                        ? dawBranchOrTracks
                        : dawBranchOrTracks.getChildWithName (DawIDs::tracks);
        if (! tracks.isValid())
            return false;

        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto track = tracks.getChild (t);
            if (! track.hasType (DawIDs::track))
                continue;
            if (readFlag (track, DawIDs::solo))
                return true;

            auto lanes = track.getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
                if (lanes.getChild (l).hasType (DawIDs::lane)
                    && readFlag (lanes.getChild (l), DawIDs::solo))
                    return true;
        }
        return false;
    }

    /** Whether a lane sounds, given its owning track and the global solo state
        (pass the result of anySoloActive() so callers scan the tree once). */
    inline bool isLaneAudible (const juce::ValueTree& trackNode,
                               const juce::ValueTree& laneNode,
                               bool soloActive)
    {
        if (readFlag (trackNode, DawIDs::muted) || readFlag (laneNode, DawIDs::muted))
            return false;
        if (! soloActive)
            return true;
        return readFlag (trackNode, DawIDs::solo) || readFlag (laneNode, DawIDs::solo);
    }
}
