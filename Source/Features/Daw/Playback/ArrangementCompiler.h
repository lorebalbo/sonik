#pragma once
//==============================================================================
// PRD-0079: ArrangementCompiler — message-thread compiler that transforms
// the `daw` juce::ValueTree into an ArrangementSnapshot for publication.
//
// MESSAGE THREAD ONLY.  This class walks the ValueTree (allocation-safe on the
// message thread), resolves each clip's sourceFileId to a streamer-pool handle
// via an injected resolver, and builds a sorted ArrangementSnapshot.
//
// A separate ArrangementRecompileTrigger (see ArrangementRecompileTrigger.h)
// drives the compile/publish cycle and debounces rapid successive edits.
//==============================================================================

#include <functional>
#include <cmath>

#include <juce_data_structures/juce_data_structures.h>

#include "ArrangementSnapshot.h"
#include "ArrangementPublisher.h"
#include "../State/DawState.h"
#include "../State/DawClipModel.h"
#include "../Model/DawClip.h"

namespace Daw
{

class ArrangementCompiler
{
public:
    /// Rich description of a clip passed to the resolver so the playback layer
    /// can assign a per-clip streamer slot and prime it (EPIC-0010).
    struct ClipResolveRequest
    {
        juce::String sourceFileId;          // stable id (content hash), never a path
        juce::String laneKind;              // "Original" / "Instrumental" / "Vocal"
        juce::String clipId;                // unique per clip (Uuid string)
        int64_t      sourceStartSample = 0; // project-rate
        int64_t      sourceEndSample   = 0; // project-rate
        int64_t      sourceLengthSamples = 0;
        int64_t      timelineStartSample = 0; // project-rate
    };

    /// Legacy resolver: maps a sourceFileId string to a streamer-pool slot.
    /// Returns -1 if the file is not yet in the pool (handle sentinel).
    using HandleResolver = std::function<int32_t (const juce::String& sourceFileId)>;

    /// Rich resolver: maps a full clip request to a streamer-pool slot.
    using ClipHandleResolver = std::function<int32_t (const ClipResolveRequest&)>;

    explicit ArrangementCompiler (HandleResolver resolver = {})
    {
        if (resolver)
            resolver_ = [r = std::move (resolver)] (const ClipResolveRequest& req)
            {
                return r (req.sourceFileId);
            };
    }

    /// @param resolver         Per-clip slot resolver (assigns + primes streamers).
    /// @param sampleRateScale  Multiplier applied to every project-rate sample
    ///                         position to convert it to the runtime/device rate.
    ///                         Defaults to 1.0 (no conversion — device == project).
    ArrangementCompiler (ClipHandleResolver resolver, double sampleRateScale)
        : resolver_ (std::move (resolver))
        , sampleRateScale_ (sampleRateScale)
    {}

    void setSampleRateScale (double scale) noexcept { sampleRateScale_ = scale; }


    //--------------------------------------------------------------------------
    /// Compile the `daw` ValueTree into `out`.
    ///
    /// Iterates: daw → tracks → track → lanes → lane → clips → clip
    /// For each clip it:
    ///   1. Reads clip properties
    ///   2. Resolves sourceFileId → sourceReadHandle
    ///   3. Computes timelineEndSample
    ///   4. Appends a ClipEvent to the matching lane
    ///
    /// After all clips are collected, sorts each lane's ClipEvent array
    /// ascending by timelineStartSample (tie: sourceStartSample).
    ///
    /// @param daw  The root "Daw" ValueTree node (PRD-0063 schema).
    /// @param out  The snapshot to populate; reset to empty before filling.
    //--------------------------------------------------------------------------
    void compile (const juce::ValueTree& daw, ArrangementSnapshot& out) const
    {
        // Reset output snapshot.
        out = ArrangementSnapshot{};

        if (!daw.isValid())
            return;

        // We assign each unique laneId an index in the snapshot.  We keep a
        // small flat map (laneId-string → lane-index) since lane counts are
        // very small (≤ kMaxLanes = 12).
        struct LaneMapEntry { juce::String laneId; int index; };
        LaneMapEntry laneMap[kMaxLanes];
        int          laneMapSize = 0;

        auto getOrCreateLaneIndex = [&] (const juce::String& laneId) -> int
        {
            // Linear search is fine for ≤12 entries.
            for (int i = 0; i < laneMapSize; ++i)
                if (laneMap[i].laneId == laneId)
                    return laneMap[i].index;

            if (laneMapSize >= kMaxLanes)
                return -1; // overflow — drop this lane

            const int idx = out.laneCount++;
            laneMap[laneMapSize++] = { laneId, idx };
            return idx;
        };

        // Walk: daw.tracks → track → lanes → lane → clips → clip
        auto tracksNode = daw.getChildWithName (DawIDs::tracks);
        if (!tracksNode.isValid())
            return;

        for (int t = 0; t < tracksNode.getNumChildren(); ++t)
        {
            auto trackNode = tracksNode.getChild (t);
            if (!trackNode.hasType (DawIDs::track))
                continue;

            auto lanesNode = trackNode.getChildWithName (DawIDs::lanes);
            if (!lanesNode.isValid())
                continue;

            for (int l = 0; l < lanesNode.getNumChildren(); ++l)
            {
                auto laneNode = lanesNode.getChild (l);
                if (!laneNode.hasType (DawIDs::lane))
                    continue;

                const juce::String laneIdStr =
                    laneNode.getProperty (DawIDs::laneId).toString();
                const juce::String laneKindStr =
                    laneNode.getProperty (DawIDs::laneKind).toString();
                const int laneIdx = getOrCreateLaneIndex (laneIdStr);
                if (laneIdx < 0)
                    continue; // lane cap exceeded

                auto& laneSnap = out.lanes[laneIdx];

                auto clipsNode = laneNode.getChildWithName (DawIDs::clips);
                if (!clipsNode.isValid())
                    continue;

                for (int c = 0; c < clipsNode.getNumChildren(); ++c)
                {
                    auto clipNode = clipsNode.getChild (c);
                    if (!clipNode.hasType (DawIDs::clip))
                        continue;

                    if (laneSnap.count >= kMaxClipsPerLane)
                        break; // clip cap exceeded for this lane

                    // Read clip fields from ValueTree.
                    const juce::String sourceFileIdStr =
                        clipNode.getProperty (DawClipIDs::sourceFileId).toString();
                    const juce::String clipIdStr =
                        clipNode.getProperty (DawClipIDs::clipId).toString();
                    const int64_t sourceStart =
                        static_cast<int64_t> (static_cast<double> (clipNode.getProperty (DawClipIDs::sourceStartSample)));
                    const int64_t sourceEnd =
                        static_cast<int64_t> (static_cast<double> (clipNode.getProperty (DawClipIDs::sourceEndSample)));
                    const int64_t timelineStart =
                        static_cast<int64_t> (static_cast<double> (clipNode.getProperty (DawClipIDs::timelineStartSample)));
                    const int64_t sourceLength =
                        static_cast<int64_t> (static_cast<double> (clipNode.getProperty (DawClipIDs::sourceLengthSamples)));
                    const float gainDb =
                        static_cast<float> (static_cast<double> (clipNode.getProperty (DawClipIDs::gainDb)));

                    // Resolve streamer-pool handle (and prime the streamer).
                    int32_t handle = -1;
                    if (resolver_)
                    {
                        ClipResolveRequest req;
                        req.sourceFileId        = sourceFileIdStr;
                        req.laneKind            = laneKindStr;
                        req.clipId              = clipIdStr;
                        req.sourceStartSample   = sourceStart;
                        req.sourceEndSample     = sourceEnd;
                        req.sourceLengthSamples = sourceLength;
                        req.timelineStartSample = timelineStart;
                        handle = resolver_ (req);
                    }

                    // Convert project-rate sample positions to runtime/device rate.
                    const auto scale = [this] (int64_t projectSamples) -> int64_t
                    {
                        if (std::abs (sampleRateScale_ - 1.0) < 1.0e-9)
                            return projectSamples;
                        return (int64_t) std::llround ((double) projectSamples * sampleRateScale_);
                    };

                    const int64_t scaledSourceStart   = scale (sourceStart);
                    const int64_t scaledSourceEnd     = scale (sourceEnd);
                    const int64_t scaledTimelineStart = scale (timelineStart);

                    // Compute timelineEndSample: start + crop length (runtime rate).
                    const int64_t scaledTimelineEnd =
                        scaledTimelineStart + (scaledSourceEnd - scaledSourceStart);

                    // Encode sourceFileId as a 64-bit hash for the ClipEvent
                    // (diagnostic use only; audio path uses handle).
                    uint64_t fileIdHash = 0;
                    {
                        // Simple djb2-like hash of the string bytes.
                        const auto* bytes = reinterpret_cast<const unsigned char*> (
                            sourceFileIdStr.toRawUTF8());
                        for (int i = 0; bytes[i] != 0; ++i)
                            fileIdHash = fileIdHash * 31u + bytes[i];
                    }

                    // Convert gainDb to linear gain.
                    const float linearGain = std::pow (10.0f, gainDb / 20.0f);

                    // Append ClipEvent.
                    ClipEvent& ev        = laneSnap.events[laneSnap.count++];
                    ev.sourceFileId      = fileIdHash;
                    ev.sourceReadHandle  = handle;
                    ev.sourceStartSample = scaledSourceStart;
                    ev.sourceEndSample   = scaledSourceEnd;
                    ev.timelineStartSample = scaledTimelineStart;
                    ev.timelineEndSample   = scaledTimelineEnd;
                    ev.gain              = linearGain;
                    ev.laneIndex         = laneIdx;
                }

                // Sort this lane's events by timelineStartSample ascending,
                // then sourceStartSample as tie-breaker.
                if (laneSnap.count > 1)
                {
                    std::sort (laneSnap.events.begin(),
                               laneSnap.events.begin() + laneSnap.count,
                               [] (const ClipEvent& a, const ClipEvent& b)
                               {
                                   if (a.timelineStartSample != b.timelineStartSample)
                                       return a.timelineStartSample < b.timelineStartSample;
                                   return a.sourceStartSample < b.sourceStartSample;
                               });
                }
            }
        }
    }

private:
    ClipHandleResolver resolver_;
    double             sampleRateScale_ { 1.0 };
};

} // namespace Daw
