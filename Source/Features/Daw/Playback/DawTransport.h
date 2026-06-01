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
    void play()
    {
        if (currentState() == State::Stopped)
            playhead_.store (0, std::memory_order_release);  // reset to 0 on play from stop

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
    void seek (int64_t targetSample)
    {
        const int64_t clamped = juce::jmax ((int64_t) 0, targetSample);
        playhead_.store (currentState() == State::Stopped ? -1 : clamped,
                         std::memory_order_release);
        if (onSeeked) onSeeked (clamped);
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
    std::atomic<int64_t>  loopStart_   { 0 };
    std::atomic<int64_t>  loopEnd_     { 0 };
    std::atomic<bool>     loopEnabled_ { false };
};

} // namespace Daw
