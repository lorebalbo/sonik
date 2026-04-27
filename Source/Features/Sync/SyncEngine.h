#pragma once

#include "../Deck/AudioThreadState.h"

class MasterClockPublisher;

/// Audio-thread helper that drives speedMultiplier when SYNC is engaged.
///
/// Called from AudioEngine::audioDeviceIOCallbackWithContext for every deck
/// that has isSynced=true. Zero memory allocation, zero locks, zero I/O.
///
/// Algorithm (PRD-0027):
///   1. If state.isSynced == false → return immediately
///   2. Read MasterClockSnapshot via SeqLock (publisher_.read())
///   3. Guard: masterIsPlaying, masterBPM > 0.0, deckBPM > 0.0
///   4. ratio = masterBPM / deckBPM
///   5. Normalise into [0.667, 1.5] to avoid half/double-time errors
///   6. state.speedMultiplier = (float)ratio  (memory_order_relaxed)
class SyncEngine
{
public:
    explicit SyncEngine (MasterClockPublisher& publisher) noexcept
        : publisher_ (publisher) {}

    /// Process one audio block. Audio-thread safe.
    void process (DeckAudioState& state) noexcept;

private:
    MasterClockPublisher& publisher_;
};
