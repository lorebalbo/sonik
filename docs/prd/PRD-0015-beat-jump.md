---
name: "PRD-0015: Beat Jump"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0004
  - PRD-0008
  - PRD-0013
---

# 1. PRD-0015: Beat Jump

## 1.1. Problem

When a DJ is playing a track and needs to skip ahead to the next chorus, jump back to replay a buildup, or reposition within a loop to a different phrase, the only current option is manual seeking via needle-drop or waveform click. Manual seeking is imprecise — the DJ must visually identify the target position, click, and hope the landing point is on-beat. During a live performance at 128 BPM, a single beat lasts 469 ms. A seek that lands 50 ms off-beat produces an audible phase shift against the other deck, breaking the mix. Professional DJs performing phrase-level navigation (jumping 8 or 16 beats to align with a phrase boundary) cannot afford to scrub through a waveform mid-set.

Pioneer CDJ-3000, Traktor Pro, and Serato DJ Pro all provide beat jump: a one-press operation that moves the playhead forward or backward by an exact number of beats on the beatgrid, guaranteeing the destination is beat-aligned and the rhythmic phase between decks is preserved. Without beat jump, Sonik lacks a fundamental navigation tool that every professional DJ platform ships. The beatgrid (PRD-0008), transport seek (PRD-0004), and quantize snap functions (PRD-0013) are already in place — the missing piece is a feature that combines them into a single, ergonomic, beat-accurate jump operation.

A second critical use case is loop repositioning. When a DJ has an active loop (PRD-0014) and wants to move that loop forward or backward by a set number of beats — for example, shifting a 4-beat loop from the breakdown into the drop — beat jump must shift the entire loop region (both in and out points) by the jump amount. Without loop shift, the DJ must deactivate the loop, seek, and re-engage a new loop manually, losing rhythmic continuity.

## 1.2. Objective

The system provides a per-deck beat jump engine that:
- Moves the playhead forward or backward by a user-selected number of beats (1/2, 1, 2, 4, 8, 16, 32) with a single button press, landing on a beat-grid-aligned position.
- Computes the jump offset in samples as `jumpBeats * beatInterval`, where `beatInterval = sampleRate * 60.0 / beatgridBpm`, ensuring the destination is always an exact multiple of beats from the origin.
- Executes the jump via the transport's existing seek mechanism (PRD-0004), reusing the 64-sample crossfade ramp for click-free playback transitions during playing state.
- When a loop is active (PRD-0014), shifts the entire loop region (both `loopInSamples` and `loopOutSamples`) by the jump offset, keeping the loop length unchanged and the playhead inside the shifted loop — implementing the "loop move" behavior present on Pioneer CDJ-3000 and Traktor Pro.
- When quantize mode is enabled (PRD-0013), snaps the jump destination to the nearest beat via `snapToNearestBeat`, correcting any accumulated off-grid drift in the playhead position.
- Clamps the destination to valid track bounds `[0, totalSamples - 1]`, preventing jumps past the start or end of the track.
- Degrades gracefully when no beatgrid exists, using a fallback tempo of 120 BPM (matching PRD-0014's fallback behavior) so beat jump remains functional on beatless material.
- Communicates all jump commands from the UI thread to the audio thread via the existing lock-free command queue, with zero allocations, zero locks, and zero I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Basic Beat Jump (Forward)

1. The user has a track loaded on Deck A with a valid beatgrid (BPM 126.0, beat interval = `44100 * 60.0 / 126.0 = 21000` samples at 44.1 kHz). The beat jump size selector is set to "4" (4 beats). Playback is active.
2. The playhead is at sample 420,000. The user presses the Beat Jump Forward button (right arrow).
3. The system computes the raw destination: `420000 + 4 * 21000 = 504000`. If quantize is enabled, the destination is passed through `snapToNearestBeat(504000, anchor, 21000.0)` to correct any sub-beat drift. The result lands exactly on a beat position.
4. The transport receives a seek command to sample 504,000. Because the deck is playing, a 64-sample crossfade is applied: fade-out at the old position, fade-in at the new position. Playback continues from 504,000 with no audible click or phase disruption.
5. The elapsed and remaining time displays update. The waveform scrolls to the new position. The beat phase relationship between decks is preserved because the jump distance is an exact multiple of the beat interval.

### 1.3.2. Basic Beat Jump (Backward)

6. The playhead is at sample 504,000. The user presses the Beat Jump Backward button (left arrow) with size "4".
7. The system computes: `504000 - 4 * 21000 = 420000`. The transport seeks to 420,000 with crossfade.
8. The user presses Beat Jump Backward again with the playhead near the start of the track (e.g., sample 30,000). The raw destination is `30000 - 84000 = -54000`. The system clamps to 0. The transport seeks to sample 0.

### 1.3.3. Changing Jump Size

9. The beat jump control strip displays the current jump size (default: 4 beats) alongside forward and backward arrow buttons.
10. The user clicks the jump size display or uses dedicated increment/decrement buttons to cycle through available sizes: 1/2, 1, 2, 4, 8, 16, 32 beats.
11. The selected size persists as a deck-level property (`beatJumpSize`) in the ValueTree — it survives track loads and application restarts (per PRD-0001 deck-level state classification).
12. The user selects "16" and presses forward. The playhead jumps exactly 16 beats (4 bars of 4/4 time), enabling phrase-level navigation.

### 1.3.4. Beat Jump While Paused or Stopped

13. The deck is paused at sample 210,000. The user presses Beat Jump Forward with size "2".
14. The playhead moves to `210000 + 2 * 21000 = 252000`. No crossfade is applied because the deck is not producing audio. The waveform and time displays update instantly. The deck remains in Paused state.
15. The deck is in Stopped state at sample 0. The user presses Beat Jump Forward with size "1". The playhead moves to sample 21,000. The deck remains Stopped.

### 1.3.5. Beat Jump During Active Loop (Loop Shift)

16. The user has a 4-beat loop active: loop-in at sample 420,000, loop-out at sample 504,000 (length = 84,000 samples). The playhead is at sample 450,000, cycling within the loop. Beat jump size is set to "4".
17. The user presses Beat Jump Forward. The system performs a loop shift: both loop-in and loop-out move forward by `4 * 21000 = 84000` samples. New loop-in = 504,000, new loop-out = 588,000. The playhead jumps by the same offset to `450000 + 84000 = 534000`, maintaining its relative position within the loop.
18. The waveform loop overlay slides forward to reflect the new loop boundaries. Playback continues seamlessly within the shifted loop, with a 64-sample crossfade at the jump discontinuity.
19. The user presses Beat Jump Backward. The loop shifts back by 84,000 samples: loop-in = 420,000, loop-out = 504,000, playhead = 450,000. The loop returns to its original position.
20. If a loop shift would push `loopInSamples` below 0, the loop-in clamps to the first beat at or after sample 0, and the loop-out adjusts to preserve the loop length. If `loopOutSamples` would exceed `totalSamples - 1`, the loop-out clamps and the loop-in adjusts to preserve loop length.
21. If the loop length exceeds the remaining track after clamping (not enough room to preserve the loop length), the entire shift is rejected — the loop and playhead remain unchanged.

### 1.3.6. Beat Jump with Quantize Interaction

22. Quantize is enabled. The playhead is at sample 450,500 — between beats at 441,000 and 462,000 due to a previous unquantized operation. The user presses Beat Jump Forward with size "1".
23. The raw destination is `450500 + 21000 = 471500`. The system calls `snapToNearestBeat(471500, anchor, 21000.0)`, which returns 462,000 (the nearest beat). The playhead lands exactly on a beat, correcting the accumulated off-grid drift.
24. Quantize is disabled. The same scenario: the raw destination 471,500 is used directly. The playhead lands between beats, preserving its exact offset from the grid. This matches the behavior users expect when quantize is deliberately turned off.

### 1.3.7. Beat Jump with No Beatgrid

25. The user loads an ambient track with no detectable beat (BPM confidence below threshold, `beatgridBpm = 0.0`). Beat jump remains functional using a fallback interval of `sampleRate * 60.0 / 120.0 = 22050` samples (120 BPM reference), matching PRD-0014's fallback behavior.
26. The user presses Beat Jump Forward with size "4". The playhead jumps `4 * 22050 = 88200` samples (~2 seconds). This provides usable navigation even on beatless material.
27. The beat jump forward/backward buttons remain active. A subtle visual indicator (e.g., the jump size display at reduced opacity) signals that the fallback tempo is in use.

### 1.3.8. Beat Jump on Empty Deck

28. The deck is in Empty state (no track loaded). All beat jump controls are visually disabled (grayed out). Pressing any beat jump button has no effect.

## 1.4. Acceptance Criteria

### 1.4.1. State and Data Model

- [ ] Each deck's `juce::ValueTree` contains a `beatJumpSize` property (`double`, representing the number of beats), defaulting to 4.0.
- [ ] Valid `beatJumpSize` values are: 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0.
- [ ] `beatJumpSize` is classified as deck-level state (per PRD-0001): it persists when loading a new track, ejecting, or reloading, and is included in session layout persistence.
- [ ] Each deck's beat jump size is fully independent — changing the size on Deck A does not affect Deck B.
- [ ] `beatJumpSize` is observable via the JUCE Listener pattern; UI components react to changes without polling.

### 1.4.2. Jump Destination Calculation

- [ ] The jump offset in samples is computed as `beatJumpSize * beatInterval`, where `beatInterval = sampleRate * 60.0 / beatgridBpm`.
- [ ] Forward jump destination: `currentPlayhead + jumpOffset`. Backward jump destination: `currentPlayhead - jumpOffset`.
- [ ] When quantize is enabled (PRD-0013) and a valid beatgrid exists, the raw destination is passed through `snapToNearestBeat(destination, anchor, beatInterval)` to align to the nearest beat.
- [ ] When quantize is disabled, the raw destination is used directly with no snapping.
- [ ] The destination is clamped to `[0, totalSamples - 1]`. A jump that would exceed either boundary clamps to the boundary.
- [ ] When no beatgrid exists (`beatgridBpm == 0.0`), the fallback interval `sampleRate * 60.0 / 120.0` is used in place of `beatInterval`.
- [ ] All arithmetic uses `double` precision for the beat interval and `int64_t` for the final sample position (cast via truncation after rounding), preventing drift in consecutive jumps.

### 1.4.3. Transport Integration

- [ ] Beat jump issues a seek command to the transport engine (PRD-0004) via the existing lock-free command queue.
- [ ] During Playing state, the seek applies the transport's 64-sample crossfade ramp (fade-out at old position, fade-in at new position), identical to a manual seek during playback (PRD-0004 §1.4).
- [ ] During Paused or Stopped state, the playhead moves instantly to the destination with no crossfade (matching PRD-0004 seek-while-paused behavior).
- [ ] The seek executes within a single `processBlock` cycle after the command is written.
- [ ] The deck's playback state (Playing, Paused, Stopped) is unchanged by a beat jump. Beat jump never triggers play, pause, or stop transitions.
- [ ] The published `std::atomic<int64_t>` playhead position updates to reflect the new position at the end of the `processBlock` cycle in which the jump executes.

### 1.4.4. Loop Shift (Beat Jump During Active Loop)

- [ ] When `loopActive` is `true` (PRD-0014), a beat jump shifts the entire loop region: `loopInSamples += jumpOffset`, `loopOutSamples += jumpOffset`. The loop length is preserved exactly.
- [ ] The playhead shifts by the same `jumpOffset`, maintaining its relative position within the loop: `newPlayhead = currentPlayhead + jumpOffset`.
- [ ] If the shifted `loopInSamples` would be less than 0, `loopInSamples` clamps to the first beat at or after sample 0 (computed via `getNextBeatAfter(-1, anchor, beatInterval)` or 0 if no beatgrid exists), and `loopOutSamples` adjusts to `loopInSamples + loopLength`.
- [ ] If the shifted `loopOutSamples` would exceed `totalSamples - 1`, `loopOutSamples` clamps to the last beat at or before `totalSamples - 1`, and `loopInSamples` adjusts to `loopOutSamples - loopLength`.
- [ ] If after clamping the loop length cannot be preserved (insufficient track remaining), the jump is rejected entirely — no state changes occur.
- [ ] The shifted `loopInSamples` and `loopOutSamples` are written via `std::atomic` stores. The audio thread picks up the new values on the next `processBlock` cycle.
- [ ] The waveform loop overlay updates immediately to reflect the shifted loop boundaries (via ValueTree listener propagation).
- [ ] When `loopActive` is `false`, beat jump moves only the playhead. Stored (inactive) loop boundaries are not affected.

### 1.4.5. Edge Cases

- [ ] Beat jump at sample 0 backward clamps to 0. The playhead does not move if already at 0 and the jump direction is backward.
- [ ] Beat jump near end of track forward clamps to `totalSamples - 1`. The playhead does not move if already at the last sample and the jump direction is forward.
- [ ] Rapid consecutive beat jumps (pressing the button multiple times within a few hundred milliseconds) queue correctly in the lock-free command queue. Each jump executes relative to the position after the previous jump.
- [ ] Beat jump during cue preview mode (PRD-0004 §1.3 step 10) moves the playhead but does not exit cue preview. CUE release still returns to the temp cue point (which is not modified by beat jump).
- [ ] Beat jump does not modify the temp cue point (PRD-0004). The temp cue remains at its previously set position.
- [ ] Beat jump does not modify saved hot cue positions (PRD-0012). Only the playhead (and active loop boundaries during loop shift) are affected.
- [ ] When `speedMultiplier != 1.0`, the jump offset calculation uses the beatgrid interval (based on `beatgridBpm`), not the effective BPM. The jump distance in beats is always exact regardless of pitch fader position.

### 1.4.6. UI Controls

- [ ] The beat jump control strip contains: a backward arrow button, a jump size display, and a forward arrow button, arranged horizontally.
- [ ] The jump size display shows the current `beatJumpSize` value formatted as: "1/2" for 0.5, "1" for 1.0, "2" for 2.0, "4" for 4.0, "8" for 8.0, "16" for 16.0, "32" for 32.0.
- [ ] Clicking the jump size display cycles forward through the available sizes (0.5 → 1.0 → 2.0 → 4.0 → 8.0 → 16.0 → 32.0 → 0.5). A secondary interaction (e.g., right-click or Shift+click) cycles backward.
- [ ] The forward arrow button triggers a forward beat jump. The backward arrow button triggers a backward beat jump.
- [ ] Pressing a jump button triggers a single jump. Holding the button does not auto-repeat (no key-repeat behavior). Each press is one jump.
- [ ] All beat jump controls are grayed out and non-interactive when the deck is in Empty state (no track loaded).
- [ ] When no beatgrid exists, the controls remain active but the jump size display renders at reduced opacity (e.g., 50%) to indicate fallback mode.
- [ ] The beat jump control strip mounts into the DeckShellComponent in the transport area, adjacent to the loop controls (PRD-0014).
- [ ] Forward and backward buttons provide visual feedback on press (brief accent-color flash, ~100 ms).
- [ ] Forward button tooltip: "Beat Jump Forward". Backward button tooltip: "Beat Jump Backward". Size display tooltip: "Beat Jump Size (click to change)".

### 1.4.7. Audio Thread Safety

- [ ] The beat jump command is delivered from the UI thread to the audio thread via the same lock-free command queue used by the transport (PRD-0004). No new queue is introduced.
- [ ] The jump destination is pre-computed on the UI thread. The audio thread receives only a seek command (target sample position) — no beat interval arithmetic occurs on the audio thread.
- [ ] For loop shift, the new `loopInSamples` and `loopOutSamples` are pre-computed on the UI thread and delivered via `std::atomic` writes.
- [ ] Zero memory allocations, zero mutex locks, and zero I/O occur on the audio thread for any beat jump operation.
- [ ] `beatJumpSize` is readable from any thread via the ValueTree atomic property bridge (PRD-0001).

### 1.4.8. Scope Boundaries

- [ ] Beat jump sizes are fixed to the 7 presets (0.5, 1, 2, 4, 8, 16, 32). Custom/arbitrary sizes are NOT supported in this PRD.
- [ ] Beat jump auto-repeat (holding the button to jump continuously) is NOT implemented in this PRD. Each button press triggers exactly one jump.
- [ ] Beat jump does not interact with slip mode in this PRD. Slip mode integration (maintaining the underlying timeline during jumps) is deferred to PRD-0017.
- [ ] MIDI mapping of beat jump buttons and size selector is NOT implemented in this PRD.
- [ ] Beat jump during cue preview does not modify the temp cue point — cue return behavior is unaffected.
- [ ] All beat jump code resides under `Source/Features/BeatJump/`. Dependencies (transport, beatgrid, quantize, loop state) are passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Jump Reference: Current Position vs. Grid-Aligned Position

Pioneer CDJ-3000 and Traktor Pro both compute the beat jump destination relative to the current playhead position, not relative to the nearest grid-aligned position. This means a playhead sitting between beats will land between beats after the jump (offset preserved). Quantize mode corrects this by snapping the destination to the nearest beat.

This PRD follows the same approach: the jump offset is added to (or subtracted from) the raw playhead position. When quantize is enabled, the destination is snapped via `snapToNearestBeat`. When quantize is off, the raw destination is used. This is deliberate — a DJ who has intentionally offset the playhead from the grid (e.g., for creative phasing) expects the offset to be preserved.

### 1.5.2. Jump Sizes: Beats vs. Bars

The EPIC specifies "1/2, 1, 2, 4, 8, 16, 32 bars." Industry-standard implementations (Pioneer CDJ-3000, Traktor Pro, Serato DJ Pro) all use **beats**, not bars. A "4-beat jump" is one bar of 4/4 time; a "4-bar jump" would be 16 beats — an unusually large default. The EPIC's list of sizes (1/2 through 32) exactly matches the beat-based sizes found on Pioneer CDJ-3000.

This PRD interprets the EPIC's sizes as **beats**, consistent with all major DJ platforms. The UI labels sizes as beat counts (e.g., "4" means 4 beats). If bar-based navigation is desired in the future, it can be added as a separate mode by multiplying the beat count by 4 (assuming 4/4 time signature). No time-signature-aware bar counting is implemented in this PRD.

### 1.5.3. Crossfade Strategy at Jump Point

During playback, a beat jump is functionally identical to a seek operation. This PRD reuses the transport's existing seek crossfade (PRD-0004): 64-sample fade-out at the old position, 64-sample fade-in at the new position. This is consistent with Pioneer CDJ-3000 behavior, where beat jumps produce a near-instantaneous transition with no audible gap or click.

An alternative approach used by some software is a slightly longer crossfade (128-256 samples) specifically for beat jumps to smooth the transition perceptually. This PRD does not implement an extended crossfade — the 64-sample ramp (~1.45 ms at 44.1 kHz) is short enough to be imperceptible while eliminating discontinuity artifacts. If user testing reveals that jumps sound harsh at certain positions, a beat-jump-specific crossfade length could be introduced as a future refinement.

### 1.5.4. Loop Shift Boundary Behavior

When a beat jump shifts a loop near the boundaries of a track, this PRD clamps the loop boundaries and attempts to preserve the loop length. If preservation is impossible (the remaining track is shorter than the loop), the jump is rejected entirely.

An alternative approach would be to allow partial loops or truncated loops at track boundaries. This PRD rejects that approach because a truncated loop changes the rhythmic pattern, which would surprise the DJ. Rejection (no-op) is the safest behavior — the DJ receives no audible change and understands the loop cannot be moved further.

### 1.5.5. Slip Mode Integration (Future PRD-0017)

When slip mode is active, beat jump should ideally move the audible playhead while the underlying "silent" timeline continues advancing. When slip exits, playback resumes at the position where the silent timeline has progressed to. This creates interesting scenarios:
- Beat jump forward 16 beats in slip mode, then exit slip: the DJ hears the jumped section, then seamlessly returns to where the track would have been without the jump.
- Loop shift in slip mode: the shifted loop plays while the underlying timeline advances, enabling creative re-entry.

This interaction is deferred to PRD-0017. In this PRD, beat jump modifies the actual playhead position. Slip mode awareness is not implemented.

### 1.5.6. Beat Jump During Cue Preview

When the DJ is in cue preview mode (holding CUE, per PRD-0004), a beat jump moves the playhead but does not cancel the preview. On CUE release, the playhead returns to the temp cue point — not to the jumped position. This matches Pioneer CDJ-3000 behavior where cue preview is a separate mode that beat jump does not interfere with.

If this causes confusion in user testing (the DJ expected to stay at the jumped position), a future revision could cancel cue preview on beat jump, transitioning to normal Playing state. This PRD preserves cue preview state for consistency with CDJ behavior.

### 1.5.7. Variable BPM / Non-Constant Beatgrid

PRD-0008 generates a fixed beatgrid (anchor + constant interval) even for variable-tempo tracks. Beat jump uses this fixed interval for offset computation, meaning jumps on a variable-tempo track may land slightly off from the actual beat positions. This is an acceptable limitation of the fixed-grid model. A future variable beatgrid implementation (individual beat markers) would require beat jump to look up the N-th beat from the current position rather than computing `N * interval`, which is a straightforward extension of the API.