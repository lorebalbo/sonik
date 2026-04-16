---
name: "PRD-0018: Jog Wheel"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0004
  - PRD-0011
  - PRD-0017
---

# 1. PRD-0018: Jog Wheel

## 1.1. Problem

After the transport system (PRD-0004), pitch fader (PRD-0010), and slip mode (PRD-0017) are implemented, a DJ can play, pause, seek, and adjust tempo — but has no tactile, continuous-control interface for physically manipulating the audio stream. On a Pioneer CDJ-3000, the jog wheel is the most-used control surface on the entire unit: DJs touch the top platter to scratch and back-cue, spin the outer ring to nudge tempo for beat-matching, and scrub through tracks during preparation. Without a jog wheel, Sonik reduces the DJ to discrete button presses and static fader positions — eliminating scratch performance, manual beat-matching by ear, and the intuitive "hands on the music" interaction that defines the DJ workflow.

In hardware, the CDJ-3000 jog wheel is a 206mm capacitive platter with a conductive touch surface on top and a mechanical ring around the perimeter. Touching the top surface engages vinyl mode (direct pitch control, like placing a finger on a spinning record), while spinning the outer ring applies a temporary pitch bend proportional to rotation velocity. Software DJ applications (Traktor Pro, Virtual DJ, djay Pro, Serato) replicate this interaction with a virtual jog wheel rendered on screen, mapping mouse drag gestures to angular rotation. The challenge is translating a 2D mouse interaction on a circular widget into a natural-feeling rotational input that captures the physical momentum, resistance, and velocity sensitivity of a real platter — all while feeding the correct speed and displacement values into the audio transport with zero latency.

Without a virtual jog wheel, Sonik cannot offer scratch performance (a core DJ technique), manual beat-alignment by pitch nudging (essential when sync is unavailable or untrusted), or fluid track scrubbing during cue preparation. These are not optional features — they are prerequisites for professional use.

## 1.2. Objective

The system provides a per-deck virtual jog wheel component that:
- Renders an interactive circular platter with two interaction zones: an inner platter (scratch/vinyl mode) and an outer ring (pitch bend mode), visually modeled after the Pioneer CDJ-3000 layout.
- Maps mouse drag gestures on the circular widget to angular rotation, converting angular displacement and velocity into transport commands (direct playhead control for scratch, temporary `speedMultiplier` offset for pitch bend).
- Implements vinyl mode (togglable per deck): when vinyl mode is on and the user clicks the inner platter, playback speed becomes directly tied to mouse-driven angular velocity — releasing the platter resumes normal playback. When vinyl mode is off, clicking the inner platter behaves as a pitch bend (same as the outer ring).
- Feeds scratch displacement into the transport system by overriding the playhead advancement rate in `processBlock`, mapping angular velocity (degrees per second) to a playback speed multiplier with sub-sample precision.
- Feeds pitch bend into the transport system by applying a temporary additive offset to `speedMultiplier`, returning smoothly to the base speed on release via an exponential decay curve.
- Integrates with slip mode (PRD-0017): when slip is enabled and the user scratches, the shadow playhead continues advancing at the normal rate, enabling snap-back when the scratch ends.
- Integrates with time stretching (PRD-0011): when key lock is enabled, scratch and pitch bend still affect playback speed, but the stretcher compensates pitch — scratching with key lock on produces time-compressed/expanded audio at the original pitch.
- Animates the platter rotation continuously, with angular velocity matching the current effective playback speed — spinning forward during normal playback, reversing during back-scratch, and stopping when paused.
- Stores `vinylModeEnabled` as a deck-level boolean in the `juce::ValueTree` state tree (per PRD-0001 classification), persisting across track loads.
- Communicates all jog state between the UI thread and the audio thread via `std::atomic` — zero allocations, zero locks, and zero I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Vinyl Mode Scratch (Inner Platter)

1. The user has Deck A with a track loaded and playing. Vinyl mode is enabled (the default). The virtual jog wheel displays a spinning platter with a position marker (dot or line) rotating clockwise at a rate proportional to the current `speedMultiplier` (one full rotation per 1.8 seconds at 33-1/3 RPM equivalent).
2. The user clicks and holds the mouse on the inner platter surface. Playback immediately stops advancing at the transport's normal rate — the playhead is now directly controlled by the mouse. The platter animation freezes at its current angle. This is analogous to placing a finger on a spinning vinyl record.
3. The user drags the mouse in a clockwise circular motion around the platter center. The system computes the angular displacement from the previous mouse position and translates it to a sample displacement: one full 360-degree rotation equals exactly 1.8 seconds of audio (matching the 33-1/3 RPM convention). The playhead advances forward through the audio by the computed displacement. The DJ hears the audio playing forward, pitched proportionally to the drag speed.
4. The user reverses direction, dragging counter-clockwise. The playhead moves backward through the audio. The DJ hears the audio in reverse — a classic scratch sound. The platter animation reverses direction to match.
5. The user moves the mouse rapidly back and forth in short arcs. The playhead oscillates forward and backward over a small region of audio, producing the characteristic "wikka-wikka" scratch effect. Each direction change is instantaneous — no crossfade or ramp is applied during scratch, because the DJ's hand movement IS the audio signal.
6. The user releases the mouse button. The system transitions out of scratch mode. If the deck was in Playing state before the scratch, playback resumes at the normal `speedMultiplier`. A short spin-up ramp (150 ms exponential ease-in from 0.0 to 1.0 speed) is applied to simulate the platter returning to full speed, replicating the feel of releasing a real vinyl record. If the deck was Paused, the playhead holds at the release position and the deck remains Paused.
7. During the scratch, if slip mode (PRD-0017) is enabled, the shadow playhead continues advancing at the normal `speedMultiplier`. When the user releases the platter, playback snaps back to the shadow position with a 64-sample crossfade (per PRD-0017 snap-back behavior), creating a seamless return to the track's natural timeline.

### 1.3.2. Pitch Bend (Outer Ring)

8. The user clicks and holds the mouse on the outer ring of the jog wheel (the annular region between the inner platter and the widget boundary). Playback continues normally — the playhead does not stop.
9. The user drags clockwise on the outer ring. The system computes angular velocity (degrees per second) and maps it to a temporary speed offset. A moderate clockwise drag (~180 degrees/second) produces a +4% speed increase. The transport's effective speed becomes `baseSpeedMultiplier + bendOffset`. The DJ hears the track playing slightly faster — this is used to push a track forward to align beats with the other deck.
10. The user holds the drag at a steady angular velocity. The bend offset remains constant as long as the drag velocity is steady. The track plays at the elevated speed continuously.
11. The user slows the drag or stops moving (mouse held but stationary). The bend offset decays toward zero via an exponential curve (time constant 80 ms), smoothly returning the playback speed to the base `speedMultiplier`. This prevents an abrupt speed change when the user pauses their drag motion.
12. The user drags counter-clockwise. The system applies a negative speed offset (same magnitude curve), slowing the track. This is used to pull a track back for beat alignment.
13. The user releases the mouse button. The bend offset decays to zero via the same exponential curve (80 ms time constant). Playback returns to the base `speedMultiplier` smoothly.
14. Pitch bend does not create a slip displacement. The speed change is temporary and the playhead continues along the natural timeline — there is no divergence between the audible and shadow playheads.

### 1.3.3. Vinyl Mode Off (Inner Platter as Pitch Bend)

15. The user clicks the Vinyl Mode button on Deck A. The button toggles from active (illuminated) to inactive (dimmed). The state tree's `vinylModeEnabled` property for Deck A updates to `false`.
16. The user clicks and holds the mouse on the inner platter. Instead of stopping playback and entering scratch mode, the inner platter now behaves identically to the outer ring — applying a temporary pitch bend based on angular velocity. The platter animation continues spinning (it does not freeze on touch).
17. This matches the CDJ-3000's vinyl mode toggle: when vinyl mode is off on a CDJ, touching the platter top nudges tempo instead of scratching. DJs disable vinyl mode when they want to nudge without risk of accidentally stopping the track.
18. The user clicks the Vinyl Mode button again. Vinyl mode re-enables. The inner platter returns to scratch behavior.

### 1.3.4. Track Scrubbing (Paused or Stopped State)

19. The deck is Paused with a track loaded. Vinyl mode is enabled. The platter is stationary (no rotation animation).
20. The user clicks the inner platter and drags clockwise slowly. The playhead advances through the audio at the drag rate, and the DJ hears the audio scrubbing forward at low speed. This is cue preparation — the DJ is searching for the exact beat or sound to set a cue point on.
21. The user drags counter-clockwise. The playhead scrubs backward. The DJ hears the audio in reverse at the drag speed.
22. The user releases the mouse. The playhead holds at the current position. The deck remains Paused. No spin-up ramp is applied (the deck was not playing before the scrub).
23. The user sets a cue point (PRD-0012) at the scrubbed position. This is a standard DJ preparation workflow: scrub to the exact start of a phrase, set a cue.

### 1.3.5. Jog Wheel on Empty or Stopped Deck

24. The deck is in Empty state. The jog wheel renders in a disabled state (dimmed, no platter marker). Clicking has no effect.
25. The deck is in Stopped state with a track loaded. The platter is stationary. The vinyl mode button is active. Clicking the inner platter and dragging allows scrubbing (same as step 19-22). The outer ring pitch bend has no effect because the deck is not playing.

### 1.3.6. Interaction with Key Lock

26. Key lock is enabled on Deck A (PRD-0011). The user scratches on the inner platter. The Rubber Band stretcher receives audio at the scratch-driven speed. Because key lock preserves pitch across speed changes, the scratch sound maintains the original key at all drag velocities instead of pitching up/down proportionally. This produces a time-compressed/expanded scratch rather than a traditional pitched scratch.
27. For most DJs, traditional pitched scratch (vinyl-mode behavior) is preferred during scratching. Therefore, when the user enters scratch mode (platter touch in vinyl mode), key lock is temporarily bypassed — the audio path switches to the vinyl-mode linear interpolation (PRD-0004) for the duration of the scratch. When the user releases the platter, key lock re-engages with a 64-sample crossfade.
28. Key lock remains active during pitch bend operations (outer ring). The temporary speed offset is processed through the stretcher, maintaining pitch stability while the DJ nudges tempo.

### 1.3.7. Visual Design and Animation

29. The jog wheel renders as a circular widget per deck, centered below the waveform displays and above the transport controls. The widget has a fixed diameter determined by the deck layout (responsive to window size, minimum 120px, maximum 220px diameter).
30. The inner platter occupies the inner 70% of the radius. It is rendered as a filled circle in a dark matte color (matching the deck's surface theme) with a single radial position marker — a bright dot or short line at the platter's edge — that indicates the current rotational phase.
31. The outer ring occupies the remaining 30% of the radius. It is rendered as an annular band with a subtle textured or ridged appearance (concentric arc segments) to visually differentiate it from the platter. It is slightly lighter than the inner platter.
32. During playback, the position marker rotates at a rate of `speedMultiplier * 33.333 RPM`. At 1.0x speed, one full rotation takes 1.8 seconds (33-1/3 RPM, the standard vinyl turntable speed). At 1.06x, the rotation is 6% faster. During reverse playback or back-scratch, the marker rotates counter-clockwise.
33. When the deck is Paused or Stopped, the marker is stationary at its last angle. When the deck is Empty, the marker is at the 12 o'clock position.
34. When the user touches (clicks) the inner platter in vinyl mode, a subtle visual feedback is applied: the platter surface brightens slightly (10% luminance increase) to indicate the touch is registered.
35. The rotation animation updates at 60 fps (tied to the UI repaint timer). The rotation angle is computed from the atomic playhead position: `angle = (playheadPosition / samplesPerRotation) * 360.0`, where `samplesPerRotation = sampleRate * 1.8`.

### 1.3.8. Multiple Decks

36. Each deck has its own independent jog wheel instance. Jog state (vinyl mode, platter angle, bend offset) is entirely per-deck with zero cross-talk.
37. Only the focused deck's jog wheel responds to mouse input. Clicking a jog wheel on a non-focused deck first switches focus to that deck, then begins the interaction on the next mouse press.

## 1.4. Acceptance Criteria

### 1.4.1. State and Data Model

- [ ] Each deck's `juce::ValueTree` contains a `JogWheel` child node with property: `vinylModeEnabled` (`bool`).
- [ ] `vinylModeEnabled` defaults to `true` and is classified as deck-level state (per PRD-0001): it persists across track loads, ejects, and application restarts.
- [ ] The audio engine's per-deck transport data contains: `jogSpeedMultiplier` (`std::atomic<float>`, default 1.0) for scratch-driven playback rate, `jogBendOffset` (`std::atomic<float>`, default 0.0) for pitch bend offset, and `jogScratchActive` (`std::atomic<bool>`, default `false`) for whether scratch mode is engaged.
- [ ] All jog wheel state properties in the ValueTree are observable via the JUCE Listener pattern. UI components react to changes without polling.
- [ ] `jogScratchActive`, `jogSpeedMultiplier`, and `jogBendOffset` are not stored in the ValueTree (they change every mouse event and every `processBlock` cycle). They reside as `std::atomic` values in the audio engine's per-deck data.

### 1.4.2. Angular Displacement and Velocity Computation

- [ ] The jog wheel maps mouse drag gestures to angular rotation using the `atan2` technique: the angle from the platter center to the current mouse position is computed as `theta = atan2(dy, dx)`, and angular displacement per mouse event is `deltaTheta = currentTheta - previousTheta`, with wrap-around handling at the +/-180-degree boundary.
- [ ] Angular velocity is computed as `angularVelocity = deltaTheta / deltaTime`, where `deltaTime` is the elapsed time between consecutive mouse drag events (measured via `juce::Time::getMillisecondCounterHiRes()`). `deltaTime` is clamped to a minimum of 1 ms to prevent division by zero from high-frequency mouse events.
- [ ] Angular displacement is expressed in degrees. One full rotation (360 degrees) corresponds to exactly `sampleRate * 1.8` samples of audio (the 33-1/3 RPM convention). The samples-per-degree constant is `samplesPerDegree = sampleRate * 1.8 / 360.0`.
- [ ] Mouse events occurring at less than 2 pixels from the platter center are ignored to prevent erratic angle computation when the cursor crosses the center point.

### 1.4.3. Scratch Mode (Vinyl Mode On, Inner Platter)

- [ ] When `vinylModeEnabled` is `true` and the user presses the mouse button within the inner platter region, the system sets `jogScratchActive = true` and records the initial mouse angle and playhead position.
- [ ] While `jogScratchActive` is `true`, the transport's playhead advancement in `processBlock` is overridden: instead of advancing by `bufferSize * speedMultiplier`, the playhead is driven by the accumulated angular displacement from the UI thread. The UI writes `jogSpeedMultiplier` as the ratio of drag-driven sample advancement to real-time sample advancement, updated on each mouse drag event.
- [ ] `jogSpeedMultiplier` during scratch is computed as: `jogSpeedMultiplier = (deltaTheta * samplesPerDegree) / (deltaTime * sampleRate)`. At a drag rate equivalent to 33-1/3 RPM (360 degrees in 1.8 seconds), `jogSpeedMultiplier` equals 1.0. Faster drag produces values greater than 1.0. Reverse drag produces negative values.
- [ ] When `jogScratchActive` is `true`, `processBlock` reads `jogSpeedMultiplier` and uses it as the effective speed for playhead advancement. If `jogSpeedMultiplier` is negative, the playhead advances backward (reverse audio).
- [ ] When `jogSpeedMultiplier` is negative, `processBlock` reads samples in reverse order from the decoded buffer and writes them to the output. The same sub-sample interpolation from PRD-0004 is applied, using the absolute value of `jogSpeedMultiplier` for the interpolation fraction.
- [ ] When the mouse is held stationary on the platter (no drag movement), `jogSpeedMultiplier` decays toward 0.0 via exponential decay (time constant 30 ms, applied per mouse event at the UI event rate). This simulates the friction of a stopped hand on a record — the platter comes to a halt smoothly rather than snapping to zero.
- [ ] On mouse release, `jogScratchActive` transitions to `false`. If the deck was in Playing state before the scratch, the system applies a spin-up ramp: `jogSpeedMultiplier` transitions from its current value to 1.0 (or the deck's base `speedMultiplier`) via an exponential ease-in with a 150 ms time constant. This ramp is applied in `processBlock` by interpolating `jogSpeedMultiplier` toward the target speed each cycle.
- [ ] On mouse release, if the deck was in Paused state before the scratch, `jogSpeedMultiplier` snaps to 0.0 immediately (no spin-up). The playhead holds at the release position.
- [ ] During scratch, no 64-sample crossfade is applied on direction changes. The scratch signal is the DJ's direct manipulation and any fade would soften the attack of the scratch sound.
- [ ] The playhead position during scratch is clamped to `[0, totalSamples - 1]`. Scratching past either end of the track stops advancing in that direction.

### 1.4.4. Pitch Bend Mode (Outer Ring, or Inner Platter with Vinyl Mode Off)

- [ ] When the user presses the mouse button within the outer ring region (or within the inner platter when `vinylModeEnabled` is `false`), pitch bend mode activates. `jogScratchActive` remains `false`. Playback continues at the current transport speed.
- [ ] While in pitch bend mode, the system computes angular velocity from mouse drag events and maps it to a `jogBendOffset` value that is added to the base `speedMultiplier`.
- [ ] The pitch bend mapping function is: `jogBendOffset = clamp(angularVelocity / maxAngularVelocity, -1.0, 1.0) * maxBendRange`, where `maxAngularVelocity` is 720 degrees/second (two full rotations per second, representing a fast deliberate spin) and `maxBendRange` is 0.10 (10% of the base speed).
- [ ] The mapping from angular velocity to bend offset uses a quadratic curve: `normalizedVelocity = angularVelocity / maxAngularVelocity`, `jogBendOffset = sign(normalizedVelocity) * normalizedVelocity^2 * maxBendRange`. This provides fine resolution at low velocities (subtle nudging) and larger offsets at high velocities (aggressive pushes).
- [ ] Clockwise drag produces a positive `jogBendOffset` (speed up). Counter-clockwise drag produces a negative `jogBendOffset` (slow down).
- [ ] The effective playback speed in `processBlock` is: `effectiveSpeed = baseSpeedMultiplier + jogBendOffset`. The transport uses this value for playhead advancement.
- [ ] `effectiveSpeed` is clamped to `[0.0, maxSpeedMultiplier]`. The speed cannot go negative via pitch bend (reverse playback is not triggered by bending). The `maxSpeedMultiplier` matches the pitch fader's maximum range (e.g., 1.50 at +-50% range from PRD-0010).
- [ ] When the user stops dragging (mouse held but stationary) or releases the mouse button, `jogBendOffset` decays toward 0.0 via exponential decay with a time constant of 80 ms. The decay is applied in the UI thread on a timer callback at 60 Hz, writing the decaying value to the `std::atomic<float>`.
- [ ] The pitch bend range (default 10%) is not user-configurable in this PRD. A future settings PRD may expose it.

### 1.4.5. Spin-Up and Brake Behavior

- [ ] On platter release after scratch (deck was Playing), the spin-up ramp uses an exponential ease-in: `jogSpeedMultiplier(t) = targetSpeed * (1 - e^(-t / tau))`, where `tau = 150 ms` and `targetSpeed = baseSpeedMultiplier`. The ramp is computed per `processBlock` cycle: `jogSpeedMultiplier += (targetSpeed - jogSpeedMultiplier) * (1 - e^(-blockDuration / tau))`.
- [ ] On platter touch (entering scratch from Playing state), the speed transitions from the current `speedMultiplier` to the mouse-driven rate. No artificial brake ramp is applied — the DJ's hand position immediately controls speed, which is the expected vinyl feel (you grab the record, it stops where your hand is).
- [ ] During the spin-up ramp, if the user re-touches the platter, the ramp is canceled and direct scratch control resumes immediately.
- [ ] The spin-up ramp produces audio at intermediate speeds. Key lock (PRD-0011) is re-engaged when the ramp completes (rising above 0.5x speed), not during the ramp.

### 1.4.6. Interaction with Transport System (PRD-0004)

- [ ] The transport system's `processBlock` reads three atomic values from the jog wheel: `jogScratchActive`, `jogSpeedMultiplier`, and `jogBendOffset`.
- [ ] When `jogScratchActive` is `true`, the transport ignores the base `speedMultiplier` and uses `jogSpeedMultiplier` exclusively for playhead advancement. The advancement per cycle is `bufferSize * jogSpeedMultiplier` samples (can be negative for reverse).
- [ ] When `jogScratchActive` is `false`, the transport computes `effectiveSpeed = baseSpeedMultiplier + jogBendOffset` and advances the playhead by `bufferSize * effectiveSpeed` samples per cycle.
- [ ] When `jogBendOffset` is 0.0 and `jogScratchActive` is `false`, the transport operates exactly as defined in PRD-0004 with zero overhead from the jog wheel.
- [ ] Seek, play, pause, and stop commands (PRD-0004) take priority over jog input. If the user presses Pause while scratching, scratch mode disengages and the deck pauses. If the user presses Play while paused and scrubbing, the deck transitions to Playing and the scratch continues.
- [ ] CUE button behavior (PRD-0004) operates independently of jog state. Pressing CUE during a scratch disengages scratch mode and executes the CUE return.

### 1.4.7. Interaction with Slip Mode (PRD-0017)

- [ ] When slip mode is enabled and the user enters scratch mode, the system sets `slipDisplaced = true` (per PRD-0017). The shadow playhead continues advancing at the base `speedMultiplier` while the audible playhead follows the scratch.
- [ ] When the user releases the platter (scratch ends) and slip is enabled, the transport performs a slip snap-back: the audible playhead snaps to the shadow position with a 64-sample crossfade. The spin-up ramp is not applied — the snap-back is instantaneous (the DJ expects to return to the timeline, not to hear a gradual speed-up from the scratch).
- [ ] When slip mode is enabled and the user performs a pitch bend (outer ring), no slip displacement is created. Pitch bend is a temporary speed adjustment along the natural timeline, not a divergence from it.
- [ ] The slip snap-back after scratch uses the existing snap-back mechanism from PRD-0017 (transport seek command via lock-free queue + 64-sample crossfade). No new snap-back mechanism is introduced.

### 1.4.8. Interaction with Key Lock (PRD-0011)

- [ ] When scratch mode is active (`jogScratchActive == true`) and key lock is enabled, key lock is temporarily bypassed. The audio path switches from the Rubber Band stretcher to the vinyl-mode linear interpolation path (PRD-0004). This produces traditional pitched scratch sounds.
- [ ] On entering scratch mode, the bypass is applied immediately (no crossfade from stretched to vinyl audio — the scratch onset is the perceptual masking event).
- [ ] On exiting scratch mode (platter release), if key lock is enabled and the spin-up ramp is active, key lock re-engages when `jogSpeedMultiplier` rises above 0.5 of the base `speedMultiplier`, with a 64-sample crossfade from vinyl-mode to stretched output.
- [ ] If slip mode is also enabled (snap-back instead of spin-up), key lock re-engages immediately at the snap-back position with the 64-sample crossfade included in the snap-back transition.
- [ ] Key lock remains active during pitch bend operations. The bend-adjusted speed is processed through the Rubber Band stretcher, maintaining pitch stability.

### 1.4.9. Visual Rendering

- [ ] The jog wheel is rendered as a JUCE `Component` subclass (`JogWheelComponent`), positioned per-deck in the deck UI layout.
- [ ] The widget has a minimum size of 120x120 pixels and a maximum of 220x220 pixels, scaling proportionally with the deck layout.
- [ ] The inner platter is a filled circle occupying the inner 70% of the component radius. The outer ring occupies the annular region from 70% to 95% of the radius. The outermost 5% is unused padding.
- [ ] The inner platter is rendered in the deck's dark surface color. The outer ring is rendered 15% lighter than the inner platter.
- [ ] A single radial position marker (a bright dot, 4px diameter, in the deck's accent color) is drawn at the inner edge of the platter, at the current rotation angle.
- [ ] The rotation angle is updated every repaint cycle (60 fps target) by reading the `std::atomic<int64_t>` playhead position and computing: `angle = fmod((playheadPosition / samplesPerRotation) * 360.0, 360.0)`, where `samplesPerRotation = sampleRate * 1.8`.
- [ ] During scratch, the rotation angle is driven by the accumulated angular displacement from mouse events, not by the playhead position. This ensures the platter visually tracks the user's hand immediately, with no latency from the audio thread round-trip.
- [ ] When the user's mouse hovers over the inner platter, the cursor changes to an open hand icon. When clicked (scratching), the cursor changes to a closed hand icon.
- [ ] When the user's mouse hovers over the outer ring, the cursor changes to a horizontal resize icon (indicating push/pull interaction).
- [ ] The vinyl mode toggle button renders adjacent to the jog wheel (bottom-left corner outside the circular widget). It has two visual states: active (illuminated in deck accent color, with a small vinyl record icon) and inactive (dimmed).
- [ ] When the deck is in Empty state, the jog wheel renders with 40% opacity and does not respond to mouse events.

### 1.4.10. Mouse Sensitivity and Acceleration

- [ ] Raw angular velocity from mouse events is smoothed using an exponential moving average (EMA) with a smoothing factor of `alpha = 0.3`: `smoothedVelocity = alpha * rawVelocity + (1 - alpha) * previousSmoothedVelocity`. This reduces jitter from uneven mouse event timing without adding perceptible latency.
- [ ] For scratch mode, the angular displacement is not smoothed — it is applied directly to maintain sample-accurate scratch feel. Only the velocity (used for computing `jogSpeedMultiplier`) is smoothed.
- [ ] For pitch bend mode, both displacement and velocity are smoothed via the EMA to produce a fluid nudge feel.
- [ ] The system handles mouse events at the OS-reported rate (typically 60-120 Hz for standard mice, up to 1000 Hz for gaming mice). High-frequency events produce smaller `deltaTheta` and `deltaTime` values, which the EMA smoothing normalizes.
- [ ] If the mouse moves outside the jog wheel bounds during a drag, the interaction continues (mouse capture is active). The angular computation uses the projected angle from the center, even if the mouse is far from the widget. This prevents abrupt disengagement during fast scratches.
- [ ] If the mouse re-enters the jog wheel after moving far outside (distance from center exceeds 3x the widget radius), the previous angle is reset to the re-entry angle to prevent a large instantaneous angular jump.

### 1.4.11. Audio Thread Safety

- [ ] `jogScratchActive` is read by the audio thread via `std::atomic<bool>`. It is written by the UI thread only.
- [ ] `jogSpeedMultiplier` is read by the audio thread via `std::atomic<float>`. It is written by the UI thread on mouse drag events and by the UI thread's timer callback during the spin-up ramp and hold-decay.
- [ ] `jogBendOffset` is read by the audio thread via `std::atomic<float>`. It is written by the UI thread on mouse drag events and by the UI thread's timer callback during bend decay.
- [ ] Zero memory allocations, zero mutex locks, and zero I/O occur on the audio thread for any jog wheel operation.
- [ ] The UI thread writes all jog atomics. The audio thread only reads them. There is no audio-to-UI feedback loop through jog atomics (the UI derives platter angle from the playhead position atomic, which is a separate communication channel).
- [ ] All angular arithmetic on the UI thread uses `double` precision to prevent accumulation error over extended scratch sessions.

### 1.4.12. UI Controls

- [ ] A Vinyl Mode toggle button is provided per deck, positioned adjacent to the jog wheel component.
- [ ] The Vinyl Mode button has two visual states: active (illuminated in deck accent color) and inactive (dimmed).
- [ ] Button tooltip: "Vinyl Mode (touch platter to scratch when on, pitch bend when off)".
- [ ] The Vinyl Mode button is clickable in all deck states (Empty, Stopped, Paused, Playing).
- [ ] The jog wheel component is non-interactive (dimmed, no mouse response) only when the deck is in Empty state.

### 1.4.13. Scope Boundaries

- [ ] MIDI jog wheel / hardware controller input is NOT implemented in this PRD. The `jogScratchActive`, `jogSpeedMultiplier`, and `jogBendOffset` atomic interface is designed to accept input from any source (mouse, MIDI, touchscreen). A future MIDI mapping PRD will connect hardware jog wheels to these same atomics.
- [ ] Touchscreen multi-touch jog interaction is NOT implemented in this PRD. Mouse input only.
- [ ] Jog wheel sensitivity preferences (adjustable acceleration curves, bend range configuration) are NOT implemented in this PRD. Hardcoded defaults are used.
- [ ] Back-spin effect (flicking the platter to create a powered reverse spin with momentum) is NOT implemented in this PRD. On release, only the spin-up ramp (return to playing speed) is applied.
- [ ] Jog wheel audio feedback (platter start/stop sound effects) is NOT implemented in this PRD.
- [ ] All jog wheel UI code resides under `Source/Features/Deck/UI/`. All jog-related audio thread logic resides within the existing transport code under `Source/Features/AudioEngine/`.

## 1.5. Grey Areas

### 1.5.1. Circular Drag Gesture vs Linear Mapping

Software DJ applications use two approaches for mapping mouse input to jog rotation. The first is true circular mapping: the system computes the angle from the widget center to the mouse cursor using `atan2` and tracks angular displacement. This is geometrically accurate but produces dead zones near the center (small movements have no angular impact) and inconsistency at different radii (the same physical mouse movement produces more degrees near the center and fewer at the edge). The second is linear mapping: horizontal mouse displacement is directly mapped to rotation, ignoring the circular geometry entirely. This is simpler but feels unnatural on a circular widget and breaks down when the user drags vertically.

**Resolution:** Use true circular `atan2` mapping with a minimum-radius dead zone. The dead zone (2 pixels from center) prevents erratic angle computation when the cursor crosses the center. The radius-dependent sensitivity variation is a feature, not a bug: it matches the physics of a real turntable, where the DJ's finger near the label (center) produces faster angular rotation for the same linear hand movement than a finger at the edge. The angular displacement is what matters for audio — not the linear mouse displacement. Users of Virtual DJ, djay Pro, and Traktor all report that circular mapping feels more natural than linear for extended scratch sessions.

### 1.5.2. Mouse Release Behavior: Spin-Up vs Instant Resume

When the DJ releases the platter after a scratch, there are two options. Hardware CDJs with mechanical platters exhibit a spin-up curve as the motor brings the platter back to speed (approximately 0.3-0.5 seconds on a Pioneer CDJ-3000 with the torque setting at default). Some software DJs (Traktor) apply a simulated spin-up for realism, while others (Virtual DJ) offer instant resume as an option. DJs specializing in turntablism prefer fast spin-up (under 100 ms), while mix-oriented DJs may prefer instant resume.

**Resolution:** Apply a 150 ms exponential spin-up ramp by default. This is fast enough for turntablism (the 150 ms at exponential rise reaches 90% speed within 50 ms — the perceptible "grab" is minimal) while providing the analog feel that mix DJs expect. Instant resume is available via slip mode: when slip is enabled, the spin-up ramp is bypassed entirely and playback snaps to the shadow position, which is effectively an instant resume at the correct timeline position. This dual behavior (spin-up without slip, instant snap-back with slip) covers both DJ preferences without a configuration option.

### 1.5.3. Pitch Bend Range and Curve Shape

CDJ-3000 outer ring pitch bend range varies by firmware version but is approximately +/-10% at maximum rotation speed. Traktor defaults to +/-10%. Serato defaults to +/-8%. A linear mapping between angular velocity and bend range feels unresponsive at low velocities (the DJ has to spin fast to get any effect) and oversensitive at high velocities (a small additional push causes a large speed jump). Some software uses logarithmic curves; others use piecewise linear.

**Resolution:** Use a quadratic curve with a 10% maximum range. The quadratic `f(v) = sign(v) * v^2 * maxBend` provides fine control at low velocities (gentle nudges of 0.5-1% for precise beat alignment) while allowing aggressive 5-10% pushes at high velocity for catching large tempo mismatches. The maximum angular velocity ceiling of 720 degrees/second (two full rotations per second) represents a fast, deliberate spin — achievable with a rapid wrist flick but not accidentally triggered by slow drags. The 10% range is sufficient for manual beat-matching across all practical BPM differences and matches industry conventions. If user feedback indicates a different range or curve is needed, this can be adjusted in a future settings PRD.

### 1.5.4. Key Lock During Scratch

When key lock is active and the DJ scratches, the Rubber Band stretcher receives audio at wildly varying speeds (including reverse). Rubber Band's real-time mode is designed for gradual speed changes (pitch fader movement) and is not optimized for the rapid, oscillating speed profiles of a scratch. The stretcher introduces latency (90-180 ms per PRD-0011) that would desynchronize the scratch sound from the DJ's hand movement, and the output quality degrades severely at extreme speed ratios and rapid reversals.

**Resolution:** Bypass key lock during scratch. When `jogScratchActive` becomes `true`, the audio path switches to vinyl-mode linear interpolation (PRD-0004), bypassing the Rubber Band stretcher entirely. This produces the traditional pitched scratch sound that DJs expect — a scratch that shifts pitch proportionally to speed is the musically correct behavior. Key lock re-engages when the platter is released and the spin-up ramp reaches a stable speed. This matches the behavior of Pioneer CDJ-3000, where key lock is effectively suspended during jog manipulation and re-engages when the platter returns to steady-state. The transition uses the existing 64-sample crossfade mechanism.

### 1.5.5. Scratch with Slip Mode: Spin-Up vs Snap-Back

When the DJ scratches with slip mode enabled, two conflicting release behaviors exist: the spin-up ramp (gradual return to play speed from the scratch position) and the slip snap-back (instant jump to the shadow position). Applying both would produce a spin-up at the shadow position, which is wrong — the shadow position is already at full speed. Applying spin-up first, then snap-back, would produce a brief audible ramp at the wrong position followed by a jump, which sounds terrible.

**Resolution:** Slip snap-back takes priority. When the user releases the platter with slip mode active, the spin-up ramp is skipped entirely and the transport performs an immediate snap-back to the shadow playhead with a 64-sample crossfade. The DJ hears the track resume from where it would have been, perfectly in phase — this is the entire purpose of slip mode. The spin-up ramp is a cosmetic analog simulation; slip snap-back is a functional timeline recovery tool. Functional wins. DJs who want spin-up feel should disable slip mode during scratch passages.

### 1.5.6. MIDI Jog Wheel Interface Design

Hardware MIDI controllers send jog data in different formats: relative CC messages (incremental ticks, e.g., +1/-1 per detent), absolute position messages (0-127 representing platter angle), or high-resolution NRPN messages. Pioneer DDJ controllers use a relative tick format at 128 ticks per revolution. Denon controllers use a similar scheme. The atomic interface (`jogSpeedMultiplier`, `jogBendOffset`, `jogScratchActive`) designed for mouse input must also accommodate these MIDI formats in a future PRD.

**Resolution:** The current atomic interface is intentionally format-agnostic. `jogSpeedMultiplier` is a dimensionless speed ratio (1.0 = normal speed) and `jogBendOffset` is a dimensionless speed offset — neither is tied to mouse pixels or screen coordinates. A future MIDI mapping PRD will implement a translation layer that reads MIDI jog messages (relative ticks, absolute angle, or raw velocity) and converts them into the same `jogSpeedMultiplier` and `jogBendOffset` values. The translation layer will handle controller-specific scaling (e.g., Pioneer's 128 ticks/revolution vs Denon's 1000 ticks/revolution). No changes to the audio engine or transport code will be required — only a new input adapter. This PRD documents the atomic interface contract so the MIDI PRD can target it directly.

### 1.5.7. Platter Animation Frame Rate and CPU Cost

Rendering a smoothly spinning platter at 60 fps requires a repaint every 16.7 ms. If the jog wheel component triggers a full deck UI repaint on every frame, this could consume significant CPU, especially with 4 decks. However, if the platter uses a lower frame rate (30 fps), the rotation of the position marker appears jerky, particularly at high playback speeds where the marker moves a large angular distance per frame.

**Resolution:** Use dirty-region repainting. The `JogWheelComponent` calls `repaint()` on itself (not the parent) at 60 fps via a `juce::Timer` callback. JUCE's repaint coalescing ensures only the jog wheel's bounding rectangle is re-rendered — not the entire deck. The platter background (circles, ring texture) is pre-rendered to an off-screen `juce::Image` on resize and composited without recomputation each frame. Only the position marker's angle changes per frame, requiring minimal draw cost (one `fillEllipse` call at the rotated position). At 4 decks, 4 timer callbacks at 60 fps is trivial CPU overhead. If profiling shows this is a concern, the timer can fall back to 30 fps when the deck is not focused.