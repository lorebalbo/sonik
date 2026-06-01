#pragma once
//==============================================================================
// PRD-0079: ArrangementSnapshot — immutable, audio-thread-safe view of the
// recorded arrangement as a sorted set of ClipEvents per lane.
//
// This header defines two plain, trivially-copyable aggregate types:
//   ClipEvent        — one clip's audio-playback metadata (no ValueTree refs)
//   ArrangementSnapshot — fixed-capacity collection of lanes, each holding a
//                         fixed-capacity sorted array of ClipEvents.
//
// AUDIO THREAD SAFETY: both types are POD-like aggregates. The snapshot is
// published and consumed via ArrangementPublisher's SeqLock double-buffer.
// The audio thread NEVER holds a pointer to a juce::ValueTree, a heap
// allocation, or any mutable state inside this structure.
//
// Capacity constants (kMaxLanes, kMaxClipsPerLane) bound the snapshot size at
// compile time, avoiding any heap allocation in the double-buffer or on the
// audio thread.
//==============================================================================

#include <array>
#include <cstdint>
#include <algorithm>

namespace Daw
{

//==============================================================================
// Capacity constants
//==============================================================================

/// Maximum number of concurrent lanes (4 decks × 3 lanes: Original/Instrumental/Vocal).
static constexpr int kMaxLanes        = 12;

/// Maximum clips per lane in the published snapshot.
/// A DJ set producing more clips than this per lane causes extras to be silently
/// dropped at compile time (the recompile trigger logs this condition; the audio
/// thread is unaffected).
static constexpr int kMaxClipsPerLane = 256;

//==============================================================================
// ClipEvent
//==============================================================================

/// Trivially-copyable audio-thread view of one non-destructive clip.
/// All sample positions are in project-rate samples (44 100 Hz).
struct ClipEvent
{
    /// Stable source identifier (maps to streamer pool via sourceReadHandle).
    /// Stored for diagnostics; the audio thread uses sourceReadHandle only.
    uint64_t sourceFileId      { 0 };

    /// Index into the ClipStreamer pool (PRD-0080). -1 = not yet resolved
    /// (streamer not primed); the render engine skips this clip harmlessly.
    int32_t  sourceReadHandle  { -1 };

    /// Source crop start, project-rate samples.
    int64_t  sourceStartSample  { 0 };

    /// Source crop end (exclusive), project-rate samples.
    int64_t  sourceEndSample    { 0 };

    /// Timeline position of clip start, project-rate samples from timeline origin.
    int64_t  timelineStartSample { 0 };

    /// Timeline position of clip end (exclusive): timelineStartSample + (sourceEndSample - sourceStartSample).
    int64_t  timelineEndSample   { 0 };

    /// Linear gain multiplier (converted from gainDb by the compiler).
    float    gain               { 1.0f };

    /// Lane index within the snapshot this event belongs to.
    int32_t  laneIndex          { 0 };
};

//==============================================================================
// LaneSnapshot
//==============================================================================

/// One lane's clip list in the snapshot. Clips are sorted ascending by
/// timelineStartSample (tie-broken by sourceStartSample) by the compiler.
struct LaneSnapshot
{
    /// Number of valid ClipEvents in events[].
    int32_t  count { 0 };

    /// Fixed-capacity array of clip events for this lane.
    std::array<ClipEvent, kMaxClipsPerLane> events {};
};

//==============================================================================
// ArrangementSnapshot
//==============================================================================

/// Complete, immutable, audio-thread-safe view of the arrangement.
/// Holds kMaxLanes LaneSnapshots; only lanes[0..laneCount-1] are valid.
struct ArrangementSnapshot
{
    /// Number of lanes present in the current arrangement.
    int32_t laneCount { 0 };

    /// Per-lane clip lists (only [0..laneCount-1] populated).
    std::array<LaneSnapshot, kMaxLanes> lanes {};

    //--------------------------------------------------------------------------
    // Helpers (called from message or audio thread — read-only, trivial)
    //--------------------------------------------------------------------------

    /// True if the snapshot has no clips on any lane.
    bool isEmpty() const noexcept
    {
        for (int i = 0; i < laneCount; ++i)
            if (lanes[i].count > 0) return false;
        return true;
    }

    /// Total number of clip events across all lanes.
    int totalClipCount() const noexcept
    {
        int total = 0;
        for (int i = 0; i < laneCount; ++i)
            total += lanes[i].count;
        return total;
    }
};

} // namespace Daw
