#include "PhaseLockEngine.h"
#include "MasterClockPublisher.h"
#include "../AudioEngine/DeckAudioSource.h"
#include "../Deck/AudioThreadState.h"
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
        return;
    }

    // Read snapshot once (SeqLock, lock-free)
    const auto snapshot = publisher_.read();

    if (! snapshot.masterIsPlaying)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const auto status = static_cast<PlaybackStatusCode> (
        state.playbackStatus.load (std::memory_order_relaxed));
    if (status != PlaybackStatusCode::playing)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
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
        return;
    }

    // Guard: slip mode active → suspend correction to avoid computing
    // phase from an accumulator the listener is not hearing (PRD-0028 §1.5.5).
    const bool slipEnabled = state.slipEnabled.load (std::memory_order_relaxed);
    if (slipEnabled)
    {
        source.correctionMultiplier = 1.0;
        source.phaseOffset.store (0.0f, std::memory_order_relaxed);
        return;
    }

    // ------------------------------------------------------------------
    // 2. Phase calculation
    // ------------------------------------------------------------------
    // Account for stretcher latency: the audible output lags by this many
    // samples behind the internal accumulator.
    const double effectivePlayhead =
        source.playheadAccumulator - static_cast<double> (source.stretcherLatency);

    const double beatInterval = (sampleRate * 60.0) / slaveBPM;

    // fmod can return negative values when the dividend is negative —
    // normalise to [0.0, 1.0) explicitly.
    double rawOffset = std::fmod (
        effectivePlayhead - static_cast<double> (snapshot.masterPhaseOriginSample),
        beatInterval);
    if (rawOffset < 0.0)
        rawOffset += beatInterval;
    double phaseOffsetBeats = rawOffset / beatInterval; // [0.0, 1.0)

    // Wrap to [-0.5, +0.5].  Exactly 0.5 stays as +0.5 (strict > to match spec).
    if (phaseOffsetBeats > 0.5)
        phaseOffsetBeats -= 1.0;

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
