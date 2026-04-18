---
name: "PRD-0013: Quantize Mode"
status: Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0008
---

# 1. PRD-0013: Quantize Mode

## 1.1. Problem

When a DJ sets a cue point, marks a loop boundary, or performs a beat jump, the action happens at whatever sample the playhead occupies at the instant the button is pressed. Human timing is imprecise — even a skilled DJ pressing a hot cue pad 20 ms early or late places the cue between beats rather than on one. A cue point that lands 15 ms before a downbeat causes an audible rhythmic stutter every time it is triggered. A loop-in point that falls between beats produces a loop that clicks or drifts against the track's groove. A beat jump that lands off-grid disrupts the phase relationship between decks, breaking a mix.

Professional DJ hardware solves this with quantize mode. Pioneer CDJ-3000, Traktor Pro, and Serato all provide a quantize function that snaps beat-aware operations to the nearest beat position on the track's beatgrid. Without this feature, Sonik forces the DJ to rely entirely on manual timing for every cue, loop, and jump — a standard that no professional DJ software ships without. The beatgrid analysis engine (PRD-0008) already computes beat positions with sample-level accuracy; the missing piece is a system that uses those positions to correct human timing errors automatically.

## 1.2. Objective

The system provides a per-deck quantize mode that:
- Offers a toggle (on/off) per deck, stored as a deck-level property in the `juce::ValueTree` state tree, persisting across track loads and application restarts.
- When enabled, snaps the sample position of cue placement, loop boundary setting, and beat jump destinations to the nearest beat position on the deck's beatgrid.
- Computes the snapped position in O(1) using the beatgrid's anchor-plus-interval representation from PRD-0008, with no allocation, no locking, and no I/O — safe for invocation on any thread including the audio thread.
- Exposes a public API (`snapToNearestBeat`, `getNextBeatAfter`, `getPreviousBeatBefore`) that consumer features (PRD-0012 Cue Points, PRD-0014 Looping, PRD-0015 Beat Jump) call to resolve snapped positions, keeping all beat-snapping logic in a single location.
- Degrades gracefully when no beatgrid exists: quantize remains toggled on but all snap functions return the input position unmodified, so operations execute at the exact playhead position as if quantize were off.
- Provides clear per-deck visual feedback (a "Q" indicator button) so the DJ always knows whether quantize is active on each deck.

## 1.3. User Flow

### 1.3.1. Toggling Quantize

1. The user has a track loaded on Deck A with a valid beatgrid. The quantize button ("Q") in the deck's transport controls area is currently off (dim).
2. The user clicks the "Q" button. The button illuminates (e.g., solid cyan or the deck's accent color), indicating quantize is active for Deck A. The `quantizeEnabled` property in the deck's ValueTree is set to `true`.
3. The user loads a different track onto Deck A. Quantize remains enabled — it is a deck-level property, not a track-level property (per PRD-0001).
4. The user clicks the "Q" button again. The button dims, quantize is disabled, and `quantizeEnabled` is set to `false`.
5. The user toggles quantize on Deck B independently of Deck A. Each deck's quantize state is fully independent.

### 1.3.2. Cue Placement with Quantize Enabled

6. Quantize is enabled on Deck A. The track has a beatgrid with anchor at sample 44,100 and BPM 120.0 (beat interval = 22,050 samples at 44.1 kHz).
7. The user pauses playback at sample 65,800 — between beats at 66,150 (beat index 1) and 44,100 (beat index 0). The nearest beat is 66,150.
8. The user presses hot cue pad A (unassigned). The system calls `snapToNearestBeat(65800)`, which returns 66,150. Hot cue A stores position 66,150. The waveform marker appears aligned with the beat grid line.
9. If quantize were off, the cue would store 65,800 — between beats, producing a slightly off-grid trigger point.

### 1.3.3. Temp Cue with Quantize Enabled

10. Quantize is enabled on Deck A. The user pauses at sample 87,900.
11. The user presses CUE (PRD-0004 temp cue set behavior: pressing CUE while paused at a non-cue position sets a new temp cue). The system snaps 87,900 to the nearest beat (88,200) and sets the temp cue at 88,200. The playhead jumps to 88,200.
12. Subsequent CUE return operations (pressing CUE during playback) return to 88,200 — a beat-aligned position.

### 1.3.4. Loop Boundaries with Quantize Enabled

13. Quantize is enabled. The user presses Loop In at sample 130,500. The system snaps to the nearest beat at 132,300 and sets the loop-in point there.
14. The user presses Loop Out at sample 220,700. The system snaps to the nearest beat at 220,500 and sets the loop-out point there. The active loop spans exactly from beat to beat.

### 1.3.5. Beat Jump with Quantize Enabled

15. Quantize is enabled. The playhead is at sample 88,200 (on a beat). The user presses "Beat Jump +4". The system calculates the destination as `88200 + 4 * 22050 = 176400` — exactly 4 beats ahead, on a beat.
16. If the playhead were between beats (e.g., at 89,000 due to a previous unquantized operation), beat jump with quantize snaps the destination to the nearest beat around the raw target position.

### 1.3.6. Quantize with No Beatgrid

17. The user loads an ambient track with no detectable beat (BPM confidence below threshold, per PRD-0008). The beatgrid does not exist (`beatgridBpm` = 0.0).
18. Quantize is enabled on this deck. The "Q" button remains illuminated (quantize is a deck-level preference), but a subtle visual indicator (e.g., the "Q" text at reduced opacity, or a small "no grid" badge) signals that snapping is inactive.
19. The user sets a hot cue. Since no beatgrid exists, `snapToNearestBeat` returns the input position unchanged. The cue stores the exact playhead position as if quantize were off.
20. The user loads a different track with a valid beatgrid. Quantize immediately becomes effective — cue placement now snaps to beats.

### 1.3.7. Quantize During Playback (Trigger Timing)

21. Quantize is enabled. The user presses hot cue pad A during playback. The hot cue triggers immediately — the seek to the stored position executes within a single `processBlock` cycle (per PRD-0012). Quantize does not delay hot cue trigger timing.
22. The stored cue position is already on a beat (snapped at set time), so the jump destination is beat-aligned even though the trigger is instant.

## 1.4. Acceptance Criteria

### 1.4.1. State and Toggle

- [ ] Each deck has a `quantizeEnabled` boolean property in its `juce::ValueTree` node, defaulting to `false`.
- [ ] `quantizeEnabled` is classified as deck-level state: it persists when loading a new track, ejecting, or reloading (per PRD-0001 state classification).
- [ ] `quantizeEnabled` is persisted in the session layout data, surviving application restarts (per PRD-0001 session persistence).
- [ ] Each deck's quantize state is fully independent — toggling on Deck A does not affect Deck B.
- [ ] Toggling quantize is a `juce::ValueTree` property change that propagates to all listeners within one message-loop cycle.

### 1.4.2. Snap API

- [ ] A `QuantizeService` class (or equivalent) provides the following public methods, accepting beatgrid parameters (anchor, interval) and a sample position:
  - `snapToNearestBeat(int64_t position, int64_t anchor, double beatInterval) -> int64_t` — returns the beat position closest to `position`.
  - `getNextBeatAfter(int64_t position, int64_t anchor, double beatInterval) -> int64_t` — returns the first beat position strictly after `position`.
  - `getPreviousBeatBefore(int64_t position, int64_t anchor, double beatInterval) -> int64_t` — returns the last beat position strictly before or at `position`.
- [ ] `snapToNearestBeat` computes: `beatIndex = round((position - anchor) / beatInterval)`, result = `anchor + (int64_t)(beatIndex * beatInterval)`. The result is clamped to `[0, totalSamples - 1]`.
- [ ] `getNextBeatAfter` computes: `beatIndex = floor((position - anchor) / beatInterval) + 1`, result = `anchor + (int64_t)(beatIndex * beatInterval)`. Clamped to `[0, totalSamples - 1]`.
- [ ] `getPreviousBeatBefore` computes: `beatIndex = ceil((position - anchor) / beatInterval) - 1`, result = `anchor + (int64_t)(beatIndex * beatInterval)`. Clamped to `[0, totalSamples]` (returns 0 if before anchor).
- [ ] All three methods execute in O(1) with no memory allocation, no locks, and no I/O — safe for use on the audio thread.
- [ ] All three methods are pure functions (static or free functions) with no side effects.
- [ ] If `beatInterval <= 0.0` (no valid beatgrid), all three methods return the input `position` unchanged.

### 1.4.3. Consumer Integration Points

- [ ] PRD-0012 (Cue Points): when `quantizeEnabled` is `true` and a valid beatgrid exists, hot cue set operations pass the playhead position through `snapToNearestBeat` before storing. The formula matches PRD-0012 §1.4.2: `nearestBeat = anchor + round((playhead - anchor) / beatInterval) * beatInterval`.
- [ ] PRD-0012 (Cue Points): hot cue trigger operations (seek to stored position) execute immediately regardless of quantize state. Quantize does not delay trigger timing.
- [ ] PRD-0004 (Transport): when `quantizeEnabled` is `true` and the user sets a temp cue (CUE press while paused at non-cue position), the temp cue position is snapped to the nearest beat via `snapToNearestBeat`.
- [ ] PRD-0004 (Transport): CUE return (pressing CUE during playback to return to cue point) executes immediately regardless of quantize state.
- [ ] PRD-0014 (Looping, future): loop-in and loop-out positions are snapped via `snapToNearestBeat` when quantize is enabled.
- [ ] PRD-0015 (Beat Jump, future): jump destination positions are snapped to beats when quantize is enabled. Beat jump inherently operates on beat multiples, so quantize primarily ensures the starting position is on-grid.

### 1.4.4. Graceful Degradation

- [ ] When `quantizeEnabled` is `true` but no beatgrid exists (`beatgridBpm == 0.0` or `beatInterval <= 0.0`), all snap functions return the input position unchanged — operations execute at the exact playhead position.
- [ ] The quantize toggle remains functional (can be turned on/off) regardless of beatgrid availability, so the setting is ready when a beatgrid-analyzed track is loaded.
- [ ] When a track without a beatgrid finishes background analysis and a beatgrid becomes available, quantize becomes effective immediately for subsequent operations. Existing cue points set before the grid was available are not retroactively snapped.

### 1.4.5. UI

- [ ] Each deck displays a "Q" toggle button in the transport controls area (near Play, Pause, CUE).
- [ ] When quantize is enabled, the "Q" button is filled with the deck's accent color (or a fixed color such as cyan `#00D2FF`).
- [ ] When quantize is disabled, the "Q" button is dim (outline only, low-opacity text).
- [ ] When quantize is enabled but no beatgrid exists, the "Q" button remains illuminated but displays at reduced opacity (e.g., 50%) or with a strikethrough indicator, communicating that snapping is not currently active.
- [ ] The "Q" button is non-interactive (grayed out) when the deck is in Empty state (no track loaded).
- [ ] Clicking the "Q" button toggles `quantizeEnabled` in the ValueTree. The UI updates via the Listener pattern — no direct state mutation from the button callback.
- [ ] The "Q" button provides a tooltip: "Quantize: snap cues, loops, and jumps to beat positions" when hovered.

### 1.4.6. Audio Thread Safety

- [ ] The snap functions perform only arithmetic operations (addition, division, rounding) on numeric inputs. No allocation, no locks, no I/O.
- [ ] `quantizeEnabled` is readable from the audio thread via `std::atomic<bool>` (or via the atomic ValueTree property bridge established in PRD-0001).
- [ ] Beatgrid parameters (`anchor`, `beatInterval`) are readable from the audio thread via `std::atomic<int64_t>` and `std::atomic<double>` (per PRD-0008).
- [ ] No quantize operation introduces latency or delays in `processBlock`. The audio thread never waits for a snap result.

### 1.4.7. Scope Boundaries

- [ ] Quantized hot cue triggering (delaying a seek until the next beat boundary of the current playback position) is NOT implemented in this PRD. All triggers are instant. This feature is documented in Grey Areas for future consideration.
- [ ] Sub-beat quantize granularity (half-beat, quarter-beat snap) is NOT implemented in this PRD. The system snaps to whole beats only. The API design supports a future `divisor` parameter without breaking changes.
- [ ] Quantize does not affect needle-drop / waveform click-to-seek operations. Seeks from waveform interaction are always exact.
- [ ] Quantize does not affect pitch fader or gain controls.
- [ ] MIDI mapping of the quantize toggle is NOT implemented in this PRD.
- [ ] All code resides under `Source/Features/Quantize/`. Dependencies passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Quantized Trigger Timing (Phase 2)

Pioneer CDJ-3000 quantize mode delays hot cue triggers so the seek executes when the current playhead crosses the next beat boundary, preserving rhythmic continuity. This PRD intentionally omits this behavior — all triggers are instant (per PRD-0012 §1.4.11). The rationale: instant triggers are simpler, more predictable, and the stored position is already on a beat (snapped at set time), so the destination is beat-aligned even without trigger-time quantization.

However, trigger-time quantization matters when the DJ triggers a cue at an arbitrary point between beats — the discontinuity in the audio stream occurs mid-beat, which can sound abrupt. A future iteration could implement this by:
- Storing a "pending seek" target on the audio thread when a quantized trigger is requested.
- On each `processBlock`, checking if the playhead has reached or passed the next beat boundary.
- Executing the seek with crossfade at that exact beat boundary.
- Cancelling the pending seek if another transport command arrives before the beat boundary.

This mechanism requires careful handling of edge cases (pending seek + stop, pending seek + another seek, very slow BPM where the wait exceeds 500 ms). It should be specified in a dedicated PRD or as a revision to this one.

### 1.5.2. Sub-Beat Quantize Granularity

Traktor Pro allows the user to configure quantize resolution (1 beat, 1/2, 1/4, 1/8). This PRD snaps to whole beats only. The snap API can be extended by dividing `beatInterval` by a divisor parameter (e.g., `beatInterval / 2` for half-beats). The UI would add a dropdown or cycle button next to the "Q" toggle. This has been deferred because whole-beat quantize satisfies the primary use case (aligning cues and loops to beats), and sub-beat granularity adds UI complexity that should be validated with user feedback first.

### 1.5.3. Per-Deck vs Global Quantize

Pioneer CDJ-3000 uses per-deck quantize (each CDJ has its own Q button). Traktor Pro uses a global quantize toggle. This PRD follows the Pioneer model (per-deck) because Sonik's architecture is per-deck and a DJ may want quantize on one deck (the prepared deck) but off on another (for free-form scratching or precise manual cueing). A global toggle shortcut (e.g., toggling quantize on all decks simultaneously) could be added as a convenience feature without changing the per-deck state model.

### 1.5.4. Retroactive Snap on Toggle

When the DJ enables quantize, should existing cue points that were set without quantize be retroactively snapped to the nearest beat? This PRD says no — existing cues retain their stored positions. Retroactive snapping would silently move saved cues, which could surprise the DJ. If the DJ wants to re-align a cue, they can delete and re-set it with quantize enabled. This matches Pioneer and Traktor behavior.

### 1.5.5. Quantize and Variable-Tempo Tracks

PRD-0008 flags tracks with variable tempo (inter-onset deviation > 3%) but still generates a fixed beatgrid from the average BPM. Quantize snaps to this fixed grid, which may not align with actual beat positions in tempo-varying sections. The DJ should be aware that quantize accuracy degrades on variable-tempo material. No special handling is defined — the fixed grid is the best available reference, and the DJ can disable quantize for these tracks.