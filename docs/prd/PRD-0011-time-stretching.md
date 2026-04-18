---
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0004
  - PRD-0010
---

# 1. PRD-0011: Time Stretching

## 1.1. Problem

After PRD-0004 (Transport) and PRD-0010 (Pitch Fader) are implemented, a DJ can adjust playback speed via the pitch fader, but every speed change also shifts the musical pitch proportionally — the classic vinyl behavior where speeding up a record makes it sound higher. This is acceptable for small adjustments (+/-2%) where the pitch shift is barely noticeable, but becomes musically destructive at larger ranges. A track sped up by 6% shifts pitch by roughly a semitone, audibly warping vocals and melodies. Professional DJs mixing across genres or BPM ranges routinely need 4-8% speed adjustments, and without pitch-independent time stretching, these transitions sound unnatural and amateurish. Every competing product (Traktor, Serato, Rekordbox) offers a "Key Lock" toggle that decouples speed from pitch. Without this feature, Sonik forces DJs to choose between tempo-matched mixes and tonally correct audio — a trade-off no professional will accept.

## 1.2. Objective

The system provides per-deck real-time time stretching via the Rubber Band Library that:
- Decouples playback speed from musical pitch when key lock is enabled, allowing the DJ to change tempo via the pitch fader without shifting the track's key.
- Integrates into the per-deck audio pipeline between the decoded audio buffer read and the gain stage, processing audio in real time within `processBlock` without violating audio-thread safety (no allocations, no locks, no I/O after initialization).
- Exposes a per-deck key lock toggle in the deck state tree (PRD-0001) as a boolean, observable by UI and future MIDI mapping via the JUCE Listener pattern.
- Falls back to the existing vinyl-mode playback (PRD-0004 linear interpolation) when key lock is disabled, adding zero CPU overhead for decks not using the feature.
- Maintains output quality with no audible artifacts at speed adjustments within +/-8% of original tempo, and acceptable quality up to +/-16%.
- Reports its processing latency so that the transport system can compensate the playhead position, keeping waveform display and elapsed time synchronized with audible output.
- Handles real-time speed changes smoothly, with no clicks, pops, or dropouts when the DJ moves the pitch fader while key lock is active.

## 1.3. User Flow

1. The user has Deck A with a track loaded and playing at original speed (pitch fader at 0%, key lock off). Audio plays with vinyl-mode behavior — the Rubber Band stretcher is bypassed entirely and the transport uses linear interpolation as defined in PRD-0004.
2. The user clicks the Key Lock button on Deck A. The button illuminates (active state). The state tree's `keyLockEnabled` property for Deck A updates to `true`. Since the pitch fader is at 0%, no audible change occurs — the stretcher initializes with a 1.0 time ratio and passes audio through.
3. The user drags the pitch fader to +6% (speedMultiplier 1.06). The transport advances the playhead 6% faster (reading source audio at an accelerated rate), but the Rubber Band stretcher processes the audio to maintain the original pitch. The DJ hears the track playing faster at the original key.
4. The user continues adjusting the pitch fader in real time. The stretcher smoothly adapts to the changing time ratio on each `processBlock` cycle. No clicks, pops, or discontinuities are audible during fader movement.
5. The user moves the pitch fader to +14%. The stretcher operates at the edge of its quality range. The DJ may notice subtle artifacts (slight metallic quality or transient smearing) but the output remains usable for mixing.
6. The user clicks the Key Lock button again to disable key lock. The button deactivates. The system transitions smoothly back to vinyl-mode playback: the pitch fader at +14% now produces both a 14% speed increase and a corresponding pitch shift upward. A short crossfade (64 samples) blends between the stretcher output and the vinyl-mode output to prevent an audible click at the transition.
7. The user loads a new track onto Deck A. Per PRD-0001, pitch resets to 0% and speedMultiplier resets to 1.0. The key lock state persists (it is deck-level state) — the button remains illuminated. The Rubber Band stretcher resets its internal buffers to flush any residual audio from the previous track.
8. The user has Deck B also playing with key lock enabled at -4%. Both decks independently time-stretch their audio. Each deck's stretcher instance operates in isolation with no shared state or cross-talk.
9. The user observes the CPU load meter. With two decks running Rubber Band in real-time mode, CPU load increases by approximately 5-15% total (depending on hardware). The CPU monitor from PRD-0002 reflects this increased load.
10. The user adds Deck C and enables key lock. Three stretcher instances now run concurrently. On modern hardware (2020+), this remains well within the CPU budget at 44.1 kHz / 128-sample buffer size.

## 1.4. Acceptance Criteria

- [ ] A `keyLockEnabled` boolean property exists per deck in the state tree, defaulting to `false`.
- [ ] Key lock state is deck-level (persists across track loads and ejects, per PRD-0001 state classification).
- [ ] When `keyLockEnabled` is `false`, the audio pipeline uses the existing vinyl-mode playback path (PRD-0004 linear interpolation). The Rubber Band stretcher performs no processing and adds zero CPU overhead.
- [ ] When `keyLockEnabled` is `true` and `speedMultiplier` != 1.0, the Rubber Band stretcher processes audio to maintain original pitch while the transport advances at the modified speed.
- [ ] When `keyLockEnabled` is `true` and `speedMultiplier` == 1.0, the stretcher passes audio through with minimal overhead (Rubber Band's passthrough optimization).
- [ ] The Rubber Band stretcher is instantiated using `RubberBand::RubberBandStretcher` with the `RealTimeThreading` process flag and `PercussiveOptions` preset for optimal DJ audio quality (preserving transients in percussive material).
- [ ] Stretcher instances are created on the message thread during track load or key lock activation — never on the audio thread. A `std::atomic` pointer swap makes the stretcher available to `processBlock`.
- [ ] The stretcher's `setTimeRatio()` is called with `1.0 / speedMultiplier` on each `processBlock` cycle when the speed changes, using Rubber Band's real-time ratio update mechanism.
- [ ] The audio thread feeds source samples into the stretcher via `process()` and retrieves stretched samples via `retrieve()`, producing the correct number of output samples to fill the audio buffer.
- [ ] No memory allocations, mutex locks, or I/O occur on the audio thread during stretcher processing. Rubber Band's real-time mode is configured at construction to pre-allocate all internal buffers.
- [ ] Toggling key lock on during playback applies a 64-sample crossfade from vinyl-mode output to stretched output to prevent audible discontinuity.
- [ ] Toggling key lock off during playback applies a 64-sample crossfade from stretched output to vinyl-mode output.
- [ ] The stretcher introduces processing latency. This latency (queried via `RubberBandStretcher::getLatency()`) is published to the state tree so the transport can offset the reported playhead position for waveform synchronization.
- [ ] Latency compensation is applied to the UI playhead display only. The actual audio-thread playhead (used for buffer reads) is not modified.
- [ ] On track load, the stretcher's internal state is reset (via `reset()`) to flush residual audio from the previous track. The stretcher is reconfigured with the new track's channel count and sample rate.
- [ ] On track eject (deck returns to Empty), the stretcher is deallocated on the message thread. The atomic pointer is set to `nullptr`.
- [ ] At speed adjustments within +/-8%, stretched audio is free of audible artifacts under normal listening conditions.
- [ ] At speed adjustments between +/-8% and +/-16%, stretched audio may exhibit minor artifacts but remains usable for DJ mixing.
- [ ] At speed adjustments beyond +/-16%, artifacts are expected and no quality guarantee is made. The system does not clamp key lock to a specific range — the DJ retains full control.
- [ ] Up to 4 simultaneous Rubber Band stretcher instances (one per deck) operate within the CPU budget at 44.1 kHz / 128-sample buffer size on a 2020-era quad-core CPU (target: total stretcher CPU load under 30%).
- [ ] The key lock toggle renders as a button per deck, visually associated with the pitch fader area. The button has distinct active (illuminated) and inactive states.
- [ ] The key lock button is clickable in all deck states (Empty, Stopped, Paused, Playing). Enabling key lock on an Empty deck pre-arms it for the next loaded track.
- [ ] A per-deck key lock UI indicator is visible at a glance so the DJ always knows which decks have key lock active.
- [ ] The `keyLockEnabled` state is written to `std::atomic<bool>` and read by `processBlock` with zero locks or allocations.
- [ ] Pitch shift without speed change (master key / key adjust feature) is NOT implemented in this PRD. The stretcher only compensates for speed-induced pitch changes.
- [ ] Rubber Band Library is integrated via CMake as an external dependency (static linking preferred for distribution simplicity).
- [ ] All time-stretching code resides under `Source/Features/TimeStretch/`.

## 1.5. Grey Areas

### 1.5.1. Rubber Band Real-Time Mode vs Offline Mode

Rubber Band offers two modes: `RealTime` (processes audio in small blocks with varying latency) and `Offline` (requires the full audio file upfront, produces highest quality). Offline mode would deliver better quality but requires pre-processing the entire track at each speed, making it unusable for live pitch fader interaction.

**Resolution:** Use `RealTime` mode exclusively. DJ workflows demand instant response to pitch fader changes — even sub-second delay is unacceptable. Real-time mode introduces modest latency (typically 4096-8192 samples at 44.1 kHz, approximately 90-180 ms) but responds to ratio changes within a single `processBlock` cycle. The quality at +/-8% is indistinguishable from offline mode for most musical material. Offline pre-processing could be explored as a future enhancement (pre-compute stretched versions at common BPM targets) but is out of scope.

### 1.5.2. Rubber Band Latency Compensation

Rubber Band's real-time mode introduces processing latency that varies by configuration. If uncompensated, the waveform display and elapsed time will be ahead of the audible audio by the latency amount. However, compensating at the audio level (delaying the non-stretched signal path to match) adds unnecessary latency to vinyl mode.

**Resolution:** Compensate in the UI only. The transport publishes the raw audio-thread playhead position. The UI subtracts the stretcher's reported latency (queried once at construction and cached) from the display playhead when key lock is active. The audio signal chain remains unmodified — no artificial delay is added to the non-stretched path. This keeps vinyl-mode latency at its minimum while ensuring waveform-to-audio alignment when key lock is on.

### 1.5.3. Key Lock State Scope

Key lock could be track-specific (remembered per track in the database, restored on reload) or deck-level (persists on the deck regardless of which track is loaded). Pioneer CDJs treat key lock as deck-level. Traktor treats it as deck-level. Rekordbox treats it as track-level when exported from the library.

**Resolution:** Deck-level state. Key lock is a performance preference, not a track property. A DJ who enables key lock typically wants it on for the duration of a set, not toggling per track. This matches the classification system in PRD-0001 where deck-level state persists across track loads. A future "recommended key lock" flag per track (based on analysis) could be added as metadata but would not override the deck toggle.

### 1.5.4. Transition Behavior When Toggling Key Lock During Playback

When the DJ toggles key lock on or off mid-playback, the output switches between two different audio signals (stretched vs vinyl-mode). An abrupt switch causes an audible click because the two signals are at different phases and amplitudes.

**Resolution:** Apply a 64-sample crossfade between the two signal paths, consistent with the transport system's fade convention (PRD-0004). The outgoing signal fades out linearly over 64 samples while the incoming signal fades in. At 44.1 kHz, 64 samples is approximately 1.5 ms — imperceptible as a transition but sufficient to eliminate discontinuity. Both signal paths must run concurrently for the duration of the crossfade (one `processBlock` cycle at 128-sample buffer).

### 1.5.5. CPU Budget and Quality Preset Selection

Rubber Band offers multiple quality presets trading CPU for quality. The highest quality preset may be too CPU-intensive for 4 simultaneous decks on lower-end hardware, but the fastest preset may produce audible artifacts at moderate speed changes.

**Resolution:** Use Rubber Band's `PercussiveOptions` preset combined with `RealTimeThreading`. This preset prioritizes transient preservation (critical for drum-heavy DJ music) while maintaining reasonable CPU usage. Benchmarking during implementation will validate that 4 instances at 44.1 kHz / 128-sample buffer stay under 30% total CPU on a 2020 quad-core. If this budget is exceeded, a per-deck quality selector (High / Standard / Fast) can be added, but this is deferred unless benchmarking proves it necessary.

### 1.5.6. Stretcher Buffer Management in processBlock

Rubber Band's real-time mode does not guarantee a 1:1 input-to-output sample ratio per call. The stretcher may require more input samples than the buffer size to produce output, or may produce fewer output samples than requested. The audio thread must handle this without allocation.

**Resolution:** Use a pre-allocated lock-free ring buffer (sized at 2x the maximum expected latency) between the transport's sample reader and the stretcher output. The transport feeds source samples into the stretcher. The stretcher's output fills the ring buffer. `processBlock` reads from the ring buffer to fill the output. If the ring buffer underruns (stretcher has not produced enough output yet), the last valid samples are repeated with a fade — preferable to silence, which would be perceived as a dropout. The ring buffer is allocated once at stretcher construction on the message thread.

### 1.5.7. Pitch Shift Without Speed Change (Master Key)

Some DJ software allows shifting a track's key up or down in semitone increments without changing its speed (e.g., Traktor's "Key Adjust", Rekordbox's "Master Key"). This is the inverse of key lock: key lock preserves pitch when speed changes, while master key changes pitch while speed stays constant. Rubber Band supports this via `setPitchScale()`.

**Resolution:** Deferred to a future PRD. This PRD implements only the key-lock use case (pitch preservation during speed change). The `setPitchScale()` API will remain at its default (1.0). The architecture established here (per-deck stretcher instance with atomic parameter updates) naturally extends to support pitch shifting by adding a `pitchShiftSemitones` state property and mapping it to `setPitchScale()` in a future iteration.