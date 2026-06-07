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

/// EPIC-0009 anti-click crossfade / fade length in render-rate samples.
/// Used as the per-clip fade-in/out ramp length AND, for a butt-joined clip,
/// as the length of the continuation tail it renders past its end so its
/// fade-out overlaps the next clip's fade-in (an equal-power crossfade that
/// matches the live deck's loop crossfade). 64 samples ≈ 1.5 ms @ 44.1 kHz.
static constexpr int kClipFadeSamples = 64;

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

    /// EPIC-0009 recording continuity. A jump/loop split during recording
    /// produces consecutive clips that butt-join on the timeline (one clip's
    /// timelineEndSample equals the next clip's timelineStartSample) from a
    /// single continuous take. At such a shared edge the renderer's anti-click
    /// fade would dip the signal to silence for ~128 samples — the audible
    /// "gap between clips". These flags tell the renderer to SUPPRESS that fade
    /// so the reproduced audio is seamless, matching the deck (where the
    /// loop/jump is a sample-accurate cut). They are left false for an isolated
    /// edge that meets real silence, where the fade still runs to avoid a click.
    bool     joinsPrev          { false }; // contiguous predecessor → crossfade-IN
    bool     joinsNext          { false }; // contiguous successor   → crossfade-OUT
};

//==============================================================================
// Effective render extent
//==============================================================================

/// Timeline sample (exclusive) at which a clip stops contributing audio,
/// INCLUDING the short continuation tail a butt-joined clip renders so its
/// fade-out can crossfade with the next clip's fade-in (EPIC-0009). For an
/// isolated clip this is just timelineEndSample. The renderer and the offline
/// driver's prefetch loop must agree on this so they read the same span.
inline int64_t effectiveTimelineEnd (const ClipEvent& ev) noexcept
{
    return ev.timelineEndSample + (ev.joinsNext ? (int64_t) kClipFadeSamples : 0);
}

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
