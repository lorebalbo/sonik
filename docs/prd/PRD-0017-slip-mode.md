---
name: "PRD-0017: Slip Mode"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0004
  - PRD-0014
  - PRD-0015
---

# 1. PRD-0017: Slip Mode

## 1.1. Problem

During a live performance, DJs routinely manipulate the audible playback position — looping a 4-bar section to extend a breakdown, triggering a hot cue to stutter a vocal, playing in reverse for a dramatic buildup, or beat-jumping to a different phrase. Every one of these actions displaces the playhead from the track's natural timeline. When the DJ finishes the manipulation and wants to return to "normal" playback, the track has moved on in real time — the audience has been dancing to the other deck's rhythm, and the manipulated track is now at the wrong position relative to where it would have been if left untouched. Without slip mode, the DJ must manually seek to the correct position, guessing where the track should be, and hope the landing is close enough that the audience does not notice the phase break. At 128 BPM, a half-beat error (234 ms) is immediately audible as a trainwreck.

Pioneer CDJ-3000, Traktor Pro, and Serato DJ Pro all solve this with slip mode: a shadow playhead that continues advancing silently at the normal playback rate while the DJ manipulates the audible position. When the manipulation ends, playback snaps back to the shadow position seamlessly, as if the DJ never intervened. This lets a DJ loop a section for 16 bars, exit the loop, and have the track resume exactly where it would have been — perfectly in phase with the other deck. Without slip mode, Sonik limits the DJ to only those manipulations where they can manually recover the timeline, eliminating an entire class of professional performance techniques: slip-loops, slip-reverse buildups, slip-triggered hot cue stutters, and slip-beat jumps for phrase navigation.

Slip mode is the highest-complexity feature in the transport layer because it introduces a second concurrent playhead that must advance in lockstep with the transport's speed state, survive arbitrary displacement actions, and produce an artifact-free snap-back transition — all within the real-time audio thread constraints (zero allocations, zero locks, zero I/O).

## 1.2. Objective

The system provides a per-deck slip mode engine that:
- Maintains a shadow playhead that advances at the same `speedMultiplier` as the primary (audible) playhead, continuing to track the deck's natural timeline even while the audible playhead is displaced by loops, hot cue triggers, reverse playback, or beat jumps.
- Automatically enters a "displaced" state when the audible playhead diverges from the shadow (e.g., loop wrap-around, hot cue jump, reverse direction change, beat jump), and automatically snaps back to the shadow position when the displacement action ends (e.g., loop exit, reverse deactivation).
- Applies a 64-sample crossfade on every snap-back transition (fade-out at the displaced position, fade-in at the shadow position) to eliminate clicks and discontinuities, reusing the same ramp length established by the transport (PRD-0004).
- Publishes the shadow playhead position to the UI via `std::atomic<double>` so the waveform can render a ghost playhead marker, giving the DJ continuous visual feedback of where snap-back will land.
- Stores `slipEnabled` as a deck-level boolean in the `juce::ValueTree` state tree (per PRD-0001 classification), persisting across track loads and application restarts.
- Communicates all slip state between the UI thread and audio thread via `std::atomic` — zero allocations, zero locks, and zero I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Enabling and Disabling Slip Mode

1. The user has a track loaded on Deck A with playback active. The Slip button is visible in the transport control area, currently inactive (dimmed).
2. The user presses the Slip button. The button illuminates with the deck's accent color. The state tree's `slipEnabled` property for Deck A updates to `true`. No audible change occurs — the shadow playhead initializes to the current primary playhead position and tracks it identically until a displacement action occurs.
3. Normal playback continues. The shadow playhead advances in lockstep with the primary playhead at the current `speedMultiplier`. No visual difference appears on the waveform because both playheads are at the same position.
4. The user presses the Slip button again while no displacement is active. Slip mode disables. The shadow playhead is discarded. No audible change occurs.
5. The user presses the Slip button while a displacement is active (the audible playhead is at a different position than the shadow). Slip mode disables. The audible playhead stays at its current position — no snap-back occurs. The shadow playhead is discarded. The displacement state clears. Playback continues from the current audible position as if slip had never been enabled.
6. The user loads a new track onto Deck A. Per PRD-0001, `slipEnabled` persists (deck-level state). The shadow playhead resets to the new track's initial playhead position (sample 0). Any active displacement state from the previous track clears.

### 1.3.2. Slip Loop (Loop with Slip Enabled)

7. Slip mode is enabled on Deck A. The user activates a 4-beat auto-loop (PRD-0014) at sample 400,000. The loop region spans from sample 400,000 (loop-in) to sample 482,688 (loop-out), a length of 82,688 samples at 128 BPM.
8. The audible playhead cycles within the loop: when it reaches loop-out, it wraps back to loop-in (per PRD-0014 crossfade behavior). The shadow playhead, however, does not loop. It continues advancing past the loop-out point at the current `speedMultiplier`, tracking where the track would be without the loop.
9. On the waveform, the DJ sees two playhead indicators: the primary playhead cycling within the loop region, and a ghost (shadow) playhead marker continuing to advance past the loop, rendered at 40% opacity in the deck's accent color.
10. The shadow playhead is now at sample 650,000 — well past the loop. The user presses the Loop Active button to deactivate the loop (PRD-0014 loop exit). The system detects that slip mode is enabled and a displacement exists (shadow != primary). Instead of continuing playback from the current audible position (default PRD-0014 behavior), the system snaps back: a 64-sample crossfade fades out from the current audible position and fades in at the shadow position (sample 650,000). Playback continues from the shadow position as if the loop never happened.
11. After snap-back, the shadow and primary playheads are reunited. The ghost marker disappears from the waveform. Normal synchronized playback resumes.

### 1.3.3. Slip Hot Cue

12. Slip mode is enabled. The user triggers hot cue C (PRD-0012), which is stored at sample 200,000. The audible playhead is currently at sample 800,000.
13. The transport seeks to sample 200,000 (hot cue trigger). Since slip is enabled, the system records the displacement: the shadow playhead continues advancing from sample 800,000 at the current `speedMultiplier`, while the audible playhead plays from sample 200,000.
14. The DJ plays from the hot cue position for a few seconds. The shadow advances to sample 900,000.
15. The hot cue trigger ends. In Sonik's hot cue model (PRD-0012), hot cue pads trigger and continue playing — there is no "release to return" behavior by default. The snap-back occurs when the DJ explicitly exits the displacement. The DJ taps the Slip button (short press) to trigger a slip return, snapping back to the shadow position with a 64-sample crossfade.
16. To provide explicit snap-back for hot cue displacement, the system offers a "Slip Return" action: the user short-presses the Slip button while displaced to trigger an immediate snap-back without disabling slip mode. Alternatively, the DJ can trigger another hot cue or action to re-displace, and the shadow continues from the original timeline.

### 1.3.4. Slip Reverse

17. Slip mode is enabled. Playback is active in the forward direction. The audible playhead is at sample 600,000. The user activates reverse playback (future feature; the `speedMultiplier` becomes negative or a reverse flag is set).
18. The audible playhead begins moving backward from sample 600,000. The shadow playhead continues advancing forward from sample 600,000 at the positive `speedMultiplier`. The DJ hears the track in reverse while the shadow tracks the forward timeline.
19. The user deactivates reverse playback. The system detects a slip displacement (shadow is ahead of the audible position). A 64-sample crossfade snaps playback from the current reverse position back to the shadow position. The DJ hears the track resume in the forward direction at exactly the position it would have reached — a classic "reverse buildup into drop" effect.

### 1.3.5. Slip Beat Jump

20. Slip mode is enabled. The audible playhead is at sample 500,000. The user performs a beat jump forward by 16 beats (PRD-0015), moving the audible playhead to sample 836,000.
21. Since slip is enabled, the shadow playhead continues advancing from sample 500,000. The audible playhead plays from sample 836,000. The DJ hears the jumped section while the shadow tracks the original timeline.
22. The DJ triggers a slip return (via the Slip button short press). The audible playhead snaps back to the shadow position with a 64-sample crossfade. The jumped section is discarded from the audible timeline.

### 1.3.6. Slip and Needle Drop

23. Slip mode is enabled. The audible playhead is at sample 400,000 with a displacement active (shadow at sample 600,000). The user clicks on the overview waveform at a position corresponding to sample 100,000 (needle drop, PRD-0016).
24. A needle drop is an explicit navigation action, not a performance manipulation. Both the audible playhead and the shadow playhead reset to sample 100,000. The displacement state clears. Playback continues from sample 100,000 with both playheads synchronized.
25. This matches Pioneer CDJ-3000 behavior, where touching the strip/platter to seek explicitly re-anchors the timeline rather than creating a slip displacement.

### 1.3.7. Shadow Playhead at End of Track

26. Slip mode is enabled. A loop is active near the end of a track (`totalSamples` = 10,000,000). The loop cycles between samples 9,500,000 and 9,582,688 while the shadow playhead advances past 9,582,688.
27. The shadow playhead reaches `totalSamples - 1` (sample 9,999,999) and clamps. It does not advance further. A visual indicator on the waveform shows the shadow at the end-of-track position.
28. The user exits the loop. The system detects that the shadow is clamped at end-of-track. The snap-back targets `totalSamples - 1`. The transport fades out (64-sample ramp) and the deck transitions to Stopped at the end of the track, matching end-of-track behavior from PRD-0004.
29. If the shadow reaches end-of-track and the DJ has not exited the loop, the loop continues playing indefinitely. The DJ maintains full control. A subtle visual pulse on the shadow marker indicates it is clamped at the end.

### 1.3.8. Multiple Consecutive Displacements

30. Slip mode is enabled. The user triggers hot cue A, creating a displacement. The shadow continues from the pre-jump position.
31. While displaced by the hot cue, the user activates a loop. The loop cycles the audible playhead. The shadow continues advancing from the original timeline — it does not reset to the new loop position. The shadow represents the unbroken forward timeline from the moment of the first displacement.
32. The user exits the loop. Because the underlying displacement (hot cue) is still active and the shadow has been continuously advancing, the snap-back targets the shadow's current position — not the loop exit position or the original hot cue position.
33. Design principle: the shadow always represents "where the track would be if none of the slip-displaced actions had occurred." Multiple displacements do not create a stack. There is one shadow, and it advances monotonically (or according to `speedMultiplier`).

### 1.3.9. Slip Mode on Empty or Stopped Deck

34. The deck is in Empty state (no track loaded). The Slip button is clickable and toggles `slipEnabled` in the state tree (pre-arming for the next loaded track), but no shadow playhead exists. No visual ghost marker appears.
35. The deck is in Stopped state with a track loaded. Slip mode is enabled. The shadow playhead sits at the same position as the primary playhead (sample 0). No displacement occurs because the transport is not advancing.

## 1.4. Acceptance Criteria

### 1.4.1. State and Data Model

- [ ] Each deck's `juce::ValueTree` contains a `Slip` child node with properties: `slipEnabled` (`bool`), `slipDisplaced` (`bool`).
- [ ] `slipEnabled` defaults to `false` and is classified as deck-level state (per PRD-0001): it persists across track loads, ejects, and application restarts as part of session layout.
- [ ] `slipDisplaced` defaults to `false` and is classified as track-specific state: it resets on track load.
- [ ] The shadow playhead position is stored as `std::atomic<double>` in the audio engine's per-deck transport data, not in the ValueTree (it changes every `processBlock` cycle and must be lock-free).
- [ ] A second `std::atomic<double>` publishes the shadow playhead position for UI readout, updated at the end of every `processBlock` cycle (parallel to the primary playhead atomic from PRD-0004).
- [ ] All slip state properties in the ValueTree are observable via the JUCE Listener pattern. UI components react to changes without polling.

### 1.4.2. Shadow Playhead Advancement

- [ ] When `slipEnabled` is `true` and the deck is in Playing state, the shadow playhead advances by `bufferSize * speedMultiplier` samples per `processBlock` cycle, using the same sub-sample `double` accumulator approach as the primary playhead (PRD-0004).
- [ ] The shadow playhead reads `speedMultiplier` from the same `std::atomic<float>` as the primary playhead. Both advance at the same rate when synchronized.
- [ ] When no displacement is active (`slipDisplaced == false`), the shadow playhead is forced equal to the primary playhead at the end of every `processBlock` cycle. It does not drift independently.
- [ ] When a displacement is active (`slipDisplaced == true`), the shadow playhead advances independently of the primary playhead. The primary playhead may loop, reverse, or jump while the shadow continues linearly.
- [ ] The shadow playhead clamps to `[0, totalSamples - 1]`. It never advances past the end of the track or below sample 0.
- [ ] When the shadow playhead reaches `totalSamples - 1`, it stops advancing and holds at that position until displacement ends.
- [ ] When key lock is enabled (PRD-0011) and the time stretcher introduces latency, the shadow playhead advances using the same compensated rate as the primary playhead. The shadow does not drift relative to the primary over time.
- [ ] When `speedMultiplier` changes (pitch fader adjustment) during a displacement, both the primary and shadow playheads use the new speed immediately. The shadow does not freeze at the old speed.

### 1.4.3. Displacement Triggers

- [ ] Loop wrap-around: When `loopActive` is `true` (PRD-0014) and the primary playhead wraps from the loop-out point back to the loop-in point, the system sets `slipDisplaced = true` if slip is enabled and the shadow was previously synchronized. The shadow continues advancing past the loop-out point.
- [ ] Hot cue trigger: When a hot cue pad is triggered (PRD-0012) and slip is enabled, the system sets `slipDisplaced = true`. The shadow continues from the pre-jump primary playhead position.
- [ ] Beat jump: When a beat jump (PRD-0015) executes and slip is enabled, the system sets `slipDisplaced = true`. The shadow continues from the pre-jump position. Loop shift behavior (PRD-0015 loop shift) is overridden — when slip is enabled, a beat jump displaces the audible playhead but does not shift the active loop boundaries, because the shadow maintains the original timeline.
- [ ] Reverse activation: When reverse playback is activated (future feature) and slip is enabled, the system sets `slipDisplaced = true`. The shadow continues in the forward direction.
- [ ] Scratch/vinyl manipulation (future feature): When the user manipulates the virtual platter/jog wheel, the audible playhead follows the platter. The shadow continues at the current `speedMultiplier`.

### 1.4.4. Snap-Back Triggers

- [ ] Loop exit: When the user deactivates an active loop (PRD-0014 loop exit) while `slipDisplaced` is `true`, the primary playhead snaps to the shadow position. A 64-sample crossfade is applied (fade-out at the audible position, fade-in at the shadow position). `slipDisplaced` resets to `false`.
- [ ] Reverse deactivation: When reverse playback is deactivated while `slipDisplaced` is `true`, the primary playhead snaps to the shadow position with a 64-sample crossfade.
- [ ] Explicit slip return: Tapping the Slip button while `slipDisplaced` is `true` triggers an immediate snap-back to the shadow position with a 64-sample crossfade. `slipDisplaced` resets to `false`. Slip mode remains enabled.
- [ ] After any snap-back, the shadow and primary playheads are resynchronized. The ghost marker disappears from the waveform.

### 1.4.5. Actions That Reset Both Playheads (No Displacement)

- [ ] Needle drop / waveform seeking (PRD-0016): When the user performs a needle drop or waveform click-to-seek, both the primary and shadow playheads move to the seek target. Any active displacement clears. `slipDisplaced` resets to `false`.
- [ ] CUE button (PRD-0004): CUE return (jumping to the temp cue point during playback) resets both playheads to the temp cue position. No displacement is created. CUE preview does not create a displacement.
- [ ] Stop command (PRD-0004): Stop resets both playheads to sample 0. Any displacement clears.
- [ ] Track load / eject: Both playheads reset. Displacement clears. `slipEnabled` persists.

### 1.4.6. Disabling Slip While Displaced

- [ ] When slip mode is disabled (`slipEnabled` toggled to `false`) while `slipDisplaced` is `true`, the audible playhead remains at its current position. No snap-back occurs. The shadow playhead is discarded. `slipDisplaced` resets to `false`.
- [ ] The DJ retains full control over whether to return to the original timeline. Disabling slip is an explicit "I want to stay here" decision.

### 1.4.7. Snap-Back Audio Transition

- [ ] Every snap-back applies a linear crossfade of 64 samples at the audio thread level: the last 64 samples from the displaced position fade out linearly from 1.0 to 0.0, and the first 64 samples from the shadow position fade in linearly from 0.0 to 1.0. The two regions are summed (overlap-add).
- [ ] The crossfade ramp length is 64 samples at all sample rates, matching the transport crossfade from PRD-0004.
- [ ] If the shadow position is within 64 samples of the audible position at snap-back time, the snap-back is a no-op (positions are close enough that no audible transition occurs). `slipDisplaced` resets to `false` and the playheads resynchronize.
- [ ] The snap-back seek is dispatched as a transport seek command via the existing lock-free command queue (PRD-0004). No new command delivery mechanism is introduced.

### 1.4.8. Slip and Loop Interaction Details

- [ ] When slip is enabled and a loop is active, the shadow playhead advances past the loop region while the audible playhead cycles within it. The shadow does not loop.
- [ ] When the user activates a loop while already displaced (e.g., after a hot cue jump), the loop operates on the audible playhead. The shadow continues from the original displacement point — it does not reset to the loop-in position.
- [ ] Loop halve and loop double (PRD-0014) operate on the audible playhead's loop boundaries. The shadow is unaffected.
- [ ] Re-loop (PRD-0014) while displaced seeks the audible playhead back to the stored loop-in and re-engages the loop. The shadow continues advancing. If the audible playhead was already in a displaced state, it remains displaced.
- [ ] When the user deactivates the loop to trigger a slip snap-back, the loop state transitions to inactive (PRD-0014 loop exit) and the playhead snaps to the shadow. These two operations (loop exit + snap-back) happen atomically within the same `processBlock` cycle.

### 1.4.9. Slip and Beat Jump Interaction

- [ ] When slip is enabled and no displacement is active, a beat jump (PRD-0015) displaces the audible playhead. The shadow continues from the pre-jump position.
- [ ] When slip is enabled and a displacement is already active, a beat jump moves the audible playhead further. The shadow is unaffected — it continues from the original displacement point.
- [ ] Beat jump loop shift (PRD-0015) is suppressed when slip is enabled. A beat jump inside an active loop moves the audible playhead (and the loop boundaries shift with it for audible continuity), but the shadow playhead is unaffected. When the loop is exited, snap-back returns to the shadow position, discarding the shifted loop position.
- [ ] After snap-back from a beat-jump displacement, the beat jump has no lasting effect on the track timeline.

### 1.4.10. Waveform Visual Feedback

- [ ] When `slipEnabled` is `true` and `slipDisplaced` is `true`, the waveform renders a ghost playhead marker at the shadow position in addition to the primary playhead.
- [ ] The ghost marker is a vertical line spanning the full height of the detail waveform, drawn in the deck's accent color at 40% opacity, with a small upward-pointing triangle at the top.
- [ ] On the overview waveform, the ghost marker renders as a thin vertical line (1px) at 40% opacity in the deck's accent color.
- [ ] The ghost marker position updates at the UI refresh rate (~60 Hz), derived from the `std::atomic<double>` shadow playhead position.
- [ ] When `slipDisplaced` is `false`, no ghost marker is rendered (the shadow is synchronized with the primary).
- [ ] The ghost marker repositions correctly on waveform zoom changes, window resize, and waveform scrolling, using `samplePositionToPixelX` from PRD-0006.
- [ ] Ghost marker rendering does not degrade waveform frame rate below 60 fps.

### 1.4.11. UI Controls

- [ ] A Slip toggle button is provided per deck in the transport control area, adjacent to the quantize button (PRD-0013).
- [ ] The Slip button has two visual states: inactive (dimmed, default) and active (illuminated in deck accent color).
- [ ] While slip is active and a displacement exists, the Slip button pulses or displays a secondary visual indicator (e.g., a subtle breathing animation or a contrasting outline) to signal that a snap-back is pending.
- [ ] Tapping the Slip button while displaced triggers a snap-back without disabling slip (short press, under 300 ms). Holding the Slip button for more than 300 ms and releasing toggles the `slipEnabled` state (long press).
- [ ] The Slip button is clickable in all deck states (Empty, Stopped, Paused, Playing). Enabling slip on an Empty or Stopped deck pre-arms the mode.
- [ ] Button tooltip: "Slip Mode (timeline continues during loops, jumps, and reverse)".
- [ ] All slip-related controls are grayed out and non-interactive only when the deck is in Empty state with no track loaded.

### 1.4.12. Audio Thread Safety

- [ ] `slipEnabled` is read by the audio thread via `std::atomic<bool>`.
- [ ] `slipDisplaced` is read and written by the audio thread via `std::atomic<bool>`. The audio thread sets this flag when it detects a displacement condition (loop wrap, hot cue seek, reverse activation, beat jump seek).
- [ ] The shadow playhead is a `double` value maintained entirely on the audio thread. It is published to the UI via a `std::atomic<double>` store at the end of each `processBlock` cycle.
- [ ] Snap-back is dispatched as a seek command via the existing lock-free command queue (PRD-0004). No new queue or concurrency primitive is introduced.
- [ ] Zero memory allocations, zero mutex locks, and zero I/O occur on the audio thread for any slip mode operation.
- [ ] All shadow playhead arithmetic (advancement, clamping, comparison with primary) uses `double` precision to prevent drift, matching the transport's sub-sample accumulator from PRD-0004.

### 1.4.13. Scope Boundaries

- [ ] Scratch/vinyl manipulation (jog wheel) integration is NOT implemented in this PRD. The displacement mechanism is designed to support it, but the jog wheel input itself is deferred to a future PRD.
- [ ] Reverse playback activation is NOT implemented in this PRD. The slip-reverse interaction is documented for when reverse playback is introduced.
- [ ] MIDI mapping of the Slip button and slip return is NOT implemented in this PRD.
- [ ] Slip mode does not interact with stem separation in this PRD.
- [ ] All slip mode code resides under `Source/Features/SlipMode/`. Dependencies (transport, loop state, hot cue state, beat jump) are passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Shadow Playhead Speed: Original BPM vs. Current speedMultiplier

The shadow playhead must advance at the same `speedMultiplier` as the primary playhead, not at the track's original BPM. Pioneer CDJ-3000 follows this convention: if the DJ has the pitch fader at +6%, the shadow advances at 1.06x. This is correct because the shadow represents "where the track would be if the DJ had not performed the displacement action" — and the DJ has also set the pitch fader, which is a persistent speed change, not a displacement.

If the shadow advanced at the original BPM instead, a snap-back after 30 seconds of looping at +6% speed would land 1.8 seconds (30 * 0.06) away from the expected position — a massive phase error against the other deck. The shadow must use the current `speedMultiplier` at all times, and if the DJ changes `speedMultiplier` during a displacement, both playheads respond immediately to the new speed.

When key lock is enabled (PRD-0011), the shadow still advances at `speedMultiplier` — key lock affects pitch, not the rate of playhead advancement.

### 1.5.2. Snap-Back Crossfade Duration

Pioneer CDJ-3000 and Traktor Pro both use a very short crossfade for slip return — short enough to be nearly instantaneous but long enough to avoid clicks. The exact duration is not documented, but listening tests suggest 1-3 ms (44-132 samples at 44.1 kHz).

This PRD uses 64 samples (~1.45 ms at 44.1 kHz), matching the transport crossfade established in PRD-0004. This provides consistency across all seek-type operations (play, pause, seek, loop-back, slip return) and is short enough that the DJ perceives the snap-back as instantaneous. A longer crossfade (e.g., 256 samples / ~5.8 ms) would smooth the transition further but introduce a perceivable "blend" that sounds like a brief overlap of two different track positions — undesirable for slip, where the DJ expects a clean cut.

### 1.5.3. Needle Drop as Non-Displacement Action

A deliberate design choice: needle drop (PRD-0016 click-to-seek on waveform) resets both the primary and shadow playheads, rather than creating a displacement. Pioneer CDJ-3000 follows this convention. The reasoning is that needle drop is an explicit "I want to be here" navigation action, not a performance manipulation. If the DJ clicks on the waveform to seek to a new position, they are establishing a new reference point for the timeline — the shadow should follow.

In contrast, hot cue triggers, loops, reverse, and beat jumps are performance tools. The DJ expects these to be "temporary" manipulations that slip can undo. The distinction is: navigation actions (needle drop, CUE return, stop) reset the shadow; performance actions (loop, hot cue trigger, reverse, beat jump) displace against the shadow.

The CUE button (PRD-0004 temp cue) is classified as a navigation action because its semantic is "return to this reference point" — similar to needle drop. A DJ pressing CUE expects to anchor at the temp cue position, not to slip-displace from it.

### 1.5.4. Disabling Slip While Displaced: Stay vs. Snap Back

Two design approaches exist:
- **Stay at current position (chosen):** Pioneer CDJ-3000 behavior. Disabling slip while displaced keeps the audible playhead where it is. The DJ explicitly chose to turn off slip, meaning they want to continue from the current position. This is the safest behavior — the DJ is in control.
- **Snap back on disable:** Some DJ software snaps back when slip is disabled. This is dangerous because the DJ may be disabling slip to escape the slip paradigm entirely, and an unexpected position jump mid-performance could be disastrous.

This PRD follows Pioneer CDJ-3000: disabling slip while displaced discards the shadow and keeps the audible position. If the DJ wants to snap back, they tap the Slip button (short press for return) before disabling.

### 1.5.5. Shadow Playhead Overflow at End of Track

When the shadow playhead reaches `totalSamples - 1`, it clamps and holds. This matches the transport's end-of-track behavior (PRD-0004): the track does not advance past the last sample. If the DJ is looping near the end and the shadow reaches the end, snap-back will land at the final sample and the deck will transition to Stopped with a fade-out, exactly as if the track had played to completion naturally.

An alternative approach would be to auto-trigger snap-back when the shadow reaches end-of-track, forcing the DJ out of the loop. This PRD rejects that approach because it removes control from the DJ. The DJ may intentionally loop at the end of a track during a transition, and an auto-snap-back could interrupt the mix. The visual indicator (ghost marker at the end) provides sufficient feedback.

### 1.5.6. Dual Playhead vs. Position Offset Implementation

Two architectural approaches were evaluated:
- **Dual playhead (chosen):** Maintain a completely independent shadow playhead (`double`) alongside the primary. The shadow has its own accumulator and advances independently within `processBlock`. On snap-back, a seek is issued to the shadow position.
- **Position offset:** Store a single `int64_t` offset representing the difference between the shadow and primary positions. The shadow position is computed as `primary + offset` at any time. On snap-back, the primary jumps by the offset.

The dual playhead approach is chosen because:
1. When `speedMultiplier` changes during a displacement, both accumulators pick up the new speed immediately. An offset would need to be recomputed on every speed change to avoid drift.
2. Loop wrap-around creates non-linear primary playhead motion (jumps backward), which makes offset arithmetic error-prone. A shadow that simply ignores the wrap and advances linearly is simpler and more reliable.
3. The cost is one additional `double` accumulator per deck — negligible in both memory and CPU.

### 1.5.7. Slip and Hot Cue: Trigger-and-Continue vs. Momentary

Pioneer CDJ-3000 treats hot cue triggers as "momentary" in slip mode: the DJ holds a hot cue pad, audio plays from the cue position, and releasing the pad triggers snap-back. Traktor Pro treats hot cues as trigger-and-continue: the hot cue fires and playback continues from the cue position until the DJ explicitly returns.

Sonik's hot cue model (PRD-0012) is trigger-and-continue (pressing a pad at any time starts playback from the cue, no hold semantics). To accommodate slip return from hot cue displacement, this PRD introduces the "slip return" action (tapping the Slip button while displaced) as the explicit snap-back mechanism. This gives the DJ full control over when to return to the shadow timeline without introducing hold-to-preview semantics on hot cue pads, which would conflict with PRD-0012's established trigger model.

A future PRD could add a "momentary hot cue" mode (hold pad = play from cue, release = return) as an alternative trigger mode. This is out of scope for PRD-0017.

### 1.5.8. Slip Button Interaction: Short Press vs. Long Press

The Slip button serves dual purpose when a displacement is active: short press (under 300 ms) triggers snap-back (slip return), long press (over 300 ms) toggles the `slipEnabled` state. This mimics Pioneer CDJ-3000's slip button interaction where the button is both a mode toggle and a return trigger.

If user testing reveals that the short/long press distinction causes accidental toggles, an alternative design is a separate "Slip Return" button. This can be introduced as a layout option without architectural changes, since the underlying snap-back mechanism is the same regardless of which button triggers it.