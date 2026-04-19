---
status: Implemented
epic: EPIC-0002
depends-on:
  - PRD-0021
  - PRD-0011
---

# 1. PRD-0022: Stem-Aware Time Stretching

## 1.1. Problem

When a DJ enables key lock (pitch preservation) while playing a track with active stems, the current time stretcher processes only the original single stereo buffer. With stems active, the audio engine reads from 4 separate stem buffers (PRD-0021) and sums them. If the time stretcher is only applied to the summed output, the stretching algorithm cannot isolate the spectral characteristics of each stem — the result is lower quality than stretching each stem independently.

More critically, the current architecture creates a single `TimeStretcher` (Rubberband) per deck at track load time. When stems are active and key lock is toggled, the stretcher is fed the summed stem output, but if a stem is muted *after* stretching, the stretched output still contains the muted stem's contribution — the mute does not work correctly with key lock active unless each stem is stretched independently.

Without per-stem time stretching, the DJ must choose between key lock quality and stem isolation — an unacceptable trade-off for a professional tool.

## 1.2. Objective

The system provides stem-aware time stretching that:
- Creates one `TimeStretcher` (Rubberband) instance per stem (4 total) when both key lock and stems are active on a deck, replacing the single-stretcher path.
- Stretches each stem independently before per-stem mute/unmute gain is applied, so that muting a stem after stretching correctly removes its contribution from the output.
- Falls back to the existing single-stretcher path when stems are not active (zero overhead, no behavioral change).
- Creates stem stretchers lazily: only allocated when both `keyLockEnabled` transitions to `true` AND `stemsActive == true`. If key lock was already active before stems loaded, stretchers are created at stem activation time.
- Manages CPU budget: 4 stretchers per deck consume ~4x the CPU. The existing CPU load monitor warns if load exceeds 80% for sustained periods.
- Maintains the same audio quality (Rubberband R3 Finer engine, RealTime mode) per stem as the existing single-stretcher implementation.

## 1.3. User Flow

1. The user has a track loaded on Deck A with stems active (PRD-0021). Key lock is off. The audio engine reads from 4 stem buffers, applies per-stem mute/unmute gain, and sums. No stretcher is involved.
2. The user presses the KEY toggle to enable key lock. The message thread detects that both key lock and stems are active. It creates 4 `TimeStretcher` instances (one per stem), primes each with the corresponding stem's audio data, caches their latencies, and publishes them atomically to the `DeckAudioSource`.
3. On the next `processBlock` cycle, the audio thread reads `keyLockOn == true`, `stemsActive == true`, and loads the 4 stem stretcher pointers. For each stem, it feeds the stem's audio at the read-ahead position to the corresponding stretcher, retrieves the stretched output, applies per-stem mute/unmute gain, and sums the results.
4. The user adjusts the pitch fader to 95% speed. All 4 stretchers receive the same `timeRatio = 1.0 / 0.95`. Each stem is stretched independently. The user hears the track at 95% speed with original pitch, and can mute/unmute individual stems cleanly.
5. The user disables key lock. The audio thread switches back to the vinyl read path for each stem (direct buffer reads with linear interpolation at speed). The message thread tears down the 4 stem stretchers (store nullptr atomically, then delete). The user hears pitch shift as expected.
6. The user has key lock already active (single stretcher running) and then stem separation completes. The message thread detects that stems are now active with key lock on. It tears down the single stretcher and creates 4 stem stretchers, one per stem. The transition is seamless.
7. The user ejects the track. All stem stretchers are torn down along with the stem buffers (PRD-0021). The deck returns to its empty state.
8. The CPU load indicator shows elevated usage (~4x baseline). If load exceeds 80% sustained, a visible CPU warning indicator appears near the KEY button. If load exceeds 90% for 3 consecutive blocks, the engine automatically falls back to single-stretcher mode (stretching the summed stem output instead of per-stem), preserving playback at reduced stem isolation quality. A `stemStretchDegraded` flag is published to the ValueTree so the UI can indicate the degradation.

## 1.4. Acceptance Criteria

- [ ] `DeckAudioSource` is extended with an array of 4 stem stretcher atomic pointers: `stemTimeStretchers[4]` (`std::atomic<TimeStretcher*>`, initialized to `nullptr`), and 4 ownership pointers `stemTimeStretchersOwned[4]` (message-thread only).
- [ ] `DeckAudioSource` has pre-allocated scratch buffers for per-stem stretcher I/O: `stemStretchInL[4][MAX_STRETCH_BLOCK]`, `stemStretchInR[4][MAX_STRETCH_BLOCK]`, `stemStretchOutL[4][MAX_STRETCH_BLOCK]`, `stemStretchOutR[4][MAX_STRETCH_BLOCK]` — all audio-thread-only, zero allocation.
- [ ] `AudioEngine` exposes `createStemStretchers(deckId)` and `destroyStemStretchers(deckId)` methods, callable only from the message thread.
- [ ] `createStemStretchers()` creates 4 `TimeStretcher` instances with the same parameters as the existing single stretcher (device sample rate, 2 channels, `maxBlockSize * 4`), primes each with the corresponding stem's audio via `primeWithAudio()`, caches latencies, and publishes via `memory_order_release` stores.
- [ ] `destroyStemStretchers()` stores `nullptr` with `memory_order_release` into all 4 atomic pointers, then deletes the owned instances on the message thread.
- [ ] Stem stretcher creation is triggered when: (a) key lock is toggled ON while stems are active, or (b) stems become active while key lock is already ON. Both paths call `createStemStretchers()`.
- [ ] Stem stretcher destruction is triggered when: (a) key lock is toggled OFF while stem stretchers exist, or (b) stems are cleared/track is ejected while stem stretchers exist, or (c) a new track is loaded. All paths call `destroyStemStretchers()`.
- [ ] In `processBlock`, when `stemsActive && keyLockOn && stemTimeStretchers[0] != nullptr`, the engine uses the per-stem stretching path: for each stem, reads ahead by `stretcherLatency` samples from that stem's buffer, feeds the stretcher, and uses the stretched output.
- [ ] In `processBlock`, when `stemsActive && !keyLockOn`, the engine uses the direct vinyl read path per stem (matching PRD-0021 behavior). The stem stretchers, if they exist (kept warm), are still fed to avoid cold-start, but their output is not used.
- [ ] When `!stemsActive`, the existing single-stretcher path is used unchanged (zero behavioral change from PRD-0011).
- [ ] All 4 stem stretchers share the same `timeRatio` (`1.0 / speed`), updated once per `processBlock` cycle.
- [ ] The stretcher latency is assumed identical across all 4 stem stretchers (same construction parameters). A single `stemStretcherLatency` value is cached and used for all read-ahead calculations.
- [ ] Per-stem mute/unmute gain (PRD-0021 crossfade ramps) is applied to the stretched output, not to the stretcher input. Muted stems are still fed to the stretcher (to keep it warm) but their output is zeroed.
- [ ] If a stem stretcher returns fewer samples than requested (underrun), the engine falls back to the vinyl-read sample for that stem at that position (matching the existing single-stretcher underrun fallback).
- [ ] The existing CPU load monitor (overload flag at 90% for 3 consecutive blocks) is unchanged. No additional monitoring is added in this PRD.
- [ ] When CPU load exceeds 90% for 3 consecutive blocks while stem stretchers are active, the engine gracefully degrades: it falls back to the existing single-stretcher path (stretching the summed stem output) instead of per-stem stretching. A `stemStretchDegraded` (`std::atomic<bool>`) flag is published to the Stems ValueTree node so the UI can display a warning indicator.
- [ ] When CPU load drops below 70% for 10 consecutive blocks after degradation, the engine can optionally re-engage per-stem stretching. However, this re-engagement is NOT automatic in this PRD — the user must toggle key lock off and on to reset to per-stem mode.
- [ ] A visible CPU warning is communicated to PRD-0023: when `stemStretchDegraded == true`, the KEY button or STEMS button should show a warning indicator (defined in PRD-0023).
- [ ] All changes reside within `Source/Features/AudioEngine/` and `Source/Features/TimeStretch/`.
- [ ] The `TimeStretcher` class itself is NOT modified — the same class is instantiated 4 times.
- [ ] All new audio-thread code contains zero allocations, zero locks, and zero I/O.