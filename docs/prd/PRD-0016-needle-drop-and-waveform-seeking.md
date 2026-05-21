---
name: "PRD-0016: Needle Drop and Waveform Seeking"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0004
  - PRD-0006
  - PRD-0008
  - PRD-0013
  - PRD-0018
---

# 1. PRD-0016: Needle Drop and Waveform Seeking

## 1.1. Problem

A DJ navigating a track during preparation or live performance needs to jump to arbitrary positions quickly. The transport system (PRD-0004) provides programmatic seek, and hot cues (PRD-0012) provide stored positions, but neither solves the fundamental need to visually scan a waveform and click on a specific location to hear what is there. Without click-to-seek, the DJ is limited to scrubbing through a track linearly, triggering stored cues, or guessing positions — none of which support the rapid, exploratory navigation required when previewing an unfamiliar track, locating an unmarked breakdown, or aligning a mix point to a specific phrase.

Pioneer CDJ-3000 provides a touch strip along the top of the display that maps the full track length to a narrow horizontal bar — the DJ slides a finger to seek instantly. Traktor Pro and Serato DJ Pro allow clicking directly on the overview waveform to jump to any position, and clicking or dragging on the scrolling detail waveform to scrub through audio at fine resolution. These interactions are so fundamental that their absence makes a DJ application feel broken: the waveform becomes a passive display rather than an interactive navigation instrument.

Sonik already renders dual waveforms per deck (PRD-0006) — a full-track overview and a scrolling, zoomable detail view — and exposes a `samplePositionToPixelX` coordinate mapping for overlays. The missing piece is the inverse mapping (`pixelXToSamplePosition`) and the mouse interaction layer that translates clicks, drags, and modifier-key combinations into seek commands dispatched to the transport.

## 1.2. Objective

The system provides interactive click-to-seek and drag-to-scrub behavior on both waveform components per deck that:
- Allows the DJ to click anywhere on the overview waveform to seek the transport to the corresponding sample position instantly, using the same crossfade ramp as PRD-0004 to eliminate artifacts.
- Allows the DJ to click and drag on the overview waveform to scrub through the track in real time, with the playhead following the mouse position continuously.
- Allows the DJ to click on the detail waveform while holding a modifier key (Shift) to seek to the clicked position, preventing accidental seeks during normal waveform interaction (zoom, future loop drag).
- Allows the DJ to Shift+click-and-drag on the detail waveform to scrub through audio at fine resolution, with the scrub range determined by the current zoom level.
- Exposes a `pixelXToSamplePosition` inverse coordinate mapping on both waveform components, converting a local pixel X coordinate to an absolute sample position within the loaded track.
- Provides a *vinyl-style press-and-scratch* gesture on the detail waveform: an unmodified mouse-down acts as a “finger on the record” — the deck transport is paused on release at the cursor position, and any drag movement before release dispatches continuous seek commands that produce audible scratch artefacts through the existing transport crossfade ramps (PRD-0004). This unmodified gesture replaces the previous “unmodified click is reserved” placeholder.
- When quantize mode is enabled (PRD-0013), optionally snaps the seek destination to the nearest beat on the beatgrid (PRD-0008), controlled by a secondary modifier key (Shift+Alt/Option for quantized seek vs. Shift alone for exact seek on the detail waveform).
- Provides visual cursor feedback (crosshair cursor on seekable areas, time-position tooltip on hover) so the DJ always knows what position a click will target.
- Deactivates an active loop when the DJ seeks to a position outside the loop region, matching Pioneer CDJ-3000 behavior where a needle-drop escape exits the loop.
- Maintains audio-thread safety: all seek commands are dispatched through the existing lock-free command queue (PRD-0004) with zero allocations, zero locks, and zero I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Overview Waveform Click-to-Seek

1. The user has a track loaded on Deck A. The overview waveform displays the full track with a playhead marker and viewport rectangle. The deck is in Playing state.
2. The user moves the mouse over the overview waveform. The cursor changes from the default arrow to a crosshair. A lightweight tooltip appears near the cursor showing the time position corresponding to the current pixel (e.g., "2:34.12"), updated in real time as the mouse moves.
3. The user clicks at a point roughly 60% through the overview. The system converts the click's local X coordinate to a sample position via `pixelXToSamplePosition`. The resulting sample position is dispatched as a seek command to the transport (PRD-0004).
4. The transport executes the seek: if the deck is Playing, a 64-sample fade-out at the old position and a 64-sample fade-in at the new position produce a clean jump. If Paused or Stopped, the playhead moves instantly with no fade.
5. The detail waveform jumps to center on the new playhead position. The overview playhead marker and viewport rectangle update within the same frame.
6. The entire interaction — click, coordinate conversion, command dispatch, seek execution — completes within a single `processBlock` cycle after the click event.

### 1.3.2. Overview Waveform Drag-to-Scrub

7. The user clicks and holds on the overview waveform. The initial click triggers a seek to the clicked position (identical to step 3).
8. While holding the mouse button, the user drags horizontally across the overview. On each mouse-drag event, the system converts the current cursor X to a sample position and dispatches a new seek command. The playhead follows the mouse position continuously.
9. During the drag, audio output follows the playhead: each seek triggers the transport's standard crossfade ramp, producing a rapid series of short audio snippets that give the DJ an audible preview of the scrubbed region.
10. Seek commands during drag are throttled to one per `processBlock` cycle to prevent command queue overflow. Intermediate mouse positions between cycles are discarded; only the most recent position at each cycle is dispatched.
11. The user releases the mouse button. If the deck was Playing before the drag began, playback continues from the release position. If the deck was Paused or Stopped, it remains Paused at the release position.

### 1.3.3. Detail Waveform Press-to-Pause and Drag-to-Scratch (Unmodified)

12a. The user has Deck A playing. The user clicks (without holding any modifier) on the detail waveform and releases without moving. The deck transport is paused on release and the playhead is positioned at the clicked sample. This mirrors a DJ resting a finger on a spinning vinyl to halt playback. The temp cue point is not modified.
12b. The user clicks (without holding any modifier) and drags horizontally across the detail waveform without releasing. During the drag, continuous seek commands are dispatched at the same one-per-`processBlock` cadence as Shift+drag. The transport remains in the Playing state for the duration of the drag so that each seek’s 64-sample crossfade ramp (PRD-0004) is audible — producing the characteristic stuttering scratch-sound feedback. On mouse release, the deck transport is paused at the release sample. The deck does not resume playback on its own — the DJ must press Play to continue, matching the “press-to-stop” semantic.
12c. The press-and-drag gesture is mutually exclusive with the Shift+drag seek gesture. Holding Shift at press-time engages the seek behavior described below; no Shift at press-time engages the scratch/pause behavior described in 12a/12b. Releasing or pressing Shift mid-drag does not switch modes — the mode is decided at press-time and held for the lifetime of that drag.
12d. The scratch gesture is a no-op when no track is loaded (Empty state) or when waveform data is not yet available.
13. The user holds Shift. The cursor changes to a crosshair over the detail waveform. A time-position tooltip appears near the cursor, showing the sample position corresponding to the pixel under the mouse.
14. The user clicks while holding Shift. The system converts the click's local X coordinate to a sample position using the detail waveform's `pixelXToSamplePosition` (which accounts for the current zoom level and scroll offset). The seek command is dispatched to the transport.
15. The seek executes identically to the overview click-to-seek (step 4).

### 1.3.4. Detail Waveform Shift+Drag-to-Scrub

16. The user holds Shift, clicks on the detail waveform, and drags horizontally. The behavior mirrors overview drag-to-scrub (steps 7-11), but operates at the detail waveform's resolution — the same horizontal drag distance corresponds to a shorter time range, giving the DJ fine-grained control over scrub position.
17. If the user releases Shift during the drag (without releasing the mouse button), the scrub stops — no further seek commands are dispatched. The cursor reverts to the default arrow. When the user releases the mouse button, the playhead remains at the last scrubbed position.
18. If the drag moves the cursor beyond the left or right edge of the detail waveform component, seeking clamps to sample position 0 or `totalSamples - 1` respectively.

### 1.3.5. Quantized Seek (Shift+Alt/Option)

19. The user has quantize mode enabled on Deck A (PRD-0013). The track has a valid beatgrid (PRD-0008).
20. On the overview waveform, the user holds Alt (Option on macOS) and clicks. The clicked sample position is snapped to the nearest beat via `snapToNearestBeat` (PRD-0013 API) before the seek command is dispatched. The tooltip shows the snapped beat position rather than the exact pixel position.
21. On the detail waveform, the user holds Shift+Alt (Shift+Option on macOS) and clicks. The same beat-snap behavior applies — the seek destination rounds to the nearest beat.
22. If quantize mode is disabled or no beatgrid exists, Alt/Option has no effect — the seek targets the exact clicked position regardless.
23. Drag-to-scrub with Alt/Option held snaps each successive seek position to the nearest beat, producing a beat-by-beat scrub effect.

### 1.3.6. Interaction with Active Loops

24. A 4-beat loop is active on Deck A (PRD-0014). The user clicks on the overview waveform at a position outside the loop region.
25. The seek command is dispatched. The loop is deactivated before the seek executes — the transport clears `loopActive` in the deck state tree and then processes the seek. Playback continues from the new position without looping.
26. The waveform overlay transitions the loop region from the active highlight to the dimmed inactive state (loop-in and loop-out points are retained for re-loop, per PRD-0014).
27. If the user clicks at a position inside the active loop region, the seek executes but the loop remains active. The playhead jumps to the clicked position within the loop, and looping continues between the loop-in and loop-out boundaries.

### 1.3.7. Interaction with Cue Points

28. The user seeks via needle drop to a new position. The temp cue point (PRD-0004) is not modified — it retains its previously stored value. The DJ can still press CUE to return to the temp cue point.
29. Hot cue markers (PRD-0012) on the waveform are visual overlays only and do not interfere with click-to-seek. Clicking on a position that happens to coincide with a hot cue marker executes a normal seek, not a hot cue trigger.

### 1.3.8. Edge Cases

30. The user clicks on the overview waveform when no track is loaded (Empty state). Nothing happens — the click is ignored. The cursor does not change to crosshair.
31. The user clicks at the far-right edge of the overview waveform, mapping to a sample position at or past `totalSamples`. The position is clamped to `totalSamples - 1`.
32. The user clicks at the far-left edge (pixel 0). The position maps to sample 0.
33. The user double-clicks on the overview waveform. The system treats this as two sequential single-clicks. No special double-click behavior is defined.
34. The user scrolls the mouse wheel over the overview waveform. No zoom effect occurs — the overview always shows the full track. Scroll events are ignored on the overview.
35. The user scrolls the mouse wheel over the detail waveform (without Shift held). Zoom changes per PRD-0006 (one zoom level per tick). No seek occurs.
36. During a drag-to-scrub on the overview, the cursor leaves the component bounds vertically (above or below). Scrubbing continues — only the X coordinate matters for position mapping. The Y coordinate is ignored.
37. The user resizes the window while hovering over a waveform. The coordinate mapping recalculates on the next mouse event using the component's new dimensions. No stale position values are used.

## 1.4. Acceptance Criteria

### 1.4.1. Coordinate Mapping

- [ ] Both the overview and detail waveform components expose a `pixelXToSamplePosition(float pixelX) -> int64_t` method that converts a local X coordinate to an absolute sample position in the loaded track.
- [ ] The overview mapping is linear: `samplePosition = (int64_t)((pixelX / componentWidth) * totalSamples)`, clamped to `[0, totalSamples - 1]`.
- [ ] The detail mapping accounts for the current zoom level and scroll offset: `samplePosition = scrollOffsetSamples + (int64_t)((pixelX / componentWidth) * visibleRangeSamples)`, clamped to `[0, totalSamples - 1]`.
- [ ] `pixelXToSamplePosition` is the exact inverse of `samplePositionToPixelX` from PRD-0006 within +/- 1 pixel rounding.
- [ ] Both methods execute in O(1) with no allocation.

### 1.4.2. Overview Waveform Interaction

- [ ] Clicking anywhere on the overview waveform dispatches a seek command to the transport at the sample position returned by `pixelXToSamplePosition`.
- [ ] Click-and-drag on the overview waveform dispatches continuous seek commands (one per `processBlock` cycle) as the mouse moves, producing a drag-to-scrub effect.
- [ ] Seek commands are dispatched through the existing lock-free command queue (PRD-0004) with zero allocations on the UI thread.
- [ ] The seek executes within a single `processBlock` cycle after the command is written, using PRD-0004 crossfade behavior (64-sample ramp during playback, instant during paused/stopped).
- [ ] The cursor changes to crosshair (`juce::MouseCursor::CrosshairCursor`) when the mouse enters the overview waveform component, and reverts on exit.
- [ ] A tooltip displays the time position (format `M:SS.ms`, e.g., "2:34.12") corresponding to the pixel under the cursor, updated on every mouse-move event.
- [ ] Mouse scroll wheel events on the overview waveform are ignored (no zoom on overview).
- [ ] Clicking when no track is loaded (Empty state) produces no seek command and no cursor change.

### 1.4.3. Detail Waveform Interaction

- [ ] Clicking on the detail waveform without a modifier key dispatches a single seek to the click position and pauses the deck transport on mouse release. If the user does not drag between press and release, this acts as a “press-to-stop” control — a Playing deck becomes Paused at the clicked sample.
- [ ] Click-and-drag on the detail waveform without a modifier key dispatches continuous seek commands (one per `processBlock` cycle) for the duration of the drag, producing audible scratch artefacts via the transport’s standard crossfade ramps. The deck transport is forced to the Playing state at press-time (if it was Paused or Stopped) for the duration of the drag and is set back to Paused on mouse release.
- [ ] The press-and-drag scratch gesture uses the same lock-free seek command path as Shift+drag — no new audio-thread state is required.
- [ ] The press-and-drag scratch gesture is mutually exclusive with the Shift+drag seek gesture. Mode is latched at mouse-down based on modifier state and is not re-evaluated during the drag.
- [ ] The press-and-drag scratch gesture is a no-op when no track is loaded or waveform data is unavailable. Releasing the mouse in that case does not pause an empty deck.
- [ ] Holding Shift changes the cursor to crosshair over the detail waveform. Releasing Shift reverts the cursor.
- [ ] Shift+click on the detail waveform dispatches a seek command at the position returned by `pixelXToSamplePosition`.
- [ ] Shift+click-and-drag dispatches continuous seek commands (one per `processBlock` cycle), producing fine-resolution scrubbing that matches the current zoom level.
- [ ] If Shift is released during an active drag, scrubbing stops immediately — no further seek commands are dispatched until Shift is re-pressed or the mouse is released.
- [ ] Dragging beyond the left or right edge of the detail waveform clamps the seek position to sample 0 or `totalSamples - 1`.
- [ ] Mouse scroll wheel events on the detail waveform (without Shift) continue to control zoom per PRD-0006.
- [ ] The time-position tooltip appears during Shift hover on the detail waveform, displaying the position at higher precision than the overview due to the zoomed-in scale.

### 1.4.4. Quantized Seek

- [ ] On the overview waveform, holding Alt (Option on macOS) while clicking snaps the seek destination to the nearest beat via `snapToNearestBeat` (PRD-0013) before dispatching.
- [ ] On the detail waveform, holding Shift+Alt (Shift+Option on macOS) while clicking snaps the seek destination to the nearest beat.
- [ ] Quantized seek only engages when the deck's `quantizeEnabled` property is `true` AND a valid beatgrid exists (`beatInterval > 0.0`). Otherwise, Alt/Option has no effect.
- [ ] During quantized drag-to-scrub, each successive seek position is snapped to the nearest beat, producing beat-by-beat navigation.
- [ ] The tooltip reflects the snapped position when Alt/Option is held.

### 1.4.5. Scrub Throttling

- [ ] During drag-to-scrub (overview or detail), seek commands are issued at most once per `processBlock` cycle.
- [ ] If multiple mouse-move events occur between two `processBlock` cycles, only the most recent cursor position is used for the next seek command.
- [ ] The throttling mechanism uses a single `std::atomic<int64_t>` storing the pending seek target. The UI thread writes the latest position; the audio thread reads and clears it once per cycle.
- [ ] No intermediate seek positions are queued or buffered beyond the single atomic value.

### 1.4.6. Loop Interaction

- [ ] Seeking to a position outside the active loop region deactivates the loop. The `loopActive` property is set to `false` in the deck state tree before the seek command is dispatched.
- [ ] The loop-in and loop-out positions are retained in the state tree (not cleared), enabling re-loop (PRD-0014).
- [ ] Seeking to a position inside the active loop region does not deactivate the loop. The playhead jumps to the clicked position and looping continues.
- [ ] "Inside" is defined as `seekPosition >= loopInSample && seekPosition < loopOutSample`.

### 1.4.7. Transport State Preservation

- [ ] Seeking does not change the deck's transport state. A Playing deck remains Playing after the seek. A Paused deck remains Paused. A Stopped deck remains Stopped.
- [ ] Exception: drag-to-scrub on a Paused or Stopped deck does not start playback. The playhead moves to each dragged position silently (no audio output during scrub while paused/stopped).
- [ ] After a Shift+drag scrub completes (mouse released), the deck returns to its pre-drag transport state at the release position.
- [ ] Exception: the unmodified press/drag scratch gesture on the detail waveform deliberately forces the deck into the Playing state at press-time and into the Paused state on release, overriding the preservation rule. This is the explicit semantic of the “press-to-stop” vinyl gesture.
- [ ] The temp cue point (PRD-0004) is not modified by any needle-drop, scrub, or scratch operation.

### 1.4.8. Cursor and Visual Feedback

- [ ] Overview waveform cursor: crosshair when a track is loaded, default arrow when empty.
- [ ] Detail waveform cursor: crosshair when Shift is held and a track is loaded, default arrow otherwise.
- [ ] Time-position tooltip renders as a small opaque box (dark background, light text, 11px font) positioned 16 pixels above the cursor, horizontally centered on the cursor.
- [ ] Tooltip format: `M:SS.ms` (minutes, seconds with leading zero, two-digit centiseconds). Example: "0:00.00", "2:34.56", "12:03.99".
- [ ] Tooltip clamps to the component bounds — it does not overflow beyond the waveform edges.
- [ ] Tooltip disappears when the cursor exits the waveform component.
- [ ] No right-click context menu is implemented in this PRD. Right-click is reserved for future features (e.g., "Set Cue Here"). Right-clicking does not trigger a seek.

### 1.4.9. Audio Thread Safety

- [ ] All seek commands from needle-drop and scrub operations are delivered via the same lock-free mechanism used by PRD-0004 transport commands.
- [ ] No new command types are introduced — needle-drop reuses the existing Seek command.
- [ ] The scrub throttle `std::atomic<int64_t>` is the only new shared state between the UI and audio threads.
- [ ] Zero memory allocations, zero mutex locks, and zero I/O occur on the audio thread as a result of needle-drop or scrub operations.
- [ ] The audio thread does not read mouse coordinates, pixel values, or any UI state directly. All UI-to-audio communication is mediated by the command queue and atomic values.

### 1.4.10. Scope Boundaries

- [ ] Right-click context menu (e.g., "Set Cue Here", "Set Loop In Here") is NOT implemented. Right-click is a no-op.
- [ ] Touch/trackpad gesture seeking (two-finger swipe, tap-to-seek on touch screens) is NOT implemented. Standard mouse events only.
- [ ] Vinyl-style scrub audio (pitch-shifted audio during scrub proportional to scrub speed) is NOT implemented. Scrub produces normal crossfade-based audio during playback, and silence during paused/stopped scrub.
- [ ] Overview waveform zoom is NOT implemented. The overview always displays the full track.
- [ ] Keyboard-based seeking (arrow keys to nudge position) is NOT implemented in this PRD.
- [ ] MIDI mapping of needle-drop (e.g., mapping a MIDI fader to track position) is NOT implemented.
- [ ] All code resides under `Source/Features/Waveform/` (extending the existing waveform components from PRD-0006). Dependencies passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Scrub Audio Output During Playback

When the DJ drags to scrub while the deck is Playing, the transport executes rapid successive seeks with crossfade ramps. At typical scrub speeds this produces a stuttering, choppy audio effect. This is acceptable and expected — it gives the DJ audible feedback of the scrubbed region, matching Traktor Pro's behavior. An alternative approach is to mute audio during scrub and only produce output on mouse release, but this removes the auditory feedback that makes scrubbing useful. A third option is to implement vinyl-style scrub audio (pitch and speed vary proportionally to scrub velocity, like a turntable), but this requires a dedicated DSP mode in the transport system and is deferred to PRD-0018 (Jog Wheel).

The current design uses standard crossfade-based seeks during playback scrub and silence during paused/stopped scrub. This is the simplest approach that provides useful feedback without introducing new audio-thread complexity.

The unmodified press/drag scratch gesture on the detail waveform deliberately keeps the transport in the Playing state during the drag specifically to surface the crossfade-based scratch audio described above. The full velocity-proportional scratch DSP path remains PRD-0018’s scope; this PRD only adds the waveform mouse surface that drives it.

### 1.5.2. Modifier Key Choice for Detail Waveform

Requiring Shift to seek on the detail waveform prevents accidental seeks during normal interaction (zoom, future drag operations). However, professional DJs accustomed to Traktor's "click anywhere to seek" behavior may find the modifier cumbersome. An alternative is to provide a per-user preference: "Detail waveform click behavior: Seek / No action". This has been deferred to avoid premature preferences UI. The Shift modifier is the safer default — accidental seeks during a live set are more disruptive than an extra key press during preparation.

If user feedback strongly favors direct click-to-seek on the detail waveform, the modifier requirement can be removed by changing a single conditional in the mouse-down handler without architectural changes.

### 1.5.3. Seeking and Loop Deactivation

This PRD specifies that seeking outside an active loop deactivates it, matching Pioneer CDJ-3000 behavior. Traktor Pro takes a different approach: seeking outside a loop deactivates it, but seeking to a position before the loop-in point while the loop is active causes the playhead to re-enter the loop when it reaches the loop-in boundary. The CDJ model is simpler and more predictable — a needle-drop is an explicit "go here" instruction, and loop deactivation on escape matches the DJ's intent to leave the looped section.

An edge case arises when the DJ seeks to a position exactly equal to `loopOutSample`. Under the definition `seekPosition >= loopInSample && seekPosition < loopOutSample`, the loop-out position is treated as outside the loop. This is correct because the loop-out sample is the first sample after the loop region, and seeking there should not re-engage looping.

### 1.5.4. Tooltip Precision and Beats Display

The time-position tooltip shows `M:SS.ms` format. An alternative is to show beat position (e.g., "Bar 32, Beat 3") when a beatgrid exists, giving the DJ structural context rather than raw time. This is deferred because:
- Beat position display requires the beatgrid to be analyzed, adding a conditional code path.
- Time position is universally useful regardless of beatgrid availability.
- Beat position can be added as a second line in the tooltip in a future iteration without changing the coordinate mapping or interaction model.

### 1.5.5. Touch Screen and Trackpad Considerations

macOS trackpad gestures (two-finger scroll, pinch-to-zoom) already map to standard mouse events: scroll wheel for zoom (PRD-0006), and click events are unchanged. However, multi-touch gestures like two-finger tap-to-seek or swipe-to-scrub are not supported without dedicated `NSTouch` event handling outside of JUCE's standard mouse API.

For touch screens (e.g., Windows touch-enabled displays), JUCE maps touch events to mouse events by default. A single-finger touch on the overview waveform would trigger a seek, matching the expected CDJ-style touch-strip behavior. However, distinguishing between a touch-to-seek and a touch-to-scroll gesture requires additional heuristics (e.g., short tap vs. long press vs. swipe). This is deferred to a dedicated touch/gesture PRD.

### 1.5.6. Right-Click Context Menu

Professional DJ software commonly offers right-click context menus on waveforms for actions like "Set Cue Point Here", "Set Loop In", or "Add Marker". This PRD intentionally defers right-click behavior to keep the interaction model simple for the initial implementation. The coordinate mapping infrastructure (`pixelXToSamplePosition`) enables a future context menu trivially — the menu handler would read the cursor position at right-click time and pass the corresponding sample position to the cue or loop system. This should be specified in a revision of this PRD or as an addendum to PRD-0012 (Cue Points).

### 1.5.7. Drag-to-Scrub vs. Waveform Panning

Some DJ software (e.g., Serato) uses click-and-drag on the detail waveform to pan/scroll the visible region rather than to seek. This creates a conflict: the same gesture (drag) could mean "move the view" or "move the playhead". This PRD resolves the conflict by requiring Shift for all detail waveform seeking (including drag). Unmodified drag on the detail waveform is reserved for future use (waveform panning, loop region drag, beatgrid adjustment). If waveform panning is implemented in a future PRD, it should use unmodified click-and-drag, maintaining the Shift convention for seeking.