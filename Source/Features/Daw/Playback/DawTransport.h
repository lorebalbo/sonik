#pragma once
//==============================================================================
// PRD-0082: DawTransport — owns the arrangement playback playhead and drives
// the TimelineRenderer.
//
// STATE MACHINE:
//   Stopped → Playing (play())
//   Playing → Paused  (pause())
//   Paused  → Playing (play())
//   Any     → Stopped (stop())
//
// THREADING:
//   playhead_ is a std::atomic<int64_t> advanced each processBlock by the
//   audio thread (via advancePlayhead), and read by the renderer (PRD-0081)
//   and the UI timer.  Transport control (play/pause/stop/seek) is message
//   thread only.  The loop boundaries and state enum are also atomics so the
//   audio thread can observe them without locks.
//
// AUDIO THREAD CONTRACT: advancePlayhead() — no allocation, no lock, no I/O.
//==============================================================================

#include <atomic>
#include <functional>
#include <cstdint>

#include <juce_events/juce_events.h>

namespace Daw
{

class DawTransport final
{
public:
    //--------------------------------------------------------------------------
    // Transport state enum (mirrored in atomic)
    //--------------------------------------------------------------------------
    enum class State : int32_t { Stopped = 0, Playing = 1, Paused = 2 };

    //--------------------------------------------------------------------------
    // Construction
    //--------------------------------------------------------------------------

    DawTransport()
    {
        // Stopped, playhead at 0.
        state_.store (static_cast<int32_t> (State::Stopped), std::memory_order_relaxed);
        playhead_.store (-1, std::memory_order_relaxed);  // -1 = not playing
        loopStart_.store (0, std::memory_order_relaxed);
        loopEnd_.store   (0, std::memory_order_relaxed);
        loopEnabled_.store (false, std::memory_order_relaxed);
    }

    ~DawTransport() = default;

    DawTransport (const DawTransport&)            = delete;
    DawTransport& operator= (const DawTransport&) = delete;

    //--------------------------------------------------------------------------
    // Message-thread transport control
    //--------------------------------------------------------------------------

    /// Start playing from current playhead position.
    ///
    /// PRD-0082 §1.5.1: a play from Stopped resumes from the transport ORIGIN,
    /// not a hardcoded sample 0. The recorded arrangement (PRD-0069) is anchored
    /// at the master-grid phase origin, which is non-zero whenever a master deck
    /// drives the grid; seeding the origin with the first clip's start (see
    /// setOriginSample) makes Play land on the recorded content instead of
    /// playing silence up to it.
    void play()
    {
        if (currentState() == State::Stopped)
        {
            // PRD-0102: a ruler click/scrub while stopped PARKS the playhead at a
            // chosen sample (>= 0). Resume from that parked position so the DJ can
            // "play from here". Only when the playhead is unset (-1, e.g. after a
            // Stop) do we fall back to the transport ORIGIN (PRD-0082 §1.5.1: the
            // start of the recorded arrangement) so Play still lands on the first
            // clip instead of silence.
            const int64_t parked = playhead_.load (std::memory_order_acquire);
            if (parked < 0)
                playhead_.store (originSample_.load (std::memory_order_acquire),
                                 std::memory_order_release);
        }

        state_.store (static_cast<int32_t> (State::Playing), std::memory_order_release);

        if (onStateChanged) onStateChanged (State::Playing);
    }

    /// Pause playback; retain current position.
    void pause()
    {
        if (currentState() != State::Playing) return;
        state_.store (static_cast<int32_t> (State::Paused), std::memory_order_release);
        if (onStateChanged) onStateChanged (State::Paused);
    }

    /// Stop playback and reset playhead to 0.
    void stop()
    {
        state_.store (static_cast<int32_t> (State::Stopped), std::memory_order_release);
        playhead_.store (-1, std::memory_order_release);
        if (onStateChanged) onStateChanged (State::Stopped);
    }

    /// Toggle play/pause.
    void togglePlayPause()
    {
        if (currentState() == State::Playing) pause();
        else                                   play();
    }

    /// Seek to `targetSample` (project samples from timeline origin).
    /// Works in any state.  The audio thread picks up the new position next block.
    ///
    /// PRD-0102: seeking while Stopped now PARKS the playhead at the clamped
    /// position (instead of forcing -1) so a ruler click/scrub can choose a
    /// visible play-start point while stopped. This is safe because the
    /// arrangement render path pulls audio only while isPlaying() is true
    /// (AudioEngine), so a parked playhead produces no sound until Play.
    void seek (int64_t targetSample)
    {
        const int64_t clamped = juce::jmax ((int64_t) 0, targetSample);
        playhead_.store (clamped, std::memory_order_release);
        if (onSeeked) onSeeked (clamped);
    }

    /// Set the transport origin: the sample a play-from-Stopped starts at.
    /// EPIC-0010: the DAW layer seeds this with the start of the recorded
    /// arrangement (DawState::earliestClipStartSample, scaled to the runtime
    /// rate) so Play lands on the first clip. Message thread only.
    void setOriginSample (int64_t originSample) noexcept
    {
        originSample_.store (juce::jmax ((int64_t) 0, originSample),
                             std::memory_order_release);
    }

    /// Current transport origin (project/runtime samples). Any thread.
    int64_t getOriginSample() const noexcept
    {
        return originSample_.load (std::memory_order_acquire);
    }

    /// Set the loop region.
    void setLoopRegion (int64_t startSample, int64_t endSample)
    {
        loopStart_.store (startSample, std::memory_order_relaxed);
        loopEnd_.store   (endSample,   std::memory_order_relaxed);
    }

    /// Arm or disarm loop review.
    void setLoopEnabled (bool enabled)
    {
        loopEnabled_.store (enabled, std::memory_order_relaxed);
    }

    bool isLoopEnabled() const noexcept
    {
        return loopEnabled_.load (std::memory_order_relaxed);
    }

    /// Current loop region start (project samples). Any thread.
    int64_t getLoopStart() const noexcept
    {
        return loopStart_.load (std::memory_order_relaxed);
    }

    /// Current loop region end, exclusive (project samples). Any thread.
    int64_t getLoopEnd() const noexcept
    {
        return loopEnd_.load (std::memory_order_relaxed);
    }

    /// Toggle loop armed state.
    void toggleLoop()
    {
        setLoopEnabled (! isLoopEnabled());
    }

    /// Convenience alias matching the enum accessor style used by DawPanel.
    State getState() const noexcept { return currentState(); }

    //--------------------------------------------------------------------------
    // Audio-thread API
    //--------------------------------------------------------------------------

    /// Called from processBlock.  Advances the playhead by numSamples if
    /// Playing; wraps to loopStart if loop is armed and end is reached.
    /// No allocation, no lock, no I/O.
    void advancePlayhead (int numSamples) noexcept
    {
        if (static_cast<State> (state_.load (std::memory_order_acquire)) != State::Playing)
            return;

        int64_t pos = playhead_.load (std::memory_order_relaxed);
        if (pos < 0) pos = 0;

        pos += numSamples;

        // Loop wrap.
        if (loopEnabled_.load (std::memory_order_relaxed))
        {
            const int64_t loopEnd = loopEnd_.load (std::memory_order_relaxed);
            if (loopEnd > 0 && pos >= loopEnd)
                pos = loopStart_.load (std::memory_order_relaxed);
        }

        playhead_.store (pos, std::memory_order_release);
    }

    //--------------------------------------------------------------------------
    // Read-only accessors (any thread)
    //--------------------------------------------------------------------------

    /// Current playhead in project samples.  -1 when Stopped.
    int64_t getPlayheadSample() const noexcept
    {
        return playhead_.load (std::memory_order_acquire);
    }

    State currentState() const noexcept
    {
        return static_cast<State> (state_.load (std::memory_order_acquire));
    }

    bool isPlaying() const noexcept { return currentState() == State::Playing; }
    bool isPaused()  const noexcept { return currentState() == State::Paused;  }
    bool isStopped() const noexcept { return currentState() == State::Stopped; }

    /// Direct reference to the playhead atomic for the renderer.
    std::atomic<int64_t>& playheadAtomic() noexcept { return playhead_; }

    //--------------------------------------------------------------------------
    // Callbacks (set on the message thread)
    //--------------------------------------------------------------------------
    std::function<void (State)>   onStateChanged;
    std::function<void (int64_t)> onSeeked;

private:
    std::atomic<int32_t>  state_       { static_cast<int32_t> (State::Stopped) };
    std::atomic<int64_t>  playhead_    { -1 };
    std::atomic<int64_t>  originSample_ { 0 };
    std::atomic<int64_t>  loopStart_   { 0 };
    std::atomic<int64_t>  loopEnd_     { 0 };
    std::atomic<bool>     loopEnabled_ { false };
};

} // namespace Daw
