#pragma once

#include <atomic>
#include <cstdint>
#include "AudioBufferHolder.h"
#include "../Deck/AudioThreadState.h"

// Transport commands sent from UI thread to audio thread (PRD-0004)
enum class TransportCommand : int
{
    None = 0,
    Play,
    Pause,
    Stop,
    Seek,
    CueSet,
    CueReturn,
    CuePreview,
    CueRelease,
    CuePlayThrough
};

/// Lightweight struct representing a deck's audio contribution.
/// The audio thread reads raw channel pointers via atomics (zero-alloc).
/// The message thread owns the AudioBufferHolder and sets up pointers.
struct DeckAudioSource
{
    // Raw channel pointers for audio thread (non-owning, point into holder's buffer)
    std::atomic<const float*> channelL { nullptr };
    std::atomic<const float*> channelR { nullptr };
    std::atomic<int64_t>      bufferNumFrames { 0 };

    // Ownership of decoded buffer (message thread only, NOT accessed by audio thread)
    AudioBufferHolder::Ptr bufferHolder;

    // Audio state from PRD-0001 (set by AudioEngine::registerDeck)
    DeckAudioState* audioState = nullptr;

    // Metering output (written by audio thread, read by UI)
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };
    std::atomic<float> rmsL  { 0.0f };
    std::atomic<float> rmsR  { 0.0f };

    // --- Transport (PRD-0004) ---

    // Command slot from UI thread (consumed by audio thread)
    std::atomic<int>     pendingCommand { 0 };
    std::atomic<int64_t> seekTarget     { 0 };

    // Playhead sub-sample accumulator (audio thread only)
    double playheadAccumulator = 0.0;

    // Temp cue preview state (audio thread only)
    bool isCuePreviewing = false;

    // Fade ramp state (audio thread only)
    int  fadeRampSamplesRemaining = 0;
    bool isFadingIn  = false;
    bool isFadingOut = false;
    static constexpr int FADE_RAMP_LENGTH = 64;

    // Deferred action after fade-out completes (audio thread only)
    enum class DeferredAction : int { None = 0, Pause, Stop, Seek, CueReturn, EndOfTrack };
    DeferredAction deferredAction    = DeferredAction::None;
    int64_t        deferredSeekTarget = 0;
};
