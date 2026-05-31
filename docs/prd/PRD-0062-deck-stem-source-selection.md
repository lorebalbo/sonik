---
status: Implemented
epic: EPIC-0002
depends-on:
  - PRD-0002
  - PRD-0021
  - PRD-0022
  - PRD-0023
---

# 1. PRD-0062: Deck Stem-Source Selection (Original vs Separated Stems)

## 1.1. Problem

By the time this PRD is reached, EPIC-0002 has delivered a complete stem pipeline: ONNX inference (PRD-0019), the separation engine (PRD-0020), stem-aware playback (PRD-0021), stem-aware time stretching (PRD-0022), and the stem UI with its "SEPARATE STEMS", "VOC", and "INST" controls (PRD-0023). But there is a structural gap. Once a track is separated, the deck's `processBlock` reads from the four stem buffers whenever `DeckAudioSource::stemsActive` is true, and the "INST" path the DJ hears is the *sum of the separated stems* — drums + bass + other, plus vocals when "VOC" is unmuted. Demucs separation, however excellent, introduces subtle spectral artifacts. There is currently no way for the DJ to say "play the pristine original file instead", short of clearing the stems entirely and losing the cached buffers.

The toggle that PRD-0021 exposes is `stemsActive`, an internal consequence of *whether stems exist*, not a deliberate user choice. `AudioEngine::setDeckStemBuffers` flips it to `true` the moment separation completes; `clearDeckStemBuffers` flips it back to `false`. There is no ValueTree property representing the DJ's intent, no UI control surfacing it, and — critically — no stable, observable signal that downstream consumers can read. EPIC-0008's in-app DAW (§1.3.3) projects each deck onto a *group of three lanes* — `Original`, `Instrumental`, `Vocal` — and must know, at any instant, which source mode a deck is producing audio from so it can grow the live clip onto the correct lane(s). That signal does not exist yet.

Finally, EPIC-0002 §3.4/§3.5 originally specified that the original full-track buffer is *released* once stems are verified (to cap deck memory at ~208 MB). That reconciliation no longer holds: if Original is a first-class, instantly-switchable playback mode, the original buffer must be *retained* alongside the stem buffers so the audio thread can crossfade between them click-free. This PRD introduces the user-facing toggle, the `sourceMode` ValueTree property as the single source of truth, the retained-original-buffer memory contract, and the published source-mode reader that EPIC-0008 consumes.

## 1.2. Objective

The system gives each deck an explicit, always-available source-mode choice such that:

- A new tactile toggle button, placed immediately to the **left** of the "SEPARATE STEMS" button on each deck, lets the DJ switch the deck between playing the **Original** source file ("ORIG") and playing the **separated stems** ("STEMS"). The toggle conforms to DESIGN.md §Tactile Buttons: `2px solid #2d2d2d` border at all times, inverted fill on the active state (`#2d2d2d` fill / `#fdfdfd` text), zero `border-radius`, instant state change with no animation.
- The choice is independent of the "VOC" / "INST" mute toggles. Even when both stems would be audible, selecting "ORIG" plays the artifact-free original file rather than the summed stems. Source mode is a deliberate, persistent choice, not a side effect of mute state.
- A new deck ValueTree property `sourceMode` (enum string: `"original"` | `"stems"`) is the single source of truth, owned and mutated exclusively on the message thread. The existing audio-thread atomic flag (`DeckAudioSource::stemsActive`) becomes *derived* from `sourceMode` rather than from buffer presence: the message thread publishes `stemsActive = (sourceMode == "stems")`.
- The audio thread switches buffers click-free using the existing 64-sample crossfade convention (the same ramp PRD-0021 uses for stem mute/unmute). No allocations, locks, or I/O occur on the audio thread; the switch is driven by an atomic flag read plus a ramp.
- After separation completes, **both** the original buffer (`channelL`/`channelR`) and the four stem buffers are retained in memory so switching is instantaneous. EPIC-0002 §3.5's "release the original buffer" budgeting is revised by this PRD (see §1.5.2).
- When a track has no stems yet (never separated, or separation in progress), the toggle is locked to "ORIG": the "STEMS" option is disabled (rendered in the standard disabled treatment) and cannot be selected until stem buffers are ready.
- The active source mode is **published for the DAW**: a stable, observable accessor (the `sourceMode` ValueTree property, mirrored by a lock-free `SourceModeReader` that EPIC-0008's `LiveProjectionTimer` reads on the message thread) exposes which lane(s) the live projection draws onto, per the mapping contract in §1.3.

## 1.3. User Flow

The deck source-mode toggle and its consumers behave as follows.

1. A track is loaded into a deck. No stems exist. The source-mode toggle renders as "ORIG", active and locked; the "STEMS" half is disabled. `sourceMode = "original"`. The published source mode reads `Original`.
2. The DJ clicks "SEPARATE STEMS" (PRD-0023). Separation runs in the background; playback continues from the original buffer. The source-mode toggle remains locked to "ORIG" while separation is in progress.
3. Separation completes. `AudioEngine::setDeckStemBuffers` publishes the four stem buffers *and retains* the original buffer. The "STEMS" option becomes enabled. Per the default-mode resolution (§1.5.3), `sourceMode` stays `"original"` — the deck keeps playing the pristine file; the DJ opts in to stems deliberately.
4. The DJ clicks the toggle to "STEMS". The message thread sets `sourceMode = "stems"` in the ValueTree and publishes `stemsActive = true`. On the next `processBlock`, the audio thread observes the flag change and crossfades (64 samples) from the original buffer to the summed active stems. The "VOC" / "INST" mute toggles now take effect.
5. The DJ clicks the toggle back to "ORIG". The message thread sets `sourceMode = "original"`, publishes `stemsActive = false`, and the audio thread crossfades back to the original buffer. The "VOC" / "INST" toggles are now ignored and rendered greyed (§1.5.4); their stored mute state is preserved for when the DJ returns to "STEMS".
6. Throughout, EPIC-0008's `LiveProjectionTimer` polls the published source mode (via `SourceModeReader`) on the message thread and grows the deck's live clip onto the lane(s) given by the mapping contract:

```text
Deck source state                         → Published lanes
├─ sourceMode == "original"               → Original
├─ sourceMode == "stems", VOC + INST on   → Instrumental + Vocal
├─ sourceMode == "stems", VOC muted       → Instrumental
└─ sourceMode == "stems", INST muted      → Vocal
```

7. If key-lock (PRD-0022) is active when the DJ switches mode, the stretcher set is reconciled on the message thread (§1.5.6): "original" uses a single stretcher over the original buffer; "stems" uses one stretcher per active stem. The reconfiguration publishes the new stretcher set via atomic pointer before `stemsActive` flips, so the audio thread never reads a half-built stretcher set.

## 1.4. Acceptance Criteria

- [ ] Each deck exposes a `sourceMode` ValueTree property (enum string, values exactly `"original"` and `"stems"`, default `"original"`), owned and mutated only on the message thread.
- [ ] `DeckAudioSource::stemsActive` is derived from `sourceMode`: the message thread sets `stemsActive.store(sourceMode == "stems", std::memory_order_release)` and the audio thread reads it with `std::memory_order_acquire`. Buffer presence alone no longer flips `stemsActive`.
- [ ] A tactile source-mode toggle button is rendered to the **left** of the "SEPARATE STEMS" button on every deck, labelled "ORIG" / "STEMS", conforming to DESIGN.md: `2px solid #2d2d2d` border in all states, active-fill inversion (`#2d2d2d`/`#fdfdfd`), `0px` border-radius, instant (no animation).
- [ ] When no stem buffers exist for the deck (never separated, or separation in progress), the "STEMS" option is disabled and `sourceMode` is locked to `"original"`; attempting to select "STEMS" is a no-op.
- [ ] When stem buffers exist, clicking the toggle flips `sourceMode` between `"original"` and `"stems"` and the audio output changes accordingly within one buffer, via a 64-sample crossfade, with no audible click.
- [ ] Source mode is independent of the "VOC" / "INST" mute toggles: with both stems unmuted and `sourceMode == "original"`, the deck outputs the original buffer (not the summed stems); with `sourceMode == "stems"`, the mute toggles take effect.
- [ ] After separation completes, both the original buffer and the four stem buffers are retained in memory; switching from "STEMS" to "ORIG" and back produces audio immediately with no reload from disk and no allocation.
- [ ] `AudioEngine::setDeckStemBuffers` no longer releases the original buffer; `clearDeckStemBuffers` releases the stem buffers and forces `sourceMode` back to `"original"`.
- [ ] In `"original"` mode the "VOC" / "INST" toggles are rendered greyed/disabled, and their stored mute state is preserved so it is restored when the DJ returns to `"stems"`.
- [ ] A published `SourceModeReader` exposes the deck's active source mode to consumers, mirroring the `sourceMode` ValueTree property; it is safe to read from the message thread by EPIC-0008's `LiveProjectionTimer`.
- [ ] The published source-mode → lane mapping is exactly: `original` → `Original`; `stems` with both stems audible → `Instrumental` + `Vocal`; `stems` with one stem muted → only the audible stem lane (Instrumental if VOC muted, Vocal if INST muted).
- [ ] Switching source mode while key-lock (PRD-0022) is active reconciles the stretcher set on the message thread (single stretcher for "original", one per active stem for "stems") and publishes the new set via atomic pointer **before** `stemsActive` flips, so the audio thread never reads a partially-constructed stretcher set.
- [ ] No audio-thread allocation, lock, or I/O is introduced. The mode switch is driven exclusively by an `std::atomic` flag read plus a crossfade ramp; all buffer retention and stretcher-set swaps occur on the message thread via atomic pointer publication.
- [ ] A test under `Tests/` (e.g. `DeckSourceModeTests.cpp`) verifies: the derived `stemsActive` flag, the locked-to-original behaviour when no stems exist, mode-switch independence from mute state, click-free crossfade on switch, and the published lane mapping for each of the four mapping rows.

## 1.5. Grey Areas

### 1.5.1. "Instrumental" Lane Semantics and Mute Interaction

The published mapping says `stems` mode with both stems audible maps to the `Instrumental` + `Vocal` lanes. But "Instrumental" in this app is a *derived* sum (drums + bass + other), and the "INST" toggle mutes that sum as a unit. The question is whether the published lane state should reflect the *sum's* audibility or the underlying four-stem buffer state, and how a future 4-stem UI would extend it.

**Resolution:** The published lane state reflects the **two-lane MVP model** that EPIC-0008 §1.3.3 consumes: `Instrumental` is the summed instrumental stem (drums + bass + other), and `Vocal` is the vocal stem. The "INST" mute toggle governs `Instrumental` lane audibility as a unit; the "VOC" mute toggle governs `Vocal`. The published `SourceModeReader` reports lane audibility derived from `sourceMode` plus the two mute booleans, nothing finer-grained. A future 4-stem Epic that exposes drums/bass/other independently will extend the lane model and the reader together; until then, the contract is deliberately two-stem so the DAW's three-lane layout (`Original` / `Instrumental` / `Vocal`) has an exact, stable source. This PRD does not attempt to future-proof the lane enumeration beyond what EPIC-0008 reads today.

### 1.5.2. Retaining the Original Buffer (Revising EPIC-0002 §3.5)

EPIC-0002 §3.4/§3.5 specified releasing the original buffer once stems are verified, capping a separated deck at ~208 MB (four stem buffers). Making Original a switchable mode requires retaining the original buffer (~52 MB) alongside the stems, raising the worst-case per-deck footprint to ~260 MB. This contradicts the original budgeting.

**Resolution:** Retain the original buffer. The §3.5 "release after activation" rule is superseded for any deck whose stems are present, because the whole point of this PRD is instant, click-free switching between original and stems — impossible if the original buffer is gone (reloading from cache would allocate and block). The revised per-deck worst case is ~260 MB (one original + four stems, 5-minute stereo @ 44.1 kHz). With the typical two-deck setup this is ~520 MB, well within budget on the target Apple Silicon hardware. `clearDeckStemBuffers` (track unload or explicit clear) still releases the stem buffers immediately; only the original buffer survives as long as the track is loaded, exactly as it did before separation. This PRD updates EPIC-0002 §3.5's memory contract accordingly.

### 1.5.3. Default Mode After Separation Completes

When separation finishes, the deck could either stay in "original" mode (DJ must opt in to stems) or auto-switch to "stems" (immediately demonstrating the result). Auto-switching is more visibly rewarding; staying put is less surprising.

**Resolution:** Stay in "original" mode. Auto-switching the audio source mid-performance — potentially mid-phrase, with audible artifacts replacing a pristine file — is exactly the kind of surprise a DJ tool must avoid. The DJ requested *separation*, not a *playback change*; conflating the two risks an unexpected sonic shift during a live set. The "STEMS" option simply becomes enabled, and the DJ opts in when ready. This also keeps behaviour consistent regardless of whether stems were freshly computed or loaded instantly from cache (PRD-0023's cache-hit path): in both cases the deck stays on "original" until the DJ chooses otherwise.

### 1.5.4. VOC / INST Toggles While in Original Mode

In "original" mode the source is the undivided file, so per-stem mute is meaningless. The "VOC" / "INST" toggles could remain live (but inert), be hidden, or be greyed. Their stored state also needs a defined fate across mode switches.

**Resolution:** Grey (disable) the "VOC" / "INST" toggles while `sourceMode == "original"`, and **preserve** their stored mute booleans. Greying communicates honestly that per-stem control does not apply to an unsplit source, without destroying the DJ's prior mute selections. When the DJ returns to "stems", the toggles re-enable in exactly the state they were left, so a "vocals muted, instrumental playing" setup survives a round-trip through "original" mode. Hiding the toggles would cause a jarring layout reflow on every mode switch; leaving them live but inert would invite confusion ("I pressed VOC and nothing happened"). Greying with state preservation is the least-surprise option.

### 1.5.5. Click-Free Switch Ramp Length

Switching buffers mid-playback risks a discontinuity at the splice point. A crossfade is required, but its length must be chosen: too short clicks, too long smears transients.

**Resolution:** Reuse the existing **64-sample crossfade** that PRD-0021 already uses for stem mute/unmute and that EPIC-0002 §3.1 establishes as the deck-wide fade convention. A 64-sample ramp (~1.5 ms @ 44.1 kHz) is short enough to be imperceptible as a "fade" yet long enough to eliminate the click from a hard buffer splice. Introducing a new, longer ramp specifically for source-mode switching would be inconsistent with the rest of the deck and provides no benefit: the original buffer and the summed stems are time-aligned (both indexed by the same `playheadPosition` source sample), so the crossfade bridges two phase-coherent signals and 64 samples is ample. The switch ramp is implemented identically to the existing mute ramp: an audio-thread gain ramp between the outgoing and incoming source, no allocation.

### 1.5.6. Mode Switch Under Active Key-Lock / Time-Stretch

PRD-0022 creates one `RubberBandStretcher` for the original buffer and one per active stem for stems. Switching mode while key-lock is engaged means destroying one stretcher topology and building the other — an allocation that must not happen on the audio thread, and a swap that must not expose a half-built set.

**Resolution:** Reconcile the stretcher set on the **message thread**, publishing the new set via atomic pointer (the PRD-0011 / PRD-0022 pattern) **before** flipping `stemsActive`. The sequence is: (1) message thread builds the target stretcher set for the incoming mode off to the side; (2) publishes it via atomic pointer release; (3) only then sets `stemsActive` to the new value. The audio thread reads the stretcher pointer and `stemsActive` with acquire semantics, so it always sees a fully-built stretcher set matching the active mode. The outgoing stretcher set is retired on the message thread after the audio thread has demonstrably moved past it (same deferred-deletion mechanism PRD-0022 already uses). When key-lock is *off*, no stretchers exist and the switch is a plain buffer crossfade. This adds no audio-thread allocation and reuses the established lazy-create / atomic-publish / deferred-delete machinery.

### 1.5.7. Where the DAW Reads Source Mode, and Future MIDI-Mappability

EPIC-0008 needs a reliable read of the active source mode. It could read the `sourceMode` ValueTree property directly, or a separate atomic could be the canonical source. Two sources of truth risk divergence. Separately, the toggle is currently mouse-only; DJs will eventually want it on a controller.

**Resolution:** The `sourceMode` **ValueTree property is the single source of truth**; the `SourceModeReader` is a thin, read-only mirror published for consumers that should not (or cannot conveniently) attach a ValueTree listener. EPIC-0008's `LiveProjectionTimer` runs on the message thread, so it may read either, but the canonical, normative value is always the ValueTree property — the reader is updated *from* it, never the reverse. This keeps one writer (the message thread, on user toggle) and avoids the divergence that two independent stores would invite. On MIDI: the toggle is intentionally **mouse-only in this PRD**, but it is designed to be mappable later — when a future Epic registers a `deck.{A,B,…}.sourceMode` target in PRD-0042's control target registry, a controller binding can flip the same ValueTree property through the existing inbound-routing path, with no change to the audio-thread contract or the published reader. This is called out as future scope, not delivered here.
