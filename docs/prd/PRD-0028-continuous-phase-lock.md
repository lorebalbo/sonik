---
status: Not Implemented
epic: EPIC-0003
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0004
  - PRD-0008
  - PRD-0011
  - PRD-0026
  - PRD-0027
---

# 1. PRD-0028: Continuous Phase Lock

## 1.1. Problem

Tempo sync (PRD-0027) guarantees that a slave deck's BPM matches the master, but it does not guarantee that their beats land at the same absolute moment in time. Even with perfect BPM matching the beats of the two decks can be offset by a fraction of a beat, causing kicks and snares from both decks to arrive at slightly different times. This "flamming" effect — one beat echoing the other a few milliseconds later — is immediately audible to any listener and signals amateur-level mixing to the crowd.

A second, subtler problem compounds the first. When key lock is engaged the slave deck routes its audio through RubberBand's time-stretcher, whose internal processing pipeline introduces a fixed latency (`stretcherLatency` samples). The audible output of the slave deck is therefore not at the position reported by `playheadAccumulator`; it lags behind by exactly that latency. Any phase calculation that ignores this lag computes a phase error relative to a position the listener cannot yet hear, and the system converges to a state that is persistently late by the stretcher latency even when the phase meter appears to show perfect alignment.

Without a continuous, artifact-free phase correction mechanism, the Sync feature delivers only half of the expected result: BPM-matched but phase-misaligned decks that sound noticeably out of sync.

## 1.2. Objective

The system continuously monitors the phase offset between the slave deck's audible beat position and the master clock's beat grid, and applies a fractional micro-tempo correction (`correctionMultiplier`) to converge and maintain phase alignment. The correction:
- Accounts for stretcher latency in the effective playhead calculation, so alignment is to the audible output, not the internal accumulator.
- Wraps the phase offset to the canonical range `[-0.5, +0.5]` beats so the system always takes the shortest path to alignment.
- Is implemented as a proportional (P-only) controller operating within `processBlock`: no integral term, no derivative term, no hard seeks.
- Ramps the correctionMultiplier over a defined window of audio blocks to avoid any perceptible speed transient.
- Publishes the per-deck `phaseOffset` as a `std::atomic<float>` from the audio thread so the phase meter UI (PRD-0029) can display real-time alignment without blocking.
- Is fully audio-thread-safe: no memory allocation, no locks, no I/O inside `processBlock`.

## 1.3. User Flow

1. Deck A is the master. It is playing at 124 BPM. Deck B has a track loaded at 127 BPM. The DJ activates SYNC on deck B.
2. `SyncEngine` (PRD-0027) immediately adjusts deck B's `speedMultiplier` so its effective BPM matches 124. Deck B is now tempo-matched but its current beat may be ahead of or behind deck A's beat by up to 0.5 beats.
3. Each `processBlock` on deck B's `DeckAudioSource` reads the `MasterClockSnapshot` (BPM, `masterPhaseOriginSample`, `masterIsPlaying`) via the lock-free SeqLock reader.
4. The system computes the effective playhead of deck B: `effectivePlayhead = playheadAccumulator − stretcherLatency`. It then calculates `phaseOffsetBeats` — how many beats ahead or behind deck B's audible output is relative to the master beat grid — and wraps the result to `[-0.5, +0.5]`.
5. Because `|phaseOffsetBeats| > convergenceThreshold` (0.02 beats), the system sets `correctionMultiplier` to `1.0 + correctionRate` (deck B is behind, so it speeds up slightly) and begins ramping the multiplier into effect over `correctionWindowBlocks` (64 blocks, approximately 185 ms at 44.1 kHz / 128 samples per block).
6. The final speed applied to the time-stretcher is `speedMultiplier × correctionMultiplier`. The tempo nudge is approximately +0.5% — imperceptible to the listener.
7. Over successive `processBlock` calls the phase offset shrinks. When `|phaseOffsetBeats|` drops below `convergenceThreshold`, `correctionMultiplier` returns to `1.0` and phase lock is held.
8. The current `phaseOffsetBeats` is written to the deck's `std::atomic<float> phaseOffset` each block. The phase meter UI reads this value asynchronously on the message thread with no locking.
9. If the master deck's phase origin shifts (e.g., a cue point jump on the master), the phase offset reappears on the next `processBlock` and the P-controller re-converges from the new offset automatically.
10. The DJ deactivates SYNC on deck B. `correctionMultiplier` is immediately set to `1.0`. The phase offset atomic is set to `0.0f`. The next `processBlock` applies only the deck's natural `speedMultiplier`.

### 1.3.1. Edge Cases

- No track is loaded on the slave deck: correction is not engaged; `phaseOffset` atomic is set to `0.0f`.
- The slave deck's detected BPM is zero or unknown: correction is not engaged; dividing by a zero beat interval would produce undefined behavior and must be explicitly guarded.
- The master deck is paused (`masterIsPlaying = false`): correction is not engaged. The slave remains tempo-matched but accumulates no phase correction, because the master's phase origin is frozen and the comparison would produce a drifting, meaningless delta.
- The slave deck itself is paused or stopped: correction is not engaged regardless of sync state.
- Key lock is off: `stretcherLatency = 0`. The formula `effectivePlayhead = playheadAccumulator − 0` is correct and the logic path is identical. No special-casing needed.
- Slip mode is active on the slave deck (`IDs::slipMode = true`): correction is suspended and `correctionMultiplier` is held at `1.0` for the duration of the slip. See Grey Areas section 1.5.5 for rationale.
- The phase offset is exactly `0.5` beats (the boundary ambiguity case): treated as `+0.5`, requiring a slowdown. See Grey Areas section 1.5.2.

## 1.4. Acceptance Criteria

- [ ] `DeckAudioSource::processBlock` computes `effectivePlayhead` as `playheadAccumulator − stretcherLatency` (using the `stretcherLatency` value already tracked in `DeckAudioSource`). When `stretcherLatency = 0` the formula produces `effectivePlayhead = playheadAccumulator`, which must also be verified correct.
- [ ] `beatInterval` is computed in samples as `(sampleRate × 60.0) / slaveBPM`, using the slave deck's own effective BPM (after tempo-sync speed is applied) rather than the master BPM, so the modular arithmetic operates on the correct beat grid.
- [ ] `phaseOffsetBeats` is computed as `((effectivePlayhead − masterPhaseOriginSample) % beatInterval) / beatInterval`, using integer or floating-point modulo such that the raw result is in the range `[0.0, 1.0)`.
- [ ] The raw phase offset is wrapped to `[-0.5, +0.5]` beats: if `phaseOffsetBeats > 0.5` subtract `1.0`; if `phaseOffsetBeats < -0.5` add `1.0`. A raw offset of exactly `0.5` is treated as `+0.5` (requiring a slowdown).
- [ ] `convergenceThreshold` is `0.02` beats (approximately 2.7 ms at 120 BPM). When `|phaseOffsetBeats| >= convergenceThreshold`, the P-controller is active. When `|phaseOffsetBeats| < convergenceThreshold`, `correctionMultiplier` is `1.0`.
- [ ] When the P-controller is active and `phaseOffsetBeats > 0` (slave is ahead of master), `correctionMultiplier` targets `1.0 − correctionRate` (slow down). When `phaseOffsetBeats < 0` (slave is behind master), `correctionMultiplier` targets `1.0 + correctionRate` (speed up).
- [ ] `correctionRate` is `0.005` (0.5%). The maximum deviation from the base speed at any moment is therefore ±0.5%, which is inaudible as a continuous correction but sufficient to converge a 0.5-beat offset within approximately 100 beats.
- [ ] `correctionMultiplier` is ramped toward its target value over `correctionWindowBlocks = 64` blocks (step per block = `|targetCorrection − 1.0| / correctionWindowBlocks`). It never jumps discontinuously. The ramp is applied incrementally each `processBlock`.
- [ ] The final speed ratio passed to the `TimeStretcher` is `speedMultiplier × correctionMultiplier`, where `speedMultiplier` is the value set by `SyncEngine` (PRD-0027). The two multipliers are strictly multiplicative; neither overwrites the other.
- [ ] `correctionMultiplier` is a plain `double` member variable of `DeckAudioSource` (or the component responsible for audio-thread processing). It is NOT declared `std::atomic` because it is only ever read and written from the audio thread.
- [ ] `phaseOffset` is a `std::atomic<float>` member of `DeckAudioSource` updated each `processBlock` with `static_cast<float>(phaseOffsetBeats)`. When correction is not engaged (any of the four conditions below is false) it is set to `0.0f`.
- [ ] Phase correction engages in `processBlock` if and only if ALL four conditions are true simultaneously:
  - `IDs::isSynced` reads `true` for the slave deck (read via `AudioStateSync` snapshot).
  - `masterIsPlaying` is `true` in the current `MasterClockSnapshot`.
  - The slave deck's transport state is playing (not stopped, not paused).
  - The slave deck's effective BPM is non-zero and finite.
- [ ] Phase correction NEVER calls `seekDeck` or any equivalent seek function. Hard seeks are unconditionally forbidden in this feature. A code-review check must confirm zero calls to seek functions anywhere in the phase-lock code path.
- [ ] When `IDs::isSynced` transitions from `true` to `false` (sync disengaged), `correctionMultiplier` is set to `1.0` immediately on the next `processBlock` call, without completing any in-progress ramp.
- [ ] When any of the four engagement conditions becomes false while a ramp is in progress, `correctionMultiplier` is set to `1.0` immediately and the ramp state is reset, regardless of how far through the ramp the system was.
- [ ] `processBlock` acquires the `MasterClockSnapshot` via `MasterClockPublisher::read()` (the SeqLock reader from PRD-0026). No mutex, no condition variable, and no blocking call of any kind is used inside `processBlock`.
- [ ] No memory allocation occurs inside the phase-lock code path in `processBlock` (no `new`, `delete`, `std::string`, `std::vector`, or any heap operation).
- [ ] All audio-thread reads of sync-related state (`isSynced`, `slaveBPM`, transport status) are read from pre-fetched atomic snapshots prepared by `AudioStateSync` on the message thread. No ValueTree access occurs inside `processBlock`.
- [ ] A unit test covering the following scenarios exists in `Tests/`:
  - Phase offset = 0.0 → correctionMultiplier = 1.0, no correction applied.
  - Phase offset = +0.3 (slave ahead) → correctionMultiplier < 1.0 (slowdown).
  - Phase offset = -0.3 (slave behind) → correctionMultiplier > 1.0 (speedup).
  - Phase offset = 0.8 (unwrapped) → wraps to -0.2 → correctionMultiplier > 1.0 (speedup).
  - stretcherLatency = 1024 → effectivePlayhead = playheadAccumulator − 1024, offset calculated correctly.
  - isSynced = false → correctionMultiplier = 1.0 immediately.
  - masterIsPlaying = false → correctionMultiplier = 1.0.
  - slaveBPM = 0 → no division, correctionMultiplier = 1.0, no crash.

## 1.5. Grey Areas

1. **Choosing `convergenceThreshold`.**
A threshold that is too small causes the P-controller to oscillate perpetually: the correction step per block at ±0.5% is approximately `0.005 × (128 / 44100) × (BPM / 60)` beats per block. At 120 BPM this is roughly `0.00024` beats per block. A threshold of `0.001` beats would require only 4 blocks to cross but the next block's accumulated drift (from numerical precision and sample-rate rounding in the modulo) could push it back above threshold, causing a hunting loop. A threshold of `0.02` beats provides a dead band of approximately 2.7 ms at 120 BPM, which is well below the human auditory fusion threshold for simultaneous drum hits (approximately 5–10 ms). Resolution: `convergenceThreshold = 0.02` beats is adopted as the default. This value is exposed as a named constant so it can be tuned in a later pass without touching the algorithm.

2. **Ambiguous direction at phase offset = 0.5 beats.**
When the wrapped phase offset is exactly `0.5`, the slave deck's beat is equidistant in both directions from the master beat. Going forward (slow down) or backward (speed up) will take the same number of corrections to converge. There is no musically correct answer, but there is a correctness requirement: the tie-breaking rule must be deterministic and stable (the same choice every time the system encounters this value) to avoid a pathological case where the system oscillates between +0.5 and -0.5 on consecutive blocks. Resolution: a raw modulo result of exactly `0.5` is left as `+0.5` (no subtraction), so the system slows down. This is a single arbitrary-but-stable convention encoded as a strict inequality (`> 0.5` triggers the subtraction, `== 0.5` does not).

3. **Ramp state when sync is turned off.**
If the user deactivates SYNC while a correction ramp is in progress (correctionMultiplier = 1.003, ramping toward 1.005), two behaviors are possible: (a) complete the ramp before returning to 1.0 — this avoids a speed discontinuity but means the deck continues deviating from its natural tempo for up to 64 more blocks after SYNC is off; (b) snap immediately to 1.0. Resolution: snap immediately to 1.0. The deviation from snap (at most ±0.5%) is inaudible over a single block boundary. The alternative — a deck drifting in speed after the user has explicitly disabled sync — would be surprising and confusing. Immediate snap is the less surprising behavior and keeps the invariant that `correctionMultiplier = 1.0` whenever sync is disengaged.

4. **Master deck loads a new track or jumps to a cue point mid-mix.**
When the master deck jumps (cue recall, loop exit at a different position, or a new track loaded), `masterPhaseOriginSample` in the published `MasterClockSnapshot` changes abruptly. On the next `processBlock`, the slave's computed `phaseOffsetBeats` may be anywhere in `[-0.5, +0.5]`. The P-controller will begin converging from that new offset. At ±0.5% correction rate, converging from a worst-case 0.5-beat offset takes approximately `0.5 / (0.005 × BPM / 60 × blockSize / sampleRate) ≈ 500` blocks (about 1.5 seconds at 44.1 kHz / 128 samples). This convergence time is intentional: the alternative (a larger correction rate for faster convergence) risks audibly slurring the slave tempo. Phrase-boundary-aware re-convergence (wait for the next 8-beat or 16-beat boundary before starting correction) would require phrase detection, which is explicitly out of scope for this PRD and is deferred to a future enhancement.

5. **Interaction with slip mode (PRD-0017).**
In slip mode the listener hears the audio at the "slip-frozen" playhead position while the internal `playheadAccumulator` continues advancing normally. The gap between the slip-frozen position and the accumulator can be arbitrarily large. If phase lock computes the effective playhead from `playheadAccumulator − stretcherLatency` during slip mode, it measures a position the listener is not hearing, producing corrections that are completely wrong relative to the audible output. Computing against the slip-frozen position is theoretically correct but requires phase lock to be aware of a different feature's internal state, creating an undesirable coupling. Resolution: phase lock is disabled (`correctionMultiplier` held at `1.0`) whenever `IDs::slipMode = true`. This is the safe, decoupled choice. When slip mode exits and the deck snaps back to its natural playhead, phase lock re-engages on the next `processBlock` and re-converges normally from whatever offset exists at that moment.