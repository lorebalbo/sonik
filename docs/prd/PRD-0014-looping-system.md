---
name: "PRD-0014: Looping System"
status: Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0004
  - PRD-0006
  - PRD-0008
  - PRD-0013
---

# 1. PRD-0014: Looping System

## 1.1. Problem

Loops are one of the most heavily used performance tools in DJ mixing. A DJ extending a 4-bar breakdown to 16 bars while waiting for the incoming track to reach its drop, or isolating a 1-beat vocal chop for a rhythmic stutter effect, depends entirely on the ability to seamlessly repeat a defined section of audio. Without a looping system, Sonik forces the DJ to manually seek back to a section every time it ends — a workflow that is tempo-inaccurate, artifact-prone, and physically impossible at the speed required during live performance.

Professional DJ hardware and software universally provide two complementary loop modes. Auto-loop creates beat-quantized loops with a single button press: the DJ taps a beat-size button (e.g., 4 beats) and the system constructs a loop from the current playhead position spanning exactly that number of beats on the beatgrid. Manual loop lets the DJ mark arbitrary in and out points for sections that do not conform to power-of-two beat lengths. Both modes require loop halve and loop double controls that subdivide or extend the active loop in real time, enabling the DJ to evolve a loop progressively (e.g., 4 beats → 2 → 1 → 1/2 for a buildup effect, then back to 4 with a single press). Pioneer CDJ-3000, Traktor Pro, and Serato DJ Pro all implement these features as core loop functionality.

The transport system (PRD-0004) already provides sample-accurate playback, seeking, and crossfade ramps. The beatgrid (PRD-0008) provides beat positions at sample-level accuracy. The quantize system (PRD-0013) provides beat-snapping functions. The missing piece is a loop engine that uses these systems to define, activate, modify, and exit loops with zero audible artifacts, zero audio-thread violations, and professional-grade control over loop boundaries.

## 1.2. Objective

The system provides a per-deck looping engine that:
- Supports auto-loop with 7 beat-size presets (1/2, 1, 2, 4, 8, 16, 32 beats), creating a loop from the current playhead position spanning the selected number of beats on the deck's beatgrid, with a single button press.
- Supports manual loop via separate Loop In and Loop Out buttons that let the DJ mark arbitrary loop boundaries independent of the beatgrid.
- When quantize mode is enabled (PRD-0013), snaps both auto-loop and manual loop boundaries to the nearest beat position using the `snapToNearestBeat` API.
- Provides loop halve and loop double controls that subdivide or extend the active loop length by a factor of 2, enabling progressive loop manipulation between a minimum of 1/32 beat and a maximum of 64 beats.
- Applies a short crossfade (64 samples, ~1.45 ms at 44.1 kHz) at the loop-back point to eliminate clicks and discontinuities at the loop boundary, reusing the same fade-ramp length established by the transport (PRD-0004).
- Stores active loop state (in point, out point, active flag) in the deck's `juce::ValueTree`, observable by the waveform display for loop region overlay rendering.
- Provides loop exit (deactivate) that lets playback continue past the loop-out point from the current position, and re-loop (reactivate) that re-engages the most recently defined loop.
- Persists saved loops per track in SQLite (keyed by content hash), allowing the DJ to recall prepared loops across sessions, analogous to hot cue persistence (PRD-0012).
- Communicates all loop operations between the UI thread and audio thread using `std::atomic` and the existing lock-free command queue, with zero allocations, zero locks, and zero I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Auto-Loop

1. The user has a track loaded on Deck A with a valid beatgrid (BPM 128.0, beat interval = 20,672 samples at 44.1 kHz). The playhead is at sample 500,000 during playback.
2. The user presses the "4" auto-loop button. The system computes the loop-in point as the nearest beat boundary at or before the current playhead (using `getPreviousBeatBefore` from PRD-0013 if quantize is enabled, or the raw playhead position if quantize is off and no snapping is desired). The loop-out point is computed as `loopIn + 4 * beatInterval`. Both points are stored in the deck state tree and the loop activates immediately.
3. Playback continues through the loop region. When the playhead reaches the loop-out point, the transport crossfades back to the loop-in point (64-sample fade-out at the end, 64-sample fade-in at the start) and continues playing from the loop-in position. The loop repeats seamlessly.
4. The waveform detail view highlights the loop region with a translucent colored overlay between the loop-in and loop-out markers. The overview waveform also shows the loop region as a colored band.
5. If quantize is enabled, both loop-in and loop-out snap to exact beat positions allowing the loop to sit perfectly on the beatgrid. If quantize is off, the loop-in point is the exact playhead position and the loop-out is computed from the beatgrid interval without snapping loop-in.
6. Pressing a different auto-loop size button (e.g., "8") while a loop is active replaces the current loop. The loop-in point remains unchanged, and the loop-out point updates to `loopIn + 8 * beatInterval`. The transition is immediate within the current `processBlock` cycle.
7. If no beatgrid exists (BPM = 0.0), auto-loop falls back to time-based sizes using 120 BPM as a reference tempo (beat interval = `sampleRate * 60.0 / 120.0 = 22,050` samples). This provides usable loop sizes even for ambient or beatless material.

### 1.3.2. Manual Loop

8. The user presses the Loop In button. The current playhead position is stored as the pending loop-in point. A green marker appears on the waveform at this position. No loop is active yet — the system is waiting for the out point.
9. The user presses the Loop Out button. The current playhead position is stored as the loop-out point. The loop activates immediately. If quantize is enabled, both the stored loop-in and the loop-out positions are snapped to the nearest beat.
10. If the user presses Loop Out when the playhead is before (to the left of) the pending loop-in point, the loop-out point is set at the playhead position and the two points swap: the earlier position becomes loop-in and the later position becomes loop-out. The loop activates.
11. If the distance between loop-in and loop-out is fewer than 128 samples (~2.9 ms at 44.1 kHz), the system rejects the loop and does not activate. A visual flash on the Loop Out button indicates the rejection. 128 samples is the minimum viable loop length (2x the crossfade ramp length of 64 samples), preventing overlapping fade regions.
12. The user presses Loop In again while a manual loop is active. The loop-in point updates to the current playhead position (snapped if quantize is on). If the new loop-in is after the existing loop-out, the loop deactivates (invalid range). Otherwise, the loop region adjusts in real time. The waveform overlay updates immediately.
13. The user presses Loop Out again while a manual loop is active. The loop-out point updates to the current playhead position (snapped if quantize is on). If the new loop-out is before the loop-in, the loop deactivates. Otherwise, the loop adjusts.

### 1.3.3. Loop Halve and Double

14. A 4-beat loop is active (loop-in at sample 200,000; loop-out at sample 282,688; length = 82,688 samples = 4 beats). The user presses Loop Halve.
15. The loop-out point moves to `loopIn + (loopLength / 2)` = sample 241,344. The loop is now 2 beats. The loop-in point remains fixed. If the playhead is currently past the new loop-out, it wraps to `loopIn + ((playhead - loopIn) % newLoopLength)` on the next `processBlock` cycle.
16. The user presses Loop Halve again. The loop becomes 1 beat (20,672 samples). Again, the user presses Loop Halve twice more — loop becomes 1/2 beat, then 1/4 beat. Each halve anchors from the loop-in point.
17. The minimum loop length is 1/32 of a beat (or 128 samples, whichever is larger). Pressing Loop Halve at the minimum has no effect.
18. The user presses Loop Double. The loop-out point moves to `loopIn + (loopLength * 2)`. From 1/4 beat, the loop expands to 1/2 beat. The user can press Loop Double repeatedly up to a maximum of 64 beats (or the remaining track length, whichever is smaller).
19. Loop Double does not extend the loop-out point beyond `totalSamples - 1`. If the computed loop-out exceeds the track length, the loop-out is clamped to the end of the track.

### 1.3.4. Loop Exit and Re-Loop

20. A 4-beat loop is active. The playhead is cycling within the loop. The user presses the Loop Active button (or the same auto-loop size button that created the loop) to deactivate the loop.
21. Playback continues from the current playhead position past the loop-out point. The loop state transitions to inactive but the loop-in and loop-out positions are retained in the state tree as the "last loop."
22. The waveform overlay changes from a solid colored region to a dimmed outline, indicating that a loop is defined but not active.
23. After the playhead has moved well past the loop region, the user presses the Re-Loop button (or the Loop Active button again). The loop reactivates with the previously stored loop-in and loop-out points. The transport immediately seeks back to the loop-in point (with crossfade) and begins looping.
24. If the user presses Re-Loop and no previous loop exists (no loop-in/out points stored), nothing happens.

### 1.3.5. Auto-Loop and Manual Loop Interaction

25. The user creates a manual loop (in at sample 100,000, out at sample 200,000). While this loop is active, the user presses the "2" auto-loop button. The manual loop is replaced by a new auto-loop: loop-in is set to the nearest beat at or before the current playhead position, loop-out is loop-in + 2 beats. The previous manual loop boundaries are discarded from active state.
26. The user creates an auto-loop and then presses Loop In to adjust the start point manually. The loop transitions from auto-loop to manual mode — the auto-loop size indicator deselects, and the loop is now treated as a manually defined loop.

### 1.3.6. Loop with No Track or No Beatgrid

27. The user presses any loop button when the deck is in Empty state (no track loaded). Nothing happens. All loop controls are visually disabled (grayed out).
28. The user loads a track with no detectable beat (BPM = 0.0). Auto-loop uses the fallback 120 BPM interval for sizing. Manual loop in/out works normally based on the playhead position. Loop halve/double operates on the current loop length regardless of beatgrid availability.

### 1.3.7. Loop Persistence

29. The user creates a loop and saves it by pressing a "Save Loop" button (or the loop is auto-saved when the track is unloaded). The loop-in position, loop-out position, and an optional color are written to SQLite on a background thread, keyed by the track's content hash.
30. The user quits Sonik, relaunches, and loads the same track. The saved loop is restored from the database. The loop region appears on the waveform as a dimmed outline (inactive). The DJ can press Re-Loop to activate it.
31. Up to 8 saved loops per track are supported (matching the hot cue slot count). Loops are stored independently from hot cues in a separate `loops` table.
32. The user loads the same file from a different path. Because persistence is keyed by content hash, saved loops are found and restored.

### 1.3.8. Waveform Loop Overlay

33. When a loop is active, the waveform detail view renders a translucent overlay (deck accent color at 25% opacity) spanning from loop-in to loop-out. Vertical markers at both boundaries extend the full height of the waveform.
34. The loop-in marker is a solid vertical line with a right-pointing triangle at the top. The loop-out marker is a solid vertical line with a left-pointing triangle at the top. Both markers use the deck's accent color.
35. On the overview waveform, the active loop renders as a colored band (accent color at 20% opacity) spanning the loop region.
36. When a loop is inactive but stored (last loop), the overlay renders at 10% opacity with dashed boundary lines, indicating a re-activatable loop.
37. Loop markers update position in real time as the waveform scrolls, computed via `samplePositionToPixelX` from PRD-0006.
38. Saved loops (from SQLite) that are not the currently active loop render as thin colored bands on the overview waveform at 15% opacity.

## 1.4. Acceptance Criteria

### 1.4.1. Loop State and Data Model

- [ ] Each deck's `juce::ValueTree` contains a `Loop` child node with properties: `loopInSamples` (`int64_t`), `loopOutSamples` (`int64_t`), `loopActive` (`bool`), `loopMode` (`int`, 0 = none, 1 = auto, 2 = manual).
- [ ] `loopInSamples` and `loopOutSamples` default to -1 (no loop defined). `loopActive` defaults to `false`.
- [ ] Loop state is classified as track-specific state (per PRD-0001): it resets on track load and is not carried across tracks.
- [ ] The last active loop's in/out points persist in the state tree when the loop is deactivated, enabling re-loop.
- [ ] All loop state properties are observable via the JUCE Listener pattern. UI components react to changes without polling.

### 1.4.2. Auto-Loop

- [ ] Seven auto-loop size buttons are provided: 1/2, 1, 2, 4, 8, 16, 32 beats.
- [ ] Pressing an auto-loop button computes loop-in as the beat boundary at or before the current playhead position (via `getPreviousBeatBefore` when quantize is enabled, via beatgrid arithmetic otherwise).
- [ ] Loop-out is computed as `loopIn + beatCount * beatInterval`, where `beatInterval = sampleRate * 60.0 / beatgridBpm`.
- [ ] When quantize is enabled (PRD-0013), both loop-in and loop-out are snapped to exact beat positions.
- [ ] When quantize is disabled, loop-in is the exact playhead position and loop-out is computed by adding the beat-length offset. Loop-in is NOT snapped.
- [ ] Pressing a different auto-loop size while a loop is active changes the loop-out point (keeping loop-in fixed) and remains active.
- [ ] Pressing the currently active auto-loop size button deactivates the loop (toggle behavior).
- [ ] When no beatgrid exists (`beatgridBpm == 0.0`), auto-loop uses a fallback interval of `sampleRate * 60.0 / 120.0` (120 BPM reference).
- [ ] The active auto-loop size button is visually highlighted (filled with accent color).

### 1.4.3. Manual Loop

- [ ] A Loop In button and a Loop Out button are provided.
- [ ] Pressing Loop In stores the current playhead position as the pending loop-in point (snapped if quantize is enabled). A marker appears on the waveform. No loop activates until Loop Out is pressed.
- [ ] Pressing Loop Out stores the current playhead position as the loop-out point (snapped if quantize is enabled) and activates the loop.
- [ ] If Loop Out is pressed before Loop In (no pending in-point exists), nothing happens.
- [ ] If the Loop Out position is before the Loop In position, the two points swap so that the earlier position is always loop-in and the later is always loop-out.
- [ ] The minimum loop length is 128 samples (2x the crossfade ramp of 64 samples). Loops shorter than 128 samples are rejected with a visual indicator (button flash).
- [ ] Pressing Loop In while a loop is active updates the loop-in point to the current playhead position. If the new in-point is after the out-point, the loop deactivates.
- [ ] Pressing Loop Out while a loop is active updates the loop-out point to the current playhead position. If the new out-point is before the in-point, the loop deactivates.

### 1.4.4. Loop Halve and Double

- [ ] A Loop Halve button and a Loop Double button are provided, active only when a loop is defined (active or inactive).
- [ ] Loop Halve sets `loopOutSamples = loopInSamples + (currentLength / 2)`. The loop-in point is fixed; only the loop-out point moves.
- [ ] Loop Double sets `loopOutSamples = loopInSamples + (currentLength * 2)`. The loop-in point is fixed; only the loop-out point moves.
- [ ] Minimum loop length after halving: max(1/32 beat, 128 samples). Halving below this minimum is a no-op.
- [ ] Maximum loop length after doubling: min(64 beats, totalSamples - loopInSamples). Doubling beyond this maximum is a no-op.
- [ ] If the playhead is past the new loop-out after a halve, the playhead wraps to `loopIn + ((playhead - loopIn) % newLength)` on the next `processBlock` cycle.
- [ ] Halve and double on an inactive loop modify the stored boundaries without activating the loop.

### 1.4.5. Loop Playback Engine (Audio Thread)

- [ ] The loop engine executes inside `processBlock` after the transport reads samples and before output.
- [ ] On each `processBlock` cycle, if `loopActive` is `true`, the engine checks whether the playhead will cross `loopOutSamples` during this buffer.
- [ ] When the playhead crosses the loop-out point mid-buffer, the engine splits the buffer at the crossing point: samples before the crossing render normally, samples after the crossing render from the loop-in point onward.
- [ ] A 64-sample crossfade is applied at the loop-back point: the last 64 samples before the loop-out fade out linearly from 1.0 to 0.0, and the first 64 samples from the loop-in fade in linearly from 0.0 to 1.0. The two regions are summed (overlap-add) to eliminate clicks.
- [ ] The crossfade ramp length is 64 samples at all sample rates, matching the transport crossfade from PRD-0004.
- [ ] If the loop length is between 128 and 256 samples (very short loops), the crossfade ramp is reduced to `loopLength / 2` to prevent the fade-in and fade-out regions from overlapping.
- [ ] The loop engine reads `loopInSamples`, `loopOutSamples`, and `loopActive` via `std::atomic` loads. No mutex, no allocation, no I/O.
- [ ] Loop boundary updates from the UI thread are delivered via `std::atomic` writes. The audio thread picks up changes on the next `processBlock` cycle.
- [ ] The loop engine handles `speedMultiplier != 1.0` correctly: the playhead advances by `bufferSize * speedMultiplier` samples per cycle, and the loop-back calculation accounts for the fractional sub-sample accumulator from the transport (PRD-0004).

### 1.4.6. Loop Exit and Re-Loop

- [ ] Deactivating a loop sets `loopActive = false` but retains `loopInSamples` and `loopOutSamples` in the state tree.
- [ ] On deactivation, playback continues from the current playhead position. No seek occurs. The playhead proceeds past the loop-out point on the next `processBlock` cycle.
- [ ] Re-loop (reactivating) sets `loopActive = true` and issues a seek command to `loopInSamples` via the transport's lock-free command queue. A 64-sample crossfade applies at the seek point.
- [ ] If re-loop is pressed when no loop boundaries are stored (`loopInSamples == -1`), the command is ignored.
- [ ] Re-loop works regardless of the current playhead position (the DJ can re-loop after the playhead has moved far past the original loop region).

### 1.4.7. Waveform Overlay

- [ ] Active loop region renders as a filled rectangle on the detail waveform, spanning from `samplePositionToPixelX(loopInSamples)` to `samplePositionToPixelX(loopOutSamples)`, using the deck's accent color at 25% opacity.
- [ ] Loop-in boundary marker: solid vertical line (2px wide, full waveform height) with a right-pointing triangle (8px) at the top edge, filled with the deck's accent color.
- [ ] Loop-out boundary marker: solid vertical line (2px wide, full waveform height) with a left-pointing triangle (8px) at the top edge, filled with the deck's accent color.
- [ ] On the overview waveform, the active loop renders as a colored band (accent color at 20% opacity) with 1px vertical lines at boundaries.
- [ ] Inactive (stored) loop renders with the same markers at 10% opacity with dashed boundary lines on the detail view.
- [ ] Saved loops from the database that are not the current active/stored loop render as thin bands at 15% opacity on the overview waveform.
- [ ] Loop overlay rendering does not degrade waveform frame rate below 60 fps.
- [ ] Markers reposition correctly on zoom level changes, window resize, and waveform scrolling.

### 1.4.8. Persistence

- [ ] Saved loops are stored in a SQLite table `loops` with columns: `id` (INTEGER PRIMARY KEY), `track_hash` (TEXT NOT NULL), `slot_index` (INTEGER NOT NULL, 0-7), `loop_in_samples` (INTEGER NOT NULL), `loop_out_samples` (INTEGER NOT NULL), `color_index` (INTEGER NOT NULL DEFAULT 0), `label` (TEXT DEFAULT ''), `created_at` (TEXT NOT NULL).
- [ ] A UNIQUE constraint exists on `(track_hash, slot_index)`.
- [ ] Up to 8 saved loops per track are supported.
- [ ] On track load, the system queries `SELECT * FROM loops WHERE track_hash = ? ORDER BY slot_index` and populates saved loop data in the state tree.
- [ ] All database writes (save, update, delete) execute on a background thread, never blocking the UI or audio thread.
- [ ] Cached load of saved loops for a track completes in under 50 ms.
- [ ] Loops with `loop_in_samples` or `loop_out_samples` beyond the loaded track's `totalSamples` are flagged as invalid and hidden but not deleted from the database.
- [ ] Persistence is keyed by content hash (from PRD-0001), not file path.
- [ ] The currently active loop is auto-saved to the first available slot when the track is unloaded (eject or new track load), if it has not already been saved.

### 1.4.9. Audio Thread Safety

- [ ] `loopInSamples` and `loopOutSamples` are read by the audio thread via `std::atomic<int64_t>`.
- [ ] `loopActive` is read by the audio thread via `std::atomic<bool>`.
- [ ] Loop activation/deactivation commands that include a seek (re-loop) are delivered via the same lock-free command queue used by the transport (PRD-0004).
- [ ] Zero memory allocations, zero locks, and zero I/O occur on the audio thread for any loop operation.
- [ ] All loop state mutations (set in/out, activate, deactivate, halve, double) originate on the UI thread and propagate to the audio thread via atomics and the lock-free queue.

### 1.4.10. UI Controls

- [ ] The loop control strip renders: Loop In button, Loop Out button, Loop Active/Re-Loop toggle button, Loop Halve button, Loop Double button, and 7 auto-loop size buttons (1/2, 1, 2, 4, 8, 16, 32).
- [ ] The Loop Active button illuminates (accent color) when a loop is active. When inactive but a loop is stored, it displays at reduced opacity to indicate re-loop is available.
- [ ] Auto-loop size buttons display their beat value. The currently active auto-loop size is highlighted.
- [ ] All loop controls are grayed out and non-interactive when the deck is in Empty state.
- [ ] The loop control strip mounts into the DeckShellComponent in the transport area.

### 1.4.11. Scope Boundaries

- [ ] Loop roll (momentary loop that engages only while held, returning to the original timeline position on release) is NOT implemented in this PRD. Loop roll depends on slip mode (future PRD-0017).
- [ ] Loop cues (hot cue pads that trigger a loop from a stored position + length) are NOT implemented in this PRD. See PRD-0012 Grey Area §1.5.7.
- [ ] Move loop (shifting the entire loop region forward/backward by beat increments) is NOT implemented in this PRD.
- [ ] MIDI mapping of loop controls is NOT implemented in this PRD.
- [ ] All code resides under `Source/Features/Loop/`. Dependencies passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Auto-Loop Anchor Behavior: Snap to Grid vs Snap to Playhead

Pioneer CDJ-3000 auto-loop behavior: pressing an auto-loop size button creates a loop starting at the nearest beat boundary at or before the current playhead position. The loop-out point is always an exact number of beats after the loop-in. This means the loop always sits on the beatgrid and loops of the same beat size are phase-consistent regardless of when the button is pressed. Traktor follows the same convention. Serato snaps to the nearest beat in either direction (before or after).

Sonik follows the Pioneer/Traktor convention: auto-loop anchors at the beat boundary at or before the playhead (`getPreviousBeatBefore`). This ensures the DJ never hears a "jump forward" when pressing auto-loop — the loop captures audio the DJ has already heard or is currently hearing. If quantize is off, the loop-in is the raw playhead position and the loop length is still beat-accurate (computed from `beatInterval`), but the loop may not align to the grid.

### 1.5.2. Manual Loop In Before Loop Out

On Pioneer CDJ-3000, pressing Loop Out before Loop In does nothing — the system requires Loop In first. Traktor allows either order. Sonik follows a lenient model: if Loop Out is pressed without a pending Loop In, the action is ignored (no loop created). If both points are set and the out point is chronologically before the in point, the system auto-swaps them to form a valid range. This prevents confusion when the DJ reverses the expected order and avoids a "nothing happened" failure mode that might frustrate a new user while still requiring Loop In to be pressed first to initiate the workflow.

### 1.5.3. Loop Halve Anchor Point

When halving a loop, there are two possible strategies: halve from the loop-in point (keep loop-in fixed, move loop-out inward) or halve from the current playhead (center the halved region around where the DJ is hearing audio). Pioneer CDJ-3000 halves from the loop-in point — the start of the loop is always preserved and only the endpoint changes. This provides predictable, repeatable behavior: the DJ knows the downbeat of the loop remains constant.

Sonik follows the Pioneer convention. Loop halve always moves the loop-out point toward the loop-in point, preserving the loop's starting anchor. If the playhead is past the new (shorter) loop-out, the playhead wraps to the appropriate position within the new loop length on the next audio callback. The same logic applies to loop double: the loop-in is fixed and the loop-out extends outward.

### 1.5.4. Loop Exit Behavior

When the DJ deactivates a loop, the playhead continues from its current position — it does not jump to where it "would have been" if the loop had never been engaged. Pioneer CDJ-3000, Traktor, and Serato all follow this convention. The rationale is that the DJ's intent when exiting a loop is to let the music play forward from where it currently is, not to simulate a timeline that was never heard.

Slip mode (future PRD-0017) changes this: with slip active, a shadow playhead continues advancing through the track at normal speed while the audible playhead loops. On loop exit, playback resumes from the shadow playhead's position, as if the loop never happened. This PRD does not implement slip interaction — it is deferred to PRD-0017.

### 1.5.5. Re-Loop Behavior: Pioneer vs Traktor

On Pioneer CDJ-3000, the "Reloop/Exit" button is a single toggle: press once to activate a loop (or jump back to the last loop and activate), press again to exit. If the DJ has moved past the loop region, pressing Reloop jumps back to the loop-in point and reactivates the loop. Traktor's behavior is similar, but Traktor also allows the DJ to set a new loop without clearing the reloop target.

Sonik follows the Pioneer model: re-loop always jumps to the stored loop-in point and reactivates the loop, even if the playhead has moved far past the loop region. This provides a "safety net" — the DJ can always return to the last loop. The loop-in and loop-out points persist in the state tree until replaced by a new loop definition or until the track is unloaded.

### 1.5.6. Crossfade at Loop Boundary

A naive loop implementation that hard-splices the audio stream from the loop-out point back to the loop-in point produces an audible click due to the discontinuity in the waveform. Professional DJ software applies a micro-crossfade at the loop boundary. Pioneer CDJ-3000 uses a crossfade of approximately 1-2 ms. Traktor reportedly uses 5-10 ms depending on settings.

Sonik applies a 64-sample crossfade (~1.45 ms at 44.1 kHz), consistent with the transport crossfade length from PRD-0004. This is implemented as an overlap-add: the last 64 samples before the loop-out are faded out (linear ramp 1.0 → 0.0), the first 64 samples from the loop-in are faded in (linear ramp 0.0 → 1.0), and the two are summed. This eliminates the click without perceptibly smearing the transient at the loop boundary. For very short loops (128-256 samples), the crossfade ramp is shortened to half the loop length to prevent overlap of the fade-in and fade-out regions.

### 1.5.7. Loop Persistence and Saved Loops

Pioneer CDJ-3000 stores loops as part of the track's metadata (via Rekordbox), alongside hot cues and memory cues. Traktor stores "active loop" state per track. Sonik supports up to 8 saved loops per track, stored in a dedicated `loops` SQLite table keyed by content hash. This is intentionally separate from the `hot_cues` table — loops have two positions (in/out) vs hot cues' single position, and mixing them in one table would require nullable columns and type discriminators.

When a track is unloaded (eject or load new track), the currently active loop is auto-saved to the first available slot if it has not already been explicitly saved. On reload, saved loops appear as dimmed overlays on the waveform. The DJ can tap a saved loop to activate it. A future PRD may integrate loop cues into the hot cue pad system (PRD-0012 §1.5.7), where pressing a pad triggers both a seek and a loop activation.

### 1.5.8. Slip Mode Interaction (Future PRD-0017)

When slip mode is active, the system maintains a shadow playhead that advances in real time regardless of loops, scratching, or reverse playback. With a loop engaged under slip mode, the audible playhead loops within the loop region while the shadow playhead continues moving forward through the track. When the loop is deactivated, playback snaps to the shadow playhead position (with crossfade), creating the effect that the loop was "transparent" and the track continued underneath.

This interaction is NOT implemented in this PRD. The loop engine in this PRD always continues from the current audible playhead position on loop exit. PRD-0017 will extend the loop engine by adding shadow playhead awareness. The loop engine's interface (activate, deactivate, read boundaries via atomics) is designed to accommodate this future extension without API changes — PRD-0017 only needs to modify the deactivation behavior (seek to shadow position instead of continuing from current position).

### 1.5.9. Visual Representation of Active Loop

The active loop region on the waveform should be immediately obvious without being visually overwhelming. Pioneer CDJ-3000 uses a bright colored overlay with sharp boundary markers. Traktor uses a colored tint with boundary lines.

Sonik uses a translucent fill (25% opacity of the deck's accent color) between two solid vertical boundary markers (2px wide, full waveform height). The loop-in marker has a right-pointing triangle at the top (indicating the direction of playback into the loop), and the loop-out marker has a left-pointing triangle (indicating the boundary where playback turns back). This follows the Pioneer visual convention and is immediately recognizable to DJs migrating from CDJ hardware. The reduced opacity ensures that the waveform's frequency coloring and beat grid lines remain visible through the overlay.