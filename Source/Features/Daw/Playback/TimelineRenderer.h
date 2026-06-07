#pragma once
//==============================================================================
// PRD-0081: TimelineRenderer — real-time-safe per-block player.
//
// Reads the SeqLock-published ArrangementSnapshot (PRD-0079), resolves active
// clips per block, copies project-rate samples from ClipStreamer ring buffers
// (PRD-0080), applies per-clip gain, applies 64-sample anti-click ramps at
// clip boundaries and schedule changes, and sums all lanes into a stereo
// masterFeed buffer that feeds MasterStage (PRD-0058).
//
// AUDIO THREAD CONTRACT (CLAUDE.md §"The Audio Thread"):
//   renderBlock() → no allocation, no lock, no I/O, no ValueTree access.
//   All buffers and tables are pre-allocated in prepare().
//==============================================================================

#include <atomic>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>

#include <juce_audio_basics/juce_audio_basics.h>

#include "ArrangementSnapshot.h"
#include "ArrangementPublisher.h"
#include "ClipStreamer.h"

namespace Daw
{

//==============================================================================
// TimelineRenderer
//==============================================================================

class TimelineRenderer
{
public:
    //--------------------------------------------------------------------------
    // Ramp length for anti-click fade-in / fade-out at clip boundaries.
    // Shared with the compiler / offline driver via ArrangementSnapshot so a
    // butt-joined clip's continuation tail and this fade always match length.
    //--------------------------------------------------------------------------
    static constexpr int kRampLengthSamples = kClipFadeSamples;

    //--------------------------------------------------------------------------
    // Construction
    //--------------------------------------------------------------------------

    explicit TimelineRenderer (ArrangementPublisher& publisher,
                               ClipStreamerPool&     streamerPool,
                               std::atomic<int64_t>& transportPlayhead)
        : publisher_          (publisher)
        , streamerPool_        (streamerPool)
        , transportPlayhead_   (transportPlayhead)
    {}

    ~TimelineRenderer() = default;

    TimelineRenderer (const TimelineRenderer&) = delete;
    TimelineRenderer& operator= (const TimelineRenderer&) = delete;

    //--------------------------------------------------------------------------
    // Lifecycle (message thread)
    //--------------------------------------------------------------------------

    /// Pre-allocate all audio-thread state.  Call before the audio callback starts.
    void prepare (double sampleRate, int blockSize, int maxLanes, int maxClipsPerLane)
    {
        sampleRate_    = sampleRate;
        maxBlockSize_  = blockSize;
        maxLanes_      = juce::jmin (maxLanes, kMaxLanes);
        maxClipsPerLane_ = juce::jmin (maxClipsPerLane, kMaxClipsPerLane);

        // Per-lane accumulator + per-clip scratch (stereo). Each clip renders
        // into clipBuffer_ then is ADDED into laneBuffer_, so clips that overlap
        // on a lane (a butt-joined pair's crossfade region) mix instead of
        // overwriting. For disjoint clips this is identical to a direct copy.
        laneBuffer_.setSize (2, blockSize, false, true, false);
        clipBuffer_.setSize (2, blockSize, false, true, false);

        // Ramp coefficient table: linear fade 0→1 over kRampLengthSamples.
        for (int i = 0; i < kRampLengthSamples; ++i)
            rampTable_[i] = static_cast<float> (i) / static_cast<float> (kRampLengthSamples - 1);

        // Pre-build per-clip state arrays.
        clipState_.fill (ClipState{});
        prevActiveHandles_.fill (-1);
    }

    void releaseResources() noexcept {}

    //--------------------------------------------------------------------------
    // Audio-thread render entry point
    //--------------------------------------------------------------------------

    /// Called once per processBlock.  Reads the published snapshot, finds
    /// active clips, copies + gains them, applies boundary ramps, and accumulates
    /// into `masterFeed`.
    ///
    /// @param masterFeed  Stereo buffer, caller-cleared; renderer ADDS into it.
    /// @param numSamples  Number of samples to render (≤ blockSize from prepare).
    void renderBlock (juce::AudioBuffer<float>& masterFeed, int numSamples) noexcept
    {
        // 1. Read playhead.
        const int64_t playhead = transportPlayhead_.load (std::memory_order_acquire);

        // If transport is not running (playhead == stopped sentinel), do nothing.
        if (playhead < 0)
            return;

        // 2. Acquire a coherent snapshot via SeqLock.
        ArrangementSnapshot snap;
        publisher_.read (snap);

        // 3. For each lane, find active clips and accumulate.
        for (int laneIdx = 0; laneIdx < snap.laneCount; ++laneIdx)
        {
            const LaneSnapshot& lane = snap.lanes[laneIdx];

            // Clear lane scratch buffer.
            laneBuffer_.clear();

            bool laneHasSamples = false;

            // Forward-scan (lane is sorted by timelineStartSample).
            for (int ci = 0; ci < lane.count; ++ci)
            {
                const ClipEvent& ev = lane.events[ci];

                // Effective end includes a butt-joined clip's continuation tail
                // so its fade-out can overlap the next clip's fade-in.
                const int64_t clipEnd = effectiveTimelineEnd (ev);

                // Clip ends before this block starts → skip.
                if (clipEnd <= playhead)
                    continue;

                // Clip starts after this block ends → no more clips can overlap.
                if (ev.timelineStartSample >= playhead + numSamples)
                    break;

                // This clip overlaps [playhead, playhead+numSamples).
                const int32_t handle = ev.sourceReadHandle;
                if (handle < 0)
                    continue; // unresolved streamer — skip silently

                ClipStreamer* streamer = streamerPool_.getStreamer (handle);
                if (streamer == nullptr)
                    continue;

                // Compute overlap region in block-local samples.
                const int blockStart = static_cast<int> (
                    juce::jmax ((int64_t) 0, ev.timelineStartSample - playhead));
                const int blockEnd = static_cast<int> (
                    juce::jmin ((int64_t) numSamples, clipEnd - playhead));
                const int copyLen = blockEnd - blockStart;
                if (copyLen <= 0)
                    continue;

                // Render this clip into its own scratch, then ADD into the lane
                // accumulator. Adding (not copying) lets a butt-joined pair's
                // overlapping crossfade region sum to a constant-power blend;
                // for disjoint clips the add lands on cleared samples (== copy).
                streamer->readInto (clipBuffer_, blockStart, copyLen);

                applyGainWithRamps (clipBuffer_, blockStart, copyLen,
                                    ev.gain, ev, playhead);

                for (int ch = 0; ch < 2; ++ch)
                    laneBuffer_.addFrom (ch, blockStart,
                                         clipBuffer_.getReadPointer (ch, blockStart),
                                         copyLen);

                laneHasSamples = true;
            }

            // 4. Accumulate lane into masterFeed.
            if (laneHasSamples)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    masterFeed.addFrom (ch, 0,
                                       laneBuffer_.getReadPointer (ch),
                                       numSamples);
                }
            }
        }
    }

private:
    //--------------------------------------------------------------------------
    // Anti-click ramp application
    //--------------------------------------------------------------------------

    // Applies per-clip gain and the boundary fades for the [startSample,
    // startSample+numSamples) span of `buffer`, whose first sample sits at
    // timeline position (playhead + startSample).
    //
    // Fades are positioned by ABSOLUTE timeline sample, not block-local offset,
    // so they land correctly for clips that start mid-block and for fades that
    // straddle a block boundary. A butt-joined clip's fade-out is shifted onto
    // its continuation tail [timelineEnd, timelineEnd+kRamp) so it overlaps the
    // next clip's fade-in [timelineStart, timelineStart+kRamp) — together they
    // form an equal-power linear crossfade once the two clips are summed.
    void applyGainWithRamps (juce::AudioBuffer<float>& buffer,
                             int startSample,
                             int numSamples,
                             float gain,
                             const ClipEvent& ev,
                             int64_t playhead) noexcept
    {
        const int64_t fadeInStart  = ev.timelineStartSample;
        const int64_t fadeOutEnd   = effectiveTimelineEnd (ev);          // tail-aware
        const int64_t fadeOutStart = fadeOutEnd - kRampLengthSamples;
        const int     lastRamp     = kRampLengthSamples - 1;

        for (int ch = 0; ch < 2; ++ch)
        {
            float* data = buffer.getWritePointer (ch, startSample);
            for (int i = 0; i < numSamples; ++i)
            {
                const int64_t tl = playhead + startSample + i; // timeline sample
                float g = gain;

                // Fade-in over [start, start+kRamp): rampTable rises 0→1.
                if (tl >= fadeInStart && tl < fadeInStart + kRampLengthSamples)
                    g *= rampTable_[(int) (tl - fadeInStart)];

                // Fade-out over [end-kRamp, end): mirror falls 1→0.
                if (tl >= fadeOutStart && tl < fadeOutEnd)
                    g *= 1.0f - rampTable_[juce::jmin ((int) (tl - fadeOutStart), lastRamp)];

                data[i] *= g;
            }
        }
    }

    //--------------------------------------------------------------------------
    // Per-clip persistent state (ramp continuity across blocks)
    //--------------------------------------------------------------------------

    struct ClipState
    {
        int32_t handle          { -1 };
        float   lastGain        { 1.0f };
        int     rampPhase       { 0 };    // unused in current impl (reserved)
    };

    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------

    ArrangementPublisher&  publisher_;
    ClipStreamerPool&       streamerPool_;
    std::atomic<int64_t>&  transportPlayhead_;

    double sampleRate_       { 44100.0 };
    int    maxBlockSize_     { 512 };
    int    maxLanes_         { kMaxLanes };
    int    maxClipsPerLane_  { kMaxClipsPerLane };

    // Pre-allocated lane accumulator + per-clip scratch (stereo, maxBlockSize_).
    juce::AudioBuffer<float>     laneBuffer_;
    juce::AudioBuffer<float>     clipBuffer_;

    // Anti-click ramp table.
    float rampTable_[kRampLengthSamples];

    // Per-clip state (indexed by slot handle, bounded).
    std::array<ClipState, kMaxLanes * kMaxClipsPerLane> clipState_;

    // Previous block's active handles per lane (for schedule-change detection).
    std::array<int32_t, kMaxLanes> prevActiveHandles_;
};

} // namespace Daw
