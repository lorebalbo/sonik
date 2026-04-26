---
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0009
  - PRD-0010
  - PRD-0011
---

# 1. PRD-0025: Key Stepper Pitch Transposition

## 1.1. Problem

The Key Stepper component (`KeyStepperComponent`) allows the DJ to navigate through Camelot key positions, but the selection has no effect on the audio: the playback pitch is not altered. As a result, the key stepper is a purely decorative display with no functional utility. A DJ performing a harmonic mix or using a remix edit technique relies on being able to transpose a track in real time by semitone increments; without this capability the Key Stepper is misleading and unusable.

Additionally, there is no defined relationship between the Key Stepper and the Key Lock button (`KEY`): it is currently possible to operate both simultaneously, creating a conceptual conflict where key lock is supposed to maintain a stable pitch independent of the speed fader, while the stepper implies an intentional pitch change. This ambiguity makes the expected audio behavior undefined and unpredictable.

## 1.2. Objective

The system provides real-time, artifact-free pitch transposition via the Key Stepper such that:
- Every increment or decrement of `keyShift` by the user causes the audio engine to apply the corresponding semitone pitch shift to the playing audio, using RubberBand R3's `pitchScale` parameter.
- The transition to the new pitch is smooth: no clicks, pops, or audible glitches occur at any step during normal playback.
- The Key Stepper arrows are disabled (non-interactive, visually dimmed) whenever the Key Lock button (`KEY`) is active.
- When the Key Lock button is activated, the current `keyShift` offset is preserved and the pitch remains at its current transposed value — the pitch is not reset to zero.

## 1.3. User Flow

1. A track is playing on a deck. Key Lock is off. The Key Stepper shows the detected key (`8A`, `keyShift = 0`). Playback pitch is at the natural pitch for the current speed-fader position.
2. The user presses the `>` arrow on the Key Stepper. `keyShift` increments to `1`. The display updates to `9A`. Within 50 ms the audio pitch smoothly shifts up by 1 semitone (a ratio of `2^(1/12) ≈ 1.0595`) with no audible artifact.
3. The user continues pressing `>`. Each step adds 1 semitone. At `keyShift = 12` the pitch is up a full octave; at `keyShift = 13` it wraps to `−11` semitones (or continues on a signed axis, depending on implementation — see Grey Areas).
4. The user presses `<` to step back down. Pitch decreases smoothly by 1 semitone per click.
5. The user presses the `KEY` button to activate Key Lock. Immediately: the Key Stepper arrows gray out and become non-interactive. The current `keyShift = 2` is preserved. Playback continues at +2 semitones; Key Lock now additionally compensates for speed-fader tempo changes so pitch stays locked to +2 semitones regardless of tempo.
6. The user moves the pitch fader. The track tempo changes but the pitch (including the +2 semitone transposition) remains constant.
7. The user deactivates Key Lock. The Key Stepper arrows become active again. `keyShift` is still `2`; pitch is still +2 semitones.
8. The user steps `keyShift` back to `0`. The pitch returns to the natural pitch of the current speed-fader position smoothly.

### 1.3.1. Edge Cases

- If Key Lock is activated while `keyShift = 0`, pitch is unchanged (no transposition, only speed-to-pitch compensation).
- If Key Lock is deactivated while `keyShift ≠ 0`, the pitch shift from `keyShift` remains active. The DJ is responsible for stepping back to 0 if they want the natural pitch.
- If the track has no detected key (`keyIndex = −1`), the Key Stepper still operates in pitch-shift mode using `keyShift` as pure semitone offset; the display shows a relative indicator (e.g., `+2`) rather than a Camelot key label.
- When the audio engine is in bypass (no track loaded or playback stopped), `keyShift` changes are stored in state but produce no audio effect.

## 1.4. Acceptance Criteria

- [ ] `IDs::keyShift` is re-defined as a signed integer in the range −12..+12 (semitones), replacing the previous 0..23 unsigned wraparound representation. The default value is `0` (no transposition).
- [ ] `AudioStateSync` reads `IDs::keyShift` from the deck root ValueTree and propagates it to a new `DeckAudioState::keyShiftSemitones` atomic int.
- [ ] In `DeckAudioSource::processBlock`, the effective pitch scale applied to `TimeStretcher` is `pitchScale = pow(2.0, keyShiftSemitones / 12.0)`.
- [ ] The `TimeStretcher` interface exposes `setPitchScale(double scale)` and the pitch scale is updated on the audio thread from `keyShiftSemitones` before each `process()` call. The RubberBand R3 engine handles the pitch change incrementally within its processing window, producing artifact-free transposition.
- [ ] When Key Lock is off (`keyLockEnabled = false`): `timeRatio = 1.0` (speed tracks pitch) and `pitchScale = pow(2.0, keyShiftSemitones / 12.0)`.
- [ ] When Key Lock is on (`keyLockEnabled = true`): `timeRatio = 1.0 / speedMultiplier` (tempo compensation) and `pitchScale = pow(2.0, keyShiftSemitones / 12.0)`. Both parameters are applied simultaneously.
- [ ] Pitch scale changes from Key Stepper use the same 64-sample crossfade mechanism already present for key lock on/off transitions, preventing audible clicks.
- [ ] `KeyStepperComponent` observes `IDs::keyLockEnabled` on the deck ValueTree. When `keyLockEnabled = true`: both arrow buttons are set to `setEnabled(false)`, their fill renders as `#B0B0B0` (dimmed), and `MouseCursor::NormalCursor` is used (no pointing-hand cursor). When `keyLockEnabled = false`: buttons are re-enabled and render in their normal interactive style.
- [ ] Activating Key Lock (`IDs::keyLockEnabled` set to `true`) does NOT reset `IDs::keyShift` to `0`. The current value is preserved.
- [ ] `KeyStepperComponent` clicking `<` decrements `IDs::keyShift` by 1 (minimum −12). Clicking `>` increments by 1 (maximum +12). Values clamp at the extremes; no wraparound.
- [ ] When `keyShift = 0` and `keyIndex` is a valid detected key (0..23), the stepper display shows the Camelot label of the detected key via `KeyUtils::toCamelot(keyIndex)`.
- [ ] When `keyShift ≠ 0` and `keyIndex` is valid, the display shows the Camelot label of the transposed key: `KeyUtils::toCamelot(((keyIndex + keyShift) % 24 + 24) % 24)`.
- [ ] When `keyIndex = −1` (unknown key), the display shows the signed offset as a string: `+0`, `+1`, `−3`, etc.
- [ ] Pitch transposition from `keyShift` is independent of and additive to any pitch change from the speed fader. Effective sounding pitch in semitones above concert pitch = `keyShiftSemitones + 12 * log2(speedMultiplier)`.
- [ ] The audio thread never allocates memory or takes locks when reading `keyShiftSemitones`.
- [ ] All ValueTree writes from `KeyStepperComponent` occur on the JUCE message thread.
- [ ] UI changes are confined to `Source/Features/Deck/UI/KeyStepperComponent.*`.
- [ ] Audio engine changes are confined to `Source/Features/AudioEngine/DeckAudioSource.*`, `Source/Features/TimeStretch/TimeStretcher.*`, and `Source/Features/Deck/AudioStateSync.*`.

## 1.5. Grey Areas

1. **Key shift range and wrapping.** The original `keyShift` (0..23) wrapped around the Camelot wheel. With pitch transposition, wrapping at ±12 (one octave) is more musically natural and avoids extreme pitch ratios. Resolution: clamp at −12/+12 semitones with no wraparound. A shift of ±12 is a full octave, which is an extreme use case a DJ would intentionally choose. Wrapping silently from +12 to −11 would cause a jarring pitch jump.

2. **Interaction between Key Lock and Key Stepper pitch.** When Key Lock is on, RubberBand maintains pitch at a fixed absolute level regardless of the pitch fader. If the Key Stepper could still be operated while Key Lock is on, the expected combined behavior (both a semitone shift AND a key lock maintaining it) would be correct technically but confusing UX-wise — the DJ would have two pitch controls active simultaneously. Resolution: disable the Key Stepper arrows when Key Lock is active, forcing the DJ to choose one mechanism at a time. The current `keyShift` is always preserved so re-enabling the stepper after disabling Key Lock resumes from the last-set value.

3. **Smoothness of rapid successive steps.** If the user rapidly clicks the Key Stepper multiple times in quick succession (e.g., stepping from 0 to +5 in half a second), each step triggers a pitch scale update. With RubberBand R3 in real-time mode, each update takes effect within the engine's processing latency (typically ≤ one analysis window ≈ 50 ms). Resolution: no additional smoothing is needed beyond what RubberBand provides; the 64-sample crossfade on each change prevents clicks even for rapid steps.