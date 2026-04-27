#pragma once

class MasterClockPublisher;
struct DeckAudioSource;
struct DeckAudioState;

/// Audio-thread P-controller that maintains beat-phase alignment between a
/// synced slave deck and the master clock.
///
/// Called each `processBlock` after `SyncEngine` has set `speedMultiplier`.
/// Computes `correctionMultiplier` (stored in `DeckAudioSource`) which is
/// then multiplied into the final `speed` value before the time-stretcher.
///
/// Algorithm (PRD-0028):
///   effectivePlayhead = playheadAccumulator − stretcherLatency
///   slaveBPM          = deckBPM × speedMultiplier (effective BPM after sync)
///   beatInterval      = (sampleRate × 60.0) / slaveBPM  (samples / beat)
///   rawPhase          = fmod(effectivePlayhead − masterPhaseOriginSample, beatInterval)
///                       / beatInterval  → normalised to [0.0, 1.0)
///   phaseOffsetBeats  = rawPhase  (wrapped to [−0.5, +0.5])
///
/// P-controller:
///   |offset| >= convergenceThreshold  → targetCorrection = 1.0 ± correctionRate
///   |offset| <  convergenceThreshold  → targetCorrection = 1.0
///   correctionMultiplier ramped by correctionRate/correctionWindowBlocks per block
///
/// Audio-thread safety constraints (identical to SyncEngine):
///   NO memory allocation, NO locks, NO I/O, NO JUCE API calls inside process().
class PhaseLockEngine
{
public:
    explicit PhaseLockEngine (MasterClockPublisher& publisher) noexcept
        : publisher_ (publisher) {}

    /// Process one audio block. Audio-thread safe.
    /// Writes source.correctionMultiplier and source.phaseOffset.
    void process (DeckAudioSource& source,
                  DeckAudioState&  state,
                  double           sampleRate) noexcept;

    // Constants exposed for unit tests
    static constexpr double convergenceThreshold = 0.02;  // beats
    static constexpr double correctionRate        = 0.005; // ±0.5% max
    static constexpr int    correctionWindowBlocks = 64;

private:
    MasterClockPublisher& publisher_;
    /// Tracks the previous isSynced state to detect the false→true transition.
    /// Only set true once all guards pass so the snap is deferred until the
    /// deck is actually playing and the master clock is running.
    bool prevIsSyncedInEngine_ = false;
};
