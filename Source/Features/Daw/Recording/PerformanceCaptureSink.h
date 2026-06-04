#pragma once
//==============================================================================
// PRD-0075: Performance-capture emission seam.
//
// A narrow sink that deck-feature trigger paths (hot cues, beat jumps, and
// later loops / source-mode) call to surface a structural performance event
// into the PRD-0072 stream. Decoupling the emitters from the FIFO keeps the
// existing feature classes ignorant of recording: when no sink is wired (not
// recording, or recording not yet assembled) they do nothing.
//
// The production sink is FifoPerformanceCaptureSink, which is gated by a
// "recording active" predicate and enqueues a flat POD into the lock-free
// PRD-0072 FIFO — safe to call from the audio thread (quantized cue, §1.5.2)
// or the message thread (UI click), per CLAUDE.md.
//==============================================================================

#include "PerformanceEventFifo.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace Daw
{

//==============================================================================
// Emission interface. deckIndex is resolved by the wiring layer (PRD-0078).
struct PerformanceCaptureSink
{
    virtual ~PerformanceCaptureSink() = default;

    // A source-position discontinuity (hot cue or beat jump): the deck leaves
    // `jumpOutSource` and resumes at `jumpInSource` (§1.2). `type` is the named
    // origin (HotCueJump / BeatJump) carried for downstream consumers; the
    // placement engine's split decision keys off the source delta (§1.5.1).
    virtual void captureJump (int                  deckIndex,
                              PerformanceEventType type,
                              std::int64_t         jumpOutSource,
                              std::int64_t         jumpInSource) = 0;

    // PRD-0076 loop capture. Loop-enter: close the lead-in at `engageSource`.
    virtual void captureLoopEnter (int deckIndex, std::int64_t engageSource) = 0;
    // One completed loop pass with the bounds in force for that pass (§1.5.2/§1.5.3).
    virtual void captureLoopPass (int deckIndex, std::int64_t loopIn, std::int64_t loopOut) = 0;
    // Loop-exit at the live source position; loopIn is the pass's in-point.
    virtual void captureLoopExit (int deckIndex, std::int64_t exitSource, std::int64_t loopIn) = 0;

    // PRD-0077: a source-mode / stem-mute change re-targeting the deck's lanes.
    // Carries the deck's instantaneous source position at the switch instant.
    virtual void captureSourceModeChange (int deckIndex, std::int64_t sourcePosition) = 0;
};

//==============================================================================
// Production sink: gates on a recording predicate and enqueues into the FIFO.
class FifoPerformanceCaptureSink final : public PerformanceCaptureSink
{
public:
    FifoPerformanceCaptureSink (PerformanceEventFifo&  fifo,
                                std::function<bool()>  isRecording)
        : fifo_ (fifo), isRecording_ (std::move (isRecording)) {}

    void captureJump (int                  deckIndex,
                      PerformanceEventType type,
                      std::int64_t         jumpOutSource,
                      std::int64_t         jumpInSource) override
    {
        // Only capture while a recording is actually running.
        if (isRecording_ && ! isRecording_())
            return;

        PerformanceEvent e;
        e.type                 = type;
        e.deckIndex            = static_cast<std::uint8_t> (deckIndex);
        e.sourceSamplePosition = jumpOutSource; // out
        e.payload              = jumpInSource;   // in
        fifo_.enqueue (e);                       // noexcept, lock-free
    }

    void captureLoopEnter (int deckIndex, std::int64_t engageSource) override
    {
        enqueue (PerformanceEventType::LoopIn, deckIndex, engageSource, 0);
    }

    void captureLoopPass (int deckIndex, std::int64_t loopIn, std::int64_t loopOut) override
    {
        // out = loopOut (sourceSamplePosition), in = loopIn (payload).
        enqueue (PerformanceEventType::LoopPass, deckIndex, loopOut, loopIn);
    }

    void captureLoopExit (int deckIndex, std::int64_t exitSource, std::int64_t loopIn) override
    {
        enqueue (PerformanceEventType::LoopOut, deckIndex, exitSource, loopIn);
    }

    void captureSourceModeChange (int deckIndex, std::int64_t sourcePosition) override
    {
        enqueue (PerformanceEventType::SourceModeChange, deckIndex, sourcePosition, 0);
    }

private:
    void enqueue (PerformanceEventType type, int deckIndex,
                  std::int64_t sourcePos, std::int64_t payload)
    {
        if (isRecording_ && ! isRecording_())
            return;

        PerformanceEvent e;
        e.type                 = type;
        e.deckIndex            = static_cast<std::uint8_t> (deckIndex);
        e.sourceSamplePosition = sourcePos;
        e.payload              = payload;
        fifo_.enqueue (e);
    }

    PerformanceEventFifo&  fifo_;
    std::function<bool()>  isRecording_;
};

} // namespace Daw
