#pragma once

#include <atomic>
#include <cstdint>
#include "AudioBufferHolder.h"
#include "../Deck/AudioThreadState.h"

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
};
