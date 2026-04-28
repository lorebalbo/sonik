#include "PhaseLockEngine.h"
#include "MasterClockPublisher.h"
#include "../AudioEngine/DeckAudioSource.h"
#include "../Deck/AudioThreadState.h"
#include "../TimeStretch/TimeStretcher.h"
#include <cmath>

void PhaseLockEngine::process (DeckAudioSource& source,
                                DeckAudioState&  state,
                                double           sampleRate) noexcept
{
    // ------------------------------------------------------------------
    // 1. Engagement guard — ALL four conditions must hold simultaneously.
    //    Any failure → snap correctionMultiplier to 1.0 and publish 0.0f.
    // ------------------------------------------------------------------
    const bool isSynced = state.isSynced.load (std::memory_order_relaxed);
    if (! isSynced)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    // Detect the first block after SYNC engagement.  prevIsSyncedInEngine_ is
    // only committed true when all guards pass, so the snap is deferred to the
    // first block where the deck is playing and the master clock is running.
    const bool justEngaged = ! prevIsSyncedInEngine_;

    // Read snapshot once (SeqLock, lock-free)
    const auto snapshot = publisher_.read();

    if (! snapshot.masterIsPlaying)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    const auto status = static_cast<PlaybackStatusCode> (
        state.playbackStatus.load (std::memory_order_relaxed));
    if (status != PlaybackStatusCode::playing)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    // Guard: slave BPM must be positive and finite
    const double deckBPM  = state.deckBPM.load (std::memory_order_relaxed);
    const double speedMul = static_cast<double> (
        state.speedMultiplier.load (std::memory_order_relaxed));
    const double slaveBPM = deckBPM * speedMul;
    if (slaveBPM <= 0.0 || ! std::isfinite (slaveBPM))
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    // Guard: slip mode active → suspend correction to avoid computing
    // phase from an accumulator the listener is not hearing (PRD-0028 §1.5.5).
    const bool slipEnabled = state.slipEnabled.load (std::memory_order_relaxed);
    if (slipEnabled)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    // Guard: master native BPM must be valid before using it as a divisor below.
    const double masterNativeBPM =
        (snapshot.masterNativeBPM > 0.0 && std::isfinite (snapshot.masterNativeBPM))
            ? snapshot.masterNativeBPM
            : snapshot.masterBPM;
    if (masterNativeBPM <= 0.0 || ! std::isfinite (masterNativeBPM))
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = false;
        return;
    }

    // ------------------------------------------------------------------
    // 2. Phase calculation — native beat intervals for each track
    // ------------------------------------------------------------------
    // Each track has its own beat grid with its own native BPM and anchor.
    // After SyncEngine sets speedMul = masterBPM / deckBPM, the slave plays
    // at masterBPM in real time, but its FILE beat positions are still spaced
    // at nativeSlaveBeatInterval = sampleRate * 60 / deckBPM.
    //
    // The correct formula computes each track's beat FRACTION in its own
    // coordinate system (0.0 = on the beat, 0.5 = halfway through), then
    // differences them.  Using masterBeatInterval as the slave's modulus
    // produces a phase error proportional to (masterBPM − deckBPM)/masterBPM,
    // up to ~6% of a beat at a 8-BPM spread — enough for an audible residual
    // that the slow P-controller (±0.5%) takes 5–30 s to close.
    //
    // fmod normalised to [0, b)
    const auto safeFmod = [] (double a, double b) noexcept -> double
    {
        double r = std::fmod (a, b);
        if (r < 0.0) r += b;
        return r;
    };

    // Account for stretcher latency only when key lock is active.
    // In vinyl mode (key lock off), playheadAccumulator already represents
    // the audible position and no latency offset should be applied.
    const bool keyLockEnabled = state.keyLockEnabled.load (std::memory_order_relaxed);
    const double latencyComp = keyLockEnabled
        ? static_cast<double> (source.stretcherLatency)
        : 0.0;
    const double effectivePlayhead = source.playheadAccumulator - latencyComp;

    // Master beat interval must use native (source-domain) master BPM because
    // masterPlayheadSample is published in source-sample coordinates.
    const double masterBeatInterval = (sampleRate * 60.0) / masterNativeBPM;

    // Slave beat interval: derived from the slave's NATIVE deckBPM (before speedMul).
    // This represents how the slave's OWN beat grid is laid out in its audio file.
    const double nativeSlaveBeatInterval = (sampleRate * 60.0) / deckBPM;

    const double masterPlayhead = static_cast<double> (
        publisher_.masterPlayheadSample.load (std::memory_order_relaxed));
    const double masterAnchor   = static_cast<double> (snapshot.masterPhaseOriginSample);
    const double slaveAnchor    = static_cast<double> (
        state.beatgridAnchor.load (std::memory_order_relaxed));

    // Beat fractions: each in [0.0, 1.0) within its own track's grid.
    const double masterBeatFraction =
        safeFmod (masterPlayhead - masterAnchor, masterBeatInterval) / masterBeatInterval;
    const double slaveBeatFraction =
        safeFmod (effectivePlayhead - slaveAnchor, nativeSlaveBeatInterval) / nativeSlaveBeatInterval;

    // Phase error in fractional beats, wrapped to [-0.5, +0.5].
    // +0.5 = slave is a half-beat ahead → slow down.
    // −0.5 = slave is a half-beat behind → speed up.
    // Exactly ±0.5 stays as-is (both cases are valid snap directions).
    double phaseOffsetBeats = slaveBeatFraction - masterBeatFraction;
    if (phaseOffsetBeats > 0.5)       phaseOffsetBeats -= 1.0;
    else if (phaseOffsetBeats < -0.5) phaseOffsetBeats += 1.0;

    // ------------------------------------------------------------------
    // 2b. Beat snap on initial SYNC engagement
    // ------------------------------------------------------------------
    // On the first block where all guards pass after isSynced transitions
    // false → true, immediately adjust the playhead so the slave's beat
    // grid aligns with the master's.  The ±0.5% P-controller then handles
    // micro-drift correction from the next block onward.
    if (justEngaged)
    {
        // snapDelta is in the slave's FILE sample space — use nativeSlaveBeatInterval,
        // NOT masterBeatInterval.  Using the master's interval introduces an error
        // proportional to (masterBPM − deckBPM)/masterBPM, leaving a residual phase
        // offset that the slow P-controller (±0.5%) would take 5–30 s to close.
        const double snapDelta = phaseOffsetBeats * nativeSlaveBeatInterval;
        source.playheadAccumulator       -= snapDelta;
        source.shadowPlayheadAccumulator -= snapDelta;  // keep slip-mode shadow consistent

        // Clamp: do not rewind past the start of the track.
        if (source.playheadAccumulator < 0.0)
            source.playheadAccumulator += nativeSlaveBeatInterval;
        if (source.shadowPlayheadAccumulator < 0.0)
            source.shadowPlayheadAccumulator += nativeSlaveBeatInterval;

        // Flush the stretcher's internal delay buffer so stale pre-snap audio
        // does not drain out and cause an immediate phase regression.
        if (auto* ts = source.timeStretcher.load (std::memory_order_acquire))
            ts->reset();

        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        prevIsSyncedInEngine_ = true;
        return;
    }

    prevIsSyncedInEngine_ = true;  // mark for subsequent P-controller blocks

    // Publish for UI (PRD-0029 phase meter)
    source.phaseOffset.store (static_cast<float> (phaseOffsetBeats),
                               std::memory_order_relaxed);

    // ------------------------------------------------------------------
    // 3. P-controller: compute target correction and ramp toward it
    // ------------------------------------------------------------------
    double targetCorrection;
    if (std::abs (phaseOffsetBeats) < convergenceThreshold)
    {
        // Within dead-band: target is locked state (1.0)
        targetCorrection = 1.0;
    }
    else if (phaseOffsetBeats > 0.0)
    {
        // Slave is ahead of master → slow down
        targetCorrection = 1.0 - correctionRate;
    }
    else
    {
        // Slave is behind master → speed up
        targetCorrection = 1.0 + correctionRate;
    }

    // Ramp correctionMultiplier one step per block toward targetCorrection.
    // step = correctionRate / correctionWindowBlocks (≈ 0.000078 per block)
    constexpr double step = correctionRate / static_cast<double> (correctionWindowBlocks);
    const double diff = targetCorrection - source.correctionMultiplier;
    if (std::abs (diff) <= step)
        source.correctionMultiplier = targetCorrection;
    else
        source.correctionMultiplier += (diff > 0.0 ? step : -step);
}
