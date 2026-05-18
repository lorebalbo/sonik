---
status: Not Implemented
epic: EPIC-0007
depends-on:
  - PRD-0005
  - PRD-0010
  - PRD-0052
  - PRD-0054
  - PRD-0055
  - PRD-0056
  - PRD-0057
  - PRD-0058
  - PRD-0059
---

# 1. PRD-0060: Channel Strip Organism and Mixer Shell

## 1.1. Problem

The Mixer Epic has, by the time this PRD is implemented, produced every individual building block of the mixer: the state schema (PRD-0052), the audio routing refactor (PRD-0053), the channel gain stage and channel fader DSP (PRD-0054), the 3-band EQ with per-band kill (PRD-0055), the per-channel filter (PRD-0056), the crossfader and A/B assignment (PRD-0057), the master output stage and metering (PRD-0058), and the four reusable UI atoms (PRD-0059: `MixRotaryKnob`, `MixKillButton`, `MixAssignButton`, `MixLevelMeter`). Despite all that, the DJ still cannot see or operate a mixer: no visible channel strip groups those atoms together, no master section is anywhere on screen, the crossfader has no on-screen surface, and the deck layout (PRD-0005) renders only the deck shell, with no spatial relationship to a mixing console.

In addition, PRD-0010 currently renders a per-deck gain knob inside the deck shell. The Mixer Epic (§1.3.5) declared that knob redundant with the mixer channel gain, and PRD-0054 has already migrated the underlying DSP and ValueTree binding to a single source of truth. The deck-shell gain knob is now visually orphaned: it manipulates the same parameter the mixer channel strip should expose, but it lives in the wrong feature slice and clutters the deck shell. Until a channel-strip organism replaces it, the user has two visual controls for the same parameter (after PRD-0054) or zero controls at all (if PRD-0010's knob is removed first), neither of which is acceptable.

Finally, the Mixer must compose into the existing adaptive deck grid (PRD-0005) without breaking the 1 / 2 / 3 / 4-deck layout rules already shipped, and it must do so deterministically — DJs must always know exactly where each channel strip and the master section sit on screen regardless of deck count.

## 1.2. Objective

The system provides a complete on-screen mixer shell composed from already-shipped atoms, such that:

- A `ChannelStrip` organism exists per deck, grouping (in fixed vertical order, top to bottom) the channel gain knob, the 3-band EQ knobs (HIGH / MID / LOW) each paired with its kill button, the filter knob, the post-fader stereo level meter, the A/B crossfader assign button pair, and the channel volume fader.
- A `MasterSection` organism exists once globally, exposing the master gain knob and the master stereo level meter.
- A `MixerComponent` organism wraps the master section and N channel strips, where N is the current deck count (1–4) reported by the deck layout manager (PRD-0005), and it reacts to deck addition / removal without restart.
- The mixer shell is integrated into the existing adaptive deck grid (PRD-0005) so the channel strip sits visually alongside its owning deck for every deck-count configuration, while the master section and the crossfader occupy fixed, predictable locations.
- The deck-shell gain knob defined by PRD-0010 is removed; the mixer channel-strip gain knob is now the sole on-screen control for the `mixer.channel.{A,B,C,D}.gain` ValueTree field. PRD-0010's pitch fader, pitch reset, and pitch range selector remain on the deck shell untouched.
- The crossfader is rendered as a single horizontal bar spanning the full width of the mixer area along its bottom edge, reusing the PRD-0010 pitch-fader atom configured with horizontal orientation and a 0.5 center detent.
- Every control in the channel strip, master section, and crossfader is bound to its `mixer.channel.*` / `mixer.master.*` / `mixer.crossfader` ValueTree path (PRD-0052) via JUCE `ValueTree::Listener` observers; no UI component polls the audio thread.
- The full mixer shell complies with `DESIGN.md` (monochrome `#2d2d2d` / `#fdfdfd`, Space Mono Regular, 2-px solid borders, zero border-radius, dithered patterns, pixel-art icons, tonal layering).

## 1.3. User Flow

1. The user launches Sonik with the default two-deck configuration. Deck A renders on the left half of the deck area; Deck B renders on the right half (per PRD-0005). Immediately to the right of Deck A's shell, the user sees Deck A's `ChannelStrip`: a vertical column containing, top to bottom, the gain knob, the three EQ knobs (HIGH / MID / LOW) each with a small kill button beside it, the filter knob, the post-fader stereo level meter, the A and B assign buttons, and finally the channel fader. Deck B's strip mirrors this layout on the right side of Deck B. The `MasterSection` (master gain knob and master stereo meter) sits in a fixed location at the top-right corner of the mixer area, outside the per-deck columns. A single horizontal `CrossfaderRail` spans the entire bottom of the mixer area.
2. The user rotates Deck A's EQ MID knob counter-clockwise. The knob's visual position updates immediately. The bound `mixer.channel.A.eq.mid` ValueTree property updates on the message thread. The audio thread, reading the parameter via its atomic snapshot (PRD-0055), attenuates the mid band; the user hears the mid scoop out smoothly with no zipper noise.
3. The user clicks Deck A's MID kill button. The button latches active (inverted fill, per `DESIGN.md`). `mixer.channel.A.eq.killMid` flips to `true`; the audio engine smoothly ramps the band to -inf dB (PRD-0055). Releasing the kill restores the previous MID knob value.
4. The user drags Deck A's channel fader downward. The fader moves; `mixer.channel.A.fader` updates; the audio engine reduces Deck A's contribution to the A/B bus (PRD-0054, PRD-0057). The Deck A level meter in the channel strip drops to match the new post-fader level (PRD-0058). The master meter also drops correspondingly.
5. The user clicks Deck A's `A` assign button. The button latches active; `mixer.channel.A.assignA` flips to `true`. The user clicks Deck B's `B` assign button. Both decks are now routed to their respective crossfader sides (PRD-0057).
6. The user drags the horizontal crossfader at the bottom of the mixer area from center fully to the right. The crossfader visual moves smoothly; `mixer.crossfader` writes 0.0 → 1.0 across the gesture; Deck A fades out, Deck B fades fully in. When the crossfader is released near the center, the configured 0.5 detent snaps the visual and the state value to exactly 0.5 (analogous to PRD-0010's pitch-fader center detent behaviour).
7. The user clicks "Add Deck" in the global toolbar. PRD-0005 transitions the deck grid from 2-column to 2 + 1 (Decks A and B on top, Deck C full-width on the bottom). `MixerComponent` observes the new deck count, instantiates a third `ChannelStrip` bound to `mixer.channel.C`, and lays it out alongside Deck C following the rule defined in §1.5.2. The master section and the crossfader bar do not move.
8. The user clicks "Add Deck" again. The grid becomes 2 × 2 (A/B top, C/D bottom). A fourth `ChannelStrip` is instantiated and bound to `mixer.channel.D`. Every channel strip retains the same internal layout and the same minimum width (see §1.5.7); if horizontal space is constrained, the layout fallback defined in §1.5.7 engages so the level-meter peak-hold ticks remain readable.
9. The user removes Deck D. `MixerComponent` destroys Deck D's `ChannelStrip` (the ValueTree subtree itself is owned by PRD-0052 / PRD-0005's deck lifecycle, not by the UI). The grid returns to 2 + 1.
10. The user moves the mouse to where Deck A's gain knob used to be inside the deck shell (per legacy PRD-0010). There is no knob there: only the pitch fader, the pitch reset button, and the pitch range selector remain on the deck shell. The gain control is now exclusively in the channel strip beside the deck.

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Mixer/Ui/Organisms/ChannelStrip.h/.cpp` exists and composes, in fixed top-to-bottom order, the channel gain knob, three EQ knobs (HIGH, MID, LOW) each paired with its kill button (PRD-0059 atoms `MixRotaryKnob` and `MixKillButton`), the filter knob, the stereo level meter (`MixLevelMeter`), the A and B crossfader assign buttons (`MixAssignButton`), and the channel volume fader (the PRD-0010 pitch-fader atom reused).
- [ ] `Source/Features/Mixer/Ui/Organisms/MasterSection.h/.cpp` exists and composes the master gain knob (`MixRotaryKnob`) and the master stereo level meter (`MixLevelMeter`).
- [ ] `Source/Features/Mixer/Ui/Organisms/MixerComponent.h/.cpp` exists, owns one `MasterSection` and N `ChannelStrip` instances, and reacts to deck addition / removal events from the existing deck layout manager (PRD-0005) to instantiate or destroy strips without restart.
- [ ] Every continuous knob in the channel strip and master section is two-way bound to its corresponding ValueTree path under `mixer.channel.{A,B,C,D}.*` or `mixer.master.*` (PRD-0052) via a `juce::ValueTree::Listener` registered on construction and removed on destruction.
- [ ] Every kill / assign button is two-way bound to its corresponding boolean ValueTree property and reflects external state changes (e.g. MIDI input via PRD-0061) without any polling timer.
- [ ] Every level meter subscribes to the read-only metering properties (`mixer.channel.{A,B,C,D}.levelPeakL|R`, `levelRmsL|R`, `clip` and the master equivalents) written by the audio engine via the lock-free metering bridge (PRD-0058); no level meter reads audio-thread state directly.
- [ ] No `ChannelStrip`, `MasterSection`, or `MixerComponent` method calls `processBlock`, `setValue` on any audio-thread object, or any other audio-thread API; all interaction with the engine is mediated through ValueTree writes on the message thread (PRD-0052).
- [ ] No audio-thread code path is added, removed, or modified by this PRD; all DSP migration relating to PRD-0010's gain knob is governed exclusively by PRD-0054.
- [ ] The audio-thread code paths read by the mixer UI (the metering bridge, the ValueTree-to-atomic snapshot publishers established in PRD-0052 through PRD-0058) continue to allocate no memory (no `new`, `delete`, `malloc`, `std::vector::push_back`, `std::string`), take no locks, perform no I/O, and communicate with the message thread only via `std::atomic` or `juce::AbstractFifo`.
- [ ] The deck-shell gain knob previously rendered by PRD-0010 inside `DeckShellComponent` is removed from the deck-shell view hierarchy; the deck shell continues to render PRD-0010's pitch fader, pitch reset button, and pitch range selector unchanged.
- [ ] The single on-screen control for `mixer.channel.{A,B,C,D}.gain` is the channel strip's gain knob.
- [ ] The crossfader is rendered as a single horizontal bar spanning the full width of the mixer area along its bottom edge, regardless of deck count.
- [ ] The crossfader is implemented by reusing the PRD-0010 pitch-fader atom, configured via an explicit orientation flag (horizontal) and an explicit detent value (0.5) — see §1.5.6 for the atom contract. No second fader atom is introduced.
- [ ] For 1 deck: the single deck occupies the upper area; its channel strip sits immediately to the right of the deck shell; the master section sits at the top-right outside the strip; the crossfader bar spans the bottom.
- [ ] For 2 decks: each deck's channel strip sits between the two deck shells, with strip A on the right edge of Deck A and strip B on the left edge of Deck B, so the two strips are visually adjacent at the centre of the screen (DDM4000-style); the master section sits at the top-right; the crossfader bar spans the bottom.
- [ ] For 3 decks: Decks A and B follow the 2-deck rule on the top row; Deck C on the bottom row carries its channel strip on its right side; the master section remains at top-right; the crossfader bar still spans the full bottom width.
- [ ] For 4 decks: each top-row strip sits on the inner edge of its deck (A right, B left) and each bottom-row strip sits on the inner edge of its deck (C right, D left), placing all four strips along the vertical centre line; the master section remains at top-right; the crossfader bar still spans the full bottom width.
- [ ] Every `ChannelStrip` instance respects a minimum width of 80 px; below that, the layout-fallback rule defined in §1.5.7 engages (level-meter peak-hold ticks are preserved, EQ kill buttons collapse from beside-knob to below-knob).
- [ ] All UI complies with `DESIGN.md`: strict monochrome `#2d2d2d` / `#fdfdfd` palette, Space Mono Regular typography, 2-px solid borders, zero border-radius, dithered patterns instead of gradients, pixel-art icons, tonal layering for depth.
- [ ] Rotary knob visuals derive from the Figma "Rotary Knob - MIX" component; the Figma asset fetch occurs during implementation (not during this PRD draft), and the atom (`MixRotaryKnob`, owned by PRD-0059) is the only knob class instantiated.
- [ ] No new UI atom is created by this PRD. Only existing atoms from PRD-0059 and the reused pitch-fader atom from PRD-0010 are used.
- [ ] No DSP block is introduced or modified by this PRD. No file under `Source/Features/Mixer/Dsp/` or `Source/Features/Mixer/Routing/` is added or modified by this PRD.
- [ ] No MIDI subsystem file is touched by this PRD; MIDI wiring is owned exclusively by PRD-0061.
- [ ] Adding or removing a deck while audio is playing causes no audible glitch and no UI-thread blocking longer than 16 ms.

## 1.5. Grey Areas

### 1.5.1. Channel-Strip Orientation: Vertical vs Horizontal

DJ hardware mixers universally lay out the channel signal chain vertically (gain at the top, fader at the bottom). A horizontal layout would save vertical space but break decades of muscle memory.

**Resolution:** Vertical orientation. The channel strip renders top-to-bottom in the fixed order specified in §1.4: gain → EQ HIGH (knob + kill) → EQ MID (knob + kill) → EQ LOW (knob + kill) → filter → level meter → A/B assign pair → channel fader. This matches Pioneer DJM, Allen & Heath Xone, and Behringer DDM4000, all of which the Mixer Epic explicitly targets for muscle-memory compatibility. The vertical fader at the bottom also gives the longest natural travel for fine fader work, which is the most-used continuous control on the strip.

### 1.5.2. Channel-Strip Placement Relative to Its Deck

The channel strip could sit on the right of the deck, on the left, inside the deck shell, or in a separate mixer pane below all decks. Each choice has consequences for the adaptive grid (PRD-0005).

**Resolution:** The channel strip sits alongside its deck, on the inner edge facing the other deck on the same row. Specifically: for any row containing two decks (rows of 2 in the 2-, 3-, and 4-deck layouts), the left deck's strip sits on its right edge and the right deck's strip sits on its left edge, placing both strips adjacent at the vertical centre line of the row — exactly mirroring the physical layout of a two-channel hardware mixer between two CDJs. For any row containing a single deck (1-deck layout, and the bottom row of the 3-deck layout), the strip sits on the right edge of the deck. This rule is deterministic across all four supported deck counts, keeps the strips at the physical "centre" of the user's gaze (where their hands naturally rest), and avoids embedding the strip inside the deck shell — which would couple the deck feature slice to the mixer feature slice and violate the Feature-Sliced Design boundary declared in EPIC-0007 §1.3.3. It also avoids a bottom mixer pane, which would force the user's eyes to dart between deck transport (top) and channel controls (bottom) during fast EQ work.

### 1.5.3. Master-Section Placement

The master section is global, used infrequently relative to channel controls, and must never move when the deck count changes.

**Resolution:** Top-right corner of the mixer area, in a fixed-size panel that is independent of the adaptive deck grid. Rationale: it must be reachable but never compete with channel strips for prime real estate; it must remain in a constant location so muscle memory works at any deck count; and the top-right of the screen is the location DJs already associate with master output on most software (Traktor, Serato, Rekordbox) and is far from the crossfader at the bottom, avoiding accidental drag conflicts. The master section's width and height are fixed and do not scale with deck count.

### 1.5.4. Crossfader Placement

Locked by the task: a single horizontal bar spanning the full width of the mixer area along its bottom edge, regardless of deck count.

**Resolution:** Confirmed and locked. Justification: a full-width bottom bar makes the crossfader equidistant from every deck, which matters in 3- and 4-deck configurations where any deck pair can be assigned to A/B (PRD-0057). It also matches the physical position of the crossfader on every standard DJ mixer (centred at the bottom of the front panel), reinforcing muscle-memory compatibility. The crossfader's vertical extent is fixed (one bar height); its horizontal extent scales with the mixer area's current width.

### 1.5.5. Channel-Strip Uniformity Across Deck Counts

Whether the channel strip should look or behave differently when only one deck is present versus four.

**Resolution:** Identical for all deck counts. The channel strip is intrinsic to the deck; the mixer shell only composes them. Internal layout, control order, control sizes (above the minimum-width threshold), and bindings are the same whether one deck or four are visible. Only the position of the strip on screen changes (per §1.5.2). This guarantees that a DJ trained on the two-deck setup needs no relearning when going to four decks.

### 1.5.6. Pitch-Fader Atom Reuse for the Crossfader: Orientation and Detent Contract

PRD-0010's pitch fader atom was designed as a vertical fader with a value range of -100 to +100 percent, a center dead zone of ±0.10 %, and a CDJ-convention "down = faster" mapping. The crossfader needs a horizontal fader with a normalized 0.0–1.0 range and a 0.5 detent. We must either fork the atom or generalise it. Forking would create the "second fader atom" the Epic forbids in §1.3.4.

**Resolution:** Generalise the existing PRD-0010 fader atom by extending its public constructor / setter contract with two new explicit parameters:
- `Orientation orientation` — enum with values `Vertical` (default, preserves PRD-0010 behaviour) and `Horizontal`. In `Horizontal` mode, drag along the X axis controls the value; in `Vertical` mode, drag along the Y axis controls the value with PRD-0010's "down = positive" inversion preserved.
- `std::optional<float> detentValue` — when set, the fader snaps to this value within a small configurable dead zone (default ±1 % of total range). PRD-0010's existing centre detent at 0 % becomes a special case where `detentValue = 0.0f` and the value range is interpreted as percent. For the crossfader, `detentValue = 0.5f` and the value range is the normalized `[0.0, 1.0]`. When `detentValue` is `std::nullopt`, no detent is applied.

The atom's default-constructed behaviour and rendering remain identical to PRD-0010, so existing call sites (the per-deck pitch faders, the new channel volume faders) are unaffected. The crossfader call site instantiates the atom with `Orientation::Horizontal` and `detentValue = 0.5f`. The generalisation is purely additive and lives inside the same atom class — no new class, no fork. This satisfies the Epic's "no second fader atom" rule and gives the crossfader the snap-to-centre behaviour DJs expect.

### 1.5.7. Level-Meter Legibility Across Deck Counts

At 4 decks on a small display the channel strip can become narrow enough that the level-meter peak-hold ticks compress to a single pixel column and the EQ kill buttons collide with the EQ knobs. Below some threshold the strip is unusable.

**Resolution:** Each `ChannelStrip` enforces a minimum width of 80 px. Within that 80 px, the level meter reserves a minimum of 24 px in width so its two stereo bars (left + right, each ≥ 8 px wide) and a 4-px peak-hold tick gutter remain visible and legible. If the mixer area is too narrow to host every channel strip at 80 px alongside the master section and the deck shells, the layout fallback engages in this order:
1. Collapse the EQ kill buttons from "beside the knob" to "below the knob" (saves ~16 px per strip).
2. Collapse the A/B assign pair from "side-by-side" to "stacked".
3. Stop honouring the minimum: scale strips down uniformly until they fit, but the layout flags a non-fatal warning to the log on the message thread (not the audio thread) so the issue is observable in QA.
The 80-px figure is calibrated for a 1280-px-wide window at 4 decks: 4 × 80 px = 320 px of strips, leaving ~960 px for deck shells, the master section, and gutters, which fits the adaptive 2×2 grid of PRD-0005. Larger windows scale strips beyond 80 px proportionally; smaller windows trigger the fallback.

### 1.5.8. ChannelStrip Ownership of Its ValueTree Subtree

The Mixer ValueTree schema (PRD-0052) defines `mixer.channel.{A,B,C,D}` subtrees, but the deck lifecycle (PRD-0005) creates and destroys decks dynamically. When Deck C is added, who creates the `mixer.channel.C` subtree, and what does the `ChannelStrip` do if its subtree doesn't yet exist?

**Resolution:** PRD-0052 owns the lifecycle of the `mixer.channel.*` subtrees and is responsible for creating them when a deck is added and removing them when a deck is removed. `MixerComponent` listens for deck-addition events from the deck layout manager (PRD-0005) and, on the message thread, instantiates a new `ChannelStrip` bound to the freshly-created subtree. `ChannelStrip` assumes the subtree exists at construction time; if it does not, construction is an assertion failure (programmer error, not a runtime condition). This keeps `ChannelStrip` simple and defect-localising: any subtree-lifecycle bug surfaces in PRD-0052's domain, not in the UI.
