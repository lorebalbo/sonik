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
    //--------------------------------------------------------------------------
    static constexpr int kRampLengthSamples = 64;

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

        // Per-lane scratch (stereo)
        laneBuffer_.setSize (2, blockSize, false, true, false);

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

                // Clip ends before this block starts → skip.
                if (ev.timelineEndSample <= playhead)
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
                    juce::jmin ((int64_t) numSamples,
                               ev.timelineEndSample - playhead));
                const int copyLen = blockEnd - blockStart;
                if (copyLen <= 0)
                    continue;

                // Temporary local buffer for this clip's contribution.
                // We write into laneBuffer_ directly.
                streamer->readInto (laneBuffer_, blockStart, copyLen);

                // Apply per-clip gain and ramps.
                applyGainWithRamps (laneBuffer_, blockStart, copyLen,
                                    ev.gain, ev, playhead, numSamples);

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

    void applyGainWithRamps (juce::AudioBuffer<float>& buffer,
                             int startSample,
                             int numSamples,
                             float gain,
                             const ClipEvent& ev,
                             int64_t playhead,
                             int /*blockLen*/) noexcept
    {
        // Apply per-clip gain uniformly.
        for (int ch = 0; ch < 2; ++ch)
        {
            float* data = buffer.getWritePointer (ch, startSample);
            for (int i = 0; i < numSamples; ++i)
                data[i] *= gain;
        }

        // Fade-in ramp at clip start.
        const int64_t clipStartInBlock = ev.timelineStartSample - playhead;
        if (clipStartInBlock >= 0 && clipStartInBlock < numSamples)
        {
            const int rampStart = static_cast<int> (clipStartInBlock);
            const int rampLen   = juce::jmin (kRampLengthSamples, numSamples - rampStart);

            for (int ch = 0; ch < 2; ++ch)
            {
                float* data = buffer.getWritePointer (ch, startSample + rampStart);
                for (int i = 0; i < rampLen; ++i)
                    data[i] *= rampTable_[i];
            }
        }

        // Fade-out ramp at clip end.
        const int64_t clipEndInBlock = ev.timelineEndSample - playhead;
        if (clipEndInBlock > 0 && clipEndInBlock <= numSamples)
        {
            const int rampEnd   = static_cast<int> (clipEndInBlock);
            const int rampStart = juce::jmax (0, rampEnd - kRampLengthSamples);
            const int rampLen   = rampEnd - rampStart;

            for (int ch = 0; ch < 2; ++ch)
            {
                float* data = buffer.getWritePointer (ch, startSample + rampStart);
                for (int i = 0; i < rampLen; ++i)
                {
                    const int rampIdx = kRampLengthSamples - rampLen + i;
                    const float fadeOut = 1.0f - rampTable_[juce::jmin (rampIdx, kRampLengthSamples - 1)];
                    data[i] *= fadeOut;
                }
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

    // Pre-allocated lane scratch buffer (stereo, maxBlockSize_).
    juce::AudioBuffer<float>     laneBuffer_;

    // Anti-click ramp table.
    float rampTable_[kRampLengthSamples];

    // Per-clip state (indexed by slot handle, bounded).
    std::array<ClipState, kMaxLanes * kMaxClipsPerLane> clipState_;

    // Previous block's active handles per lane (for schedule-change detection).
    std::array<int32_t, kMaxLanes> prevActiveHandles_;
};

} // namespace Daw
