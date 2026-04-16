---
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0004
---

# 1. PRD-0010: Pitch Fader and Gain Control

## 1.1. Problem

After the transport system (PRD-0004) gives a DJ the ability to play, pause, and seek through a loaded track, the DJ still has no way to adjust the playback speed or the volume of an individual deck. These two controls are fundamental to every DJ workflow. Pitch control is the primary tool for beatmatching: a DJ must be able to nudge one track's tempo by fractions of a percent until the beats of two tracks align. Without fine-grained pitch control, manual beatmatching is impossible and sync-based workflows lose the ability to override or fine-tune the automated tempo. Gain control is equally critical: tracks are mastered at wildly different loudness levels, and without a per-deck trim knob the DJ cannot level-match two tracks before blending them, resulting in jarring volume jumps during transitions that are immediately obvious to a dance floor.

The transport system already reads a `speedMultiplier` from `std::atomic<float>` and the audio engine already reads a per-deck gain value and applies it as a linear multiplier (PRD-0002). However, no UI control exists to set either value. The state tree (PRD-0001) defines fields for pitch and gain but nothing populates them from user interaction. Until this PRD is implemented, both values remain stuck at their defaults (speed 1.0, gain 0 dB) and the DJ has no interactive control over tempo or volume on any deck.

## 1.2. Objective

The system provides per-deck pitch fader and gain knob UI controls that:
- Allow the DJ to continuously adjust playback speed via a vertical pitch fader, mapped to the `speedMultiplier` consumed by the transport system (PRD-0004).
- Support selectable pitch ranges of +/-4%, +/-8%, +/-16%, and +/-50%, letting the DJ trade precision for range depending on the mixing scenario.
- Display the current pitch percentage with 0.01% resolution at the +/-4% range and proportionally coarser resolution at wider ranges.
- Provide a center detent mechanism (a dead zone around 0%) so the DJ can reliably return to original speed without hunting for the exact center.
- Offer a pitch reset button that instantly snaps the fader to 0% (speed multiplier 1.0).
- Allow the DJ to continuously adjust per-deck gain via a rotary knob, mapped to the gain multiplier consumed by the audio engine (PRD-0002).
- Support a gain range of -inf dB (full mute) to +12 dB, with a default of 0 dB (unity gain).
- Display the current gain value in dB alongside the knob.
- Write all pitch and gain values to the deck state tree (PRD-0001) so that observers (metering, future MIDI mapping, future sync engine) can read them without polling.
- Ensure all state updates from UI interaction propagate to the audio thread exclusively through `std::atomic` writes with no allocations, locks, or I/O on the audio thread.

## 1.3. User Flow

1. The user has Deck A in the Stopped state with a track loaded. The pitch fader is centered at 0.00%, the pitch range selector shows +/-8% (default), and the gain knob is at 0 dB. The `speedMultiplier` is 1.0 and the gain multiplier is 1.0.
2. The user presses Play on Deck A and hears the track at its original tempo and volume.
3. The user drags the pitch fader downward (toward the +% end, following CDJ convention where down = faster). The pitch percentage display updates in real time. At the +/-8% range, moving the fader to 75% of its travel shows +6.00%. The `speedMultiplier` updates to 1.06, and the DJ hears the track speed up immediately.
4. The user attempts to return the fader to center. As the fader enters the dead zone (+/-0.10% around center), it snaps to exactly 0.00%. The pitch display reads 0.00% and the `speedMultiplier` returns to 1.0. A subtle visual indicator (e.g., the center tick illuminates) confirms the fader is at true zero.
5. The user clicks the pitch range selector and changes from +/-8% to +/-4%. The fader's physical position remains unchanged but the percentage display updates to reflect the narrower range. If the fader was at +6.00% in the +/-8% range, switching to +/-4% clamps the value to +4.00% (the new range maximum) and the speedMultiplier adjusts accordingly.
6. The user wants to fine-tune the pitch. At the +/-4% range, the full fader travel covers only 4% of speed change, giving the DJ very precise control. Minor fader movements produce changes of ~0.01%.
7. The user switches to the +/-50% range for a dramatic speed change. The same fader now covers a 50% range in each direction, with coarser control per pixel.
8. The user clicks the pitch reset button. The fader animates smoothly back to center, the display shows 0.00%, and the `speedMultiplier` returns to 1.0.
9. The user rotates the gain knob clockwise on Deck A. The gain display updates from 0.0 dB upward. At full clockwise rotation, the gain reads +12.0 dB. The audio engine applies the corresponding linear multiplier, and the DJ hears the deck get louder.
10. The user rotates the gain knob fully counter-clockwise. The gain display shows -inf dB and the audio engine applies a gain multiplier of 0.0, fully muting the deck.
11. The user double-clicks the gain knob. The gain resets to 0 dB (unity gain).
12. The user loads a new track onto Deck A. Per PRD-0001, the pitch resets to 0% (speedMultiplier 1.0). The gain retains its current value (gain is deck-level state, not track-specific).
13. The user adds Deck C. The new deck initializes with pitch at 0%, range at +/-8%, and gain at 0 dB. All controls are independent of the other decks.

## 1.4. Acceptance Criteria

- [ ] Each deck displays a vertical pitch fader that controls the `speedMultiplier` value in the deck state tree.
- [ ] The pitch fader maps linearly within the selected range: `speedMultiplier = 1.0 + (pitchPercent / 100.0)`, where `pitchPercent` is positive when the fader moves toward "faster" and negative toward "slower."
- [ ] Four pitch ranges are selectable per deck: +/-4%, +/-8% (default), +/-16%, and +/-50%.
- [ ] Changing the pitch range preserves the current `speedMultiplier` value if it falls within the new range. If the current value exceeds the new range, the value is clamped to the new range boundary.
- [ ] The pitch percentage display shows 2 decimal places (e.g., +3.47%) at all ranges.
- [ ] A center dead zone of +/-0.10% exists around 0%. When the fader enters this zone, the value snaps to exactly 0.00% and the `speedMultiplier` snaps to exactly 1.0.
- [ ] The dead zone is purely a UI behavior: it quantizes the fader output, not the underlying state representation.
- [ ] A pitch reset button exists per deck. Clicking it sets the pitch to 0.00% and the `speedMultiplier` to 1.0. The fader animates to center over 150 ms.
- [ ] The pitch fader follows CDJ convention: fader down = faster (positive pitch %), fader up = slower (negative pitch %).
- [ ] The pitch fader supports both mouse drag and mouse wheel input. Mouse wheel adjusts pitch in increments of 0.05% at the +/-4% range, 0.10% at +/-8%, 0.20% at +/-16%, and 0.50% at +/-50%.
- [ ] Each deck displays a rotary gain knob that controls the per-deck gain multiplier in the deck state tree.
- [ ] The gain knob covers a range from -inf dB (linear 0.0) to +12 dB (linear ~3.981), with 0 dB (linear 1.0) as the default.
- [ ] The gain knob maps using the standard dB formula: `linearGain = pow(10, dB / 20.0)`. Values below -60 dB are treated as -inf (linear 0.0).
- [ ] The gain display shows the current value in dB with 1 decimal place (e.g., -3.2 dB), and shows "-inf" when the knob is at minimum.
- [ ] Double-clicking the gain knob resets the gain to 0 dB (unity).
- [ ] The gain knob supports mouse drag (circular/linear gesture) and mouse wheel input. Mouse wheel adjusts gain in 0.5 dB increments.
- [ ] The `speedMultiplier` is written to `std::atomic<float>` and read by the transport's `processBlock` with zero locks, zero allocations, and zero I/O.
- [ ] The gain multiplier is written to `std::atomic<float>` and read by the audio engine's `processBlock` with zero locks, zero allocations, and zero I/O.
- [ ] On track load, the pitch resets to 0% and the `speedMultiplier` resets to 1.0 (per PRD-0001). The gain value and pitch range selection persist unchanged.
- [ ] The pitch range selection persists as deck-level state (survives track load and eject).
- [ ] The pitch fader and gain knob are fully independent per deck: adjusting one deck's controls never affects another deck.
- [ ] The pitch fader renders as a vertical slider with a visible center tick mark and range labels at top and bottom (e.g., "-8%" at top, "+8%" at bottom).
- [ ] The gain knob renders as a rotary control with a start position (7 o'clock, -inf), a center-ish position (12 o'clock, 0 dB), and an end position (5 o'clock, +12 dB).
- [ ] The pitch fader and gain knob expose a value interface (normalized 0.0 to 1.0) suitable for future MIDI controller mapping. MIDI mapping itself is deferred.
- [ ] Without key-lock enabled (deferred to Time Stretching PRD), changing the pitch fader changes both tempo and musical pitch proportionally.
- [ ] All pitch and gain UI code resides under `Source/Features/Deck/` (UI components) with state integration in `Source/Features/Deck/`.

## 1.5. Grey Areas

### 1.5.1. Fader Direction Convention

CDJ-standard convention is "fader down = faster" which is counterintuitive to general slider UX (up = more). Adopting CDJ convention ensures muscle memory compatibility for professional DJs migrating from Pioneer hardware.

**Resolution:** Follow CDJ convention. Fader down = positive pitch = faster. This matches Traktor, Serato, and Rekordbox behavior. The fader's top label shows the negative range limit (e.g., "-8%") and the bottom label shows the positive limit (e.g., "+8%").

### 1.5.2. Dead Zone Size

A dead zone too large makes fine-tuning around 0% impossible. A dead zone too small means the DJ can never reliably hit exact zero. Pioneer CDJ uses approximately +/-0.10%, Traktor uses +/-0.05%.

**Resolution:** Use +/-0.10% as the dead zone, matching Pioneer CDJ behavior. This is wide enough to find zero reliably during a live performance but narrow enough that +/-0.10% of tempo deviation is inaudible. The dead zone is a UI quantization behavior only and does not prevent the underlying `speedMultiplier` from being set to values within the zone via future MIDI mapping or sync.

### 1.5.3. Pitch Range Change Behavior

When switching from a wider range (e.g., +/-16%) to a narrower range (e.g., +/-4%) while the pitch is set beyond the new range boundary (e.g., at +10%), the system must decide how to handle the out-of-range value.

**Resolution:** Clamp the pitch value to the new range boundary. If the DJ was at +10% and switches to +/-4%, the pitch clamps to +4.00%. The alternative (preserving the value and only updating when the fader moves) creates a confusing disconnect between the fader position and the audible pitch. Clamping provides immediate visual and audible consistency.

### 1.5.4. Gain Knob Taper

A linear dB mapping across the gain knob's rotation means the top half (+6 to +12 dB) is rarely used during normal operation since most level matching happens around 0 dB. Some competitors use an accelerated taper near the extremes.

**Resolution:** Use a linear dB-to-rotation mapping for simplicity and predictability. The +/-6 dB range around center covers the most common adjustment zone and occupies a proportional arc of the knob. DJs who need precise work around 0 dB can use the mouse wheel (0.5 dB steps). An accelerated taper adds complexity without clear user benefit at MVP.

### 1.5.5. Pitch Resolution at Wide Ranges

At the +/-50% range, the full fader travel covers 100 percentage points of speed change. On a 300-pixel tall fader, each pixel represents ~0.33% change, which is far coarser than the +/-4% range (~0.027% per pixel). This may feel imprecise for fine adjustments.

**Resolution:** Accept the coarser resolution at wide ranges as an inherent trade-off. DJs select +/-50% for dramatic tempo shifts (e.g., transition between genres), not for fine beatmatching. Mouse wheel input (0.50% per step at +/-50%) provides a finer alternative. Future enhancement: shift-drag modifier for 4x finer control at any range (deferred).

### 1.5.6. Gain Value on Deck Addition

When a new deck is added, the gain knob initializes at 0 dB. This is consistent with PRD-0001 deck initialization but worth calling out explicitly since some DJ software defaults new decks to -inf (muted) to prevent accidental loud audio.

**Resolution:** Default to 0 dB (unity gain). Sonik's audio engine (PRD-0002) outputs silence for decks with no track loaded, so a 0 dB default causes no unexpected audio. Defaulting to mute would create an extra step for the DJ every time a deck is added, which contradicts the goal of a fluid workflow.