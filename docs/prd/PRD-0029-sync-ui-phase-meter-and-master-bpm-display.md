---
status: Done
epic: EPIC-0003
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0026
  - PRD-0027
  - PRD-0028
---

# 1. PRD-0029: Sync UI — Phase Meter and Master BPM Display

## 1.1. Problem

Once SYNC is engaged, the DJ has no visual confirmation that the beatgrids are actually locked. Without a phase indicator, detecting micro-phase drift requires careful listening for flamming artifacts — a task that is unreliable in a loud club environment, with headphones partially off, or at low monitor volume. A drift of even 0.05 beats is aurally imperceptible on many genres yet is enough to degrade the mix. The DJ is flying blind with respect to the precision of the lock.

Beyond phase, the DJ also lacks a single authoritative tempo reference. When working across three or four decks, reading individual BPM values per deck does not tell the DJ what the global master clock is driving. The absence of a mixer-level master BPM readout forces the DJ to infer the master tempo from deck-level displays, which is cognitively expensive during a live transition and creates hesitation when judging whether to push or pull a slave deck.

Finally, MASTER and SYNC are binary state controls whose active and inactive states currently provide no distinct visual differentiation. Without a clear, immediate visual treatment, the DJ cannot confirm at a glance which deck owns the master clock and which decks are locked to it.

## 1.2. Objective

The system provides three integrated visual indicators that make sync state immediately readable without any auditory judgment:
- A `PhaseMeterComponent` per synced deck: a narrow horizontal bar whose indicator position encodes the real-time phase offset between the slave deck and the master clock, allowing the DJ to confirm lock or diagnose drift at a glance.
- A master BPM display in the mixer header area: a single authoritative readout showing the current master clock tempo to two decimal places, always visible during a mix so the DJ never has to infer global tempo from individual deck readings.
- Unambiguous MASTER and SYNC button visual states: binary inversion treatment (no transition, no fade) so active and inactive states read clearly under stage lighting and in peripheral vision.

## 1.3. User Flow

1. The DJ launches Sonik with two decks loaded. Neither deck has SYNC engaged. The phase meter area on each deck is invisible (not rendered). The master BPM readout in the mixer header shows `---.--`.
2. The DJ presses MASTER on Deck A. The MASTER button on Deck A inverts: black background, white label `MASTER`. All other deck MASTER buttons retain their default state (white background, black label). The master BPM readout updates to Deck A's BPM (e.g., `128.00`).
3. The DJ presses SYNC on Deck B. The SYNC button on Deck B inverts: black background, white label `SYNC`. The phase meter bar becomes visible below or adjacent to the waveform on Deck B. At this moment the slave may have non-zero phase offset; the indicator tick mark sits off-centre, towards the left or right of the bar.
4. The phase correction engine (PRD-0028) begins nudging Deck B's tempo. The indicator moves towards centre. Within one bar the indicator reaches the centre zone. When `|phaseOffset| < 0.02 beats` the phase meter enters the locked state: the entire bar fills solid black, replacing the tick mark with a full-fill indicator signalling confirmed lock.
5. During normal playback with SYNC engaged and phase locked, the filled bar holds. Minor drift causes the indicator to momentarily leave the locked state and return to tick-mark mode, and then reconverge to the locked fill.
6. The DJ reads the master BPM readout in the mixer header at any time without looking at the individual deck BPM. The value updates in real time as the master deck's BPM is defined (it reflects the `masterBPM` from the current `MasterClockSnapshot`).
7. The DJ presses SYNC on Deck B again to disengage it. The SYNC button reverts to its default state. The phase meter bar becomes invisible.
8. The master deck (Deck A) is ejected. The `MasterClockManager` auto-promotes the next playing deck or enters dormant state. If no deck is playing, the master BPM readout returns to `---.--`. The MASTER button on the promoted deck (if any) inverts.
9. The DJ presses MASTER on Deck B (manual override). Deck A's MASTER button immediately reverts to default state. Deck B's MASTER button immediately inverts. The master BPM readout reflects Deck B's BPM.

### 1.3.1. Edge Cases

- If Deck B (slave) is the only deck with SYNC engaged and its own playback is stopped, the phase meter becomes invisible (same treatment as SYNC disengaged).
- If no deck has been designated MASTER (dormant clock state, `masterDeckIndex = -1`), the master BPM readout shows `---.--` and pressing SYNC on any deck shows the phase meter bar but the indicator sits at centre (phase offset reads 0.0 because there is no master phase to compare against).
- If the master deck itself is pressed for SYNC, SYNC is silently ignored or disabled for the master deck. The phase meter on the master deck is never shown (the master deck has no phase offset relative to itself).
- If a track is loaded onto a slave deck mid-sync (track replacement), SYNC is automatically disengaged on that deck as part of the track-load invariant (PRD-0001), the phase meter becomes invisible, and the SYNC button reverts to default state.
- During a master handoff (the window when `MasterClockManager` is promoting a new master deck), the master BPM readout continues to display the last stable snapshot BPM rather than flickering to `---.--`. The transition completes in under one message-thread cycle (~16 ms) and is imperceptible to the DJ.
- If a slave deck is paused while SYNC is engaged, the phase meter remains visible but the indicator freezes at the last published `phaseOffset` value. Phase correction resumes the moment playback restarts.

## 1.4. Acceptance Criteria

### 1.4.1. PhaseMeterComponent

- [ ] `PhaseMeterComponent` is a narrow horizontal bar component residing in `Source/Features/Sync/UI/PhaseMeterComponent.h` and `PhaseMeterComponent.cpp`.
- [ ] The bar background renders in `#f9f9f9` (surface) with a solid `#000000` (primary) 1-pixel border. Zero border-radius. No gradients.
- [ ] A vertically centred tick mark (a narrow solid `#000000` rectangle, full bar height, 2–3 px wide) serves as the phase indicator. Its horizontal position is computed as `x = (phaseOffset + 0.5) * barWidth`, mapping the range `[−0.5, +0.5]` beats linearly across the full bar width. At `phaseOffset = 0.0` the tick sits exactly at the horizontal midpoint.
- [ ] A 1-pixel vertical centre line in `#000000` is always drawn at the horizontal midpoint of the bar as a visual reference for zero phase.
- [ ] When `|phaseOffset| < convergenceThreshold` (0.02 beats): the entire bar interior fills solid `#000000` (locked state). The tick mark is not separately drawn in this state. No transition or animation — the fill switches instantly.
- [ ] When `|phaseOffset| >= convergenceThreshold`: the bar background is `#f9f9f9`, the centre line and tick mark are drawn as described above. No intermediate or gradiated fill. No dithered pattern for the partial-fill case.
- [ ] `PhaseMeterComponent` is visible only when the deck's `isSynced` flag is `true` AND the deck is not the master deck. In all other states the component is hidden (`setVisible(false)`), occupying no visual space in the layout (the parent layout collapses the gap).
- [ ] `PhaseMeterComponent` polls `std::atomic<float> phaseOffset` (published by `DeckAudioSource::processBlock`) on a `juce::Timer` firing at 60 Hz (16.7 ms interval). Each tick calls `repaint()` if the newly read value differs from the last-rendered value by more than 0.001 beats (floating-point noise filter).
- [ ] The timer starts when `isSynced` becomes `true` and stops when `isSynced` becomes `false`. The component stops polling while invisible to avoid unnecessary CPU use.
- [ ] Polling uses only `std::atomic<float>::load(std::memory_order_relaxed)` — no locks, no blocking, no allocation on the message thread timer callback.
- [ ] `PhaseMeterComponent` observes `IDs::isSynced` and `IDs::isMaster` on the deck ValueTree via the JUCE Listener pattern to toggle visibility and start/stop the timer.

### 1.4.2. Master BPM Display

- [ ] The master BPM display is a read-only label component positioned in the mixer header area, always visible when the application is running.
- [ ] When the master clock is active (`masterDeckIndex != -1`), the label renders the current `masterBPM` value formatted to exactly two decimal places (e.g., `128.00`, `174.32`).
- [ ] When the master clock is dormant (`masterDeckIndex = -1`), the label renders the string `---.--`.
- [ ] The label uses the `spaceGrotesk` typeface at a display scale no smaller than 3.5 rem equivalent, bold weight, all-caps style consistent with DESIGN.md typography rules for BPM readouts.
- [ ] Label text color is `#000000` on `#f9f9f9` surface background. Zero border-radius. No gradients, no drop shadows.
- [ ] The master BPM display polls the `MasterClockSnapshot` (or an equivalent `std::atomic<double>` master BPM publication) on the same 60 Hz `juce::Timer` as `PhaseMeterComponent`, or on a dedicated timer of identical rate. The two may share a single timer instance if architecturally convenient, but sharing is not required.
- [ ] BPM value changes of less than 0.005 BPM between two consecutive polls do not trigger a repaint (noise filter), preventing flicker from floating-point jitter.
- [ ] The display updates within one timer cycle (≤ 17 ms) of any change to the master BPM.
- [ ] The component is implemented in `Source/Features/Sync/UI/` and does not reside in deck-scoped feature folders.

### 1.4.3. MASTER Button Visual States

- [ ] MASTER button inactive state: white (`#f9f9f9`) background, black (`#000000`) label text, 1-pixel solid black border. Zero border-radius.
- [ ] MASTER button active state: black (`#000000`) background, white (`#f9f9f9`) label text, 1-pixel solid black border. Zero border-radius.
- [ ] The state change between inactive and active is instantaneous (no CSS-style transition, no alpha fade, no animation of any kind).
- [ ] Exactly one MASTER button across all decks is in the active state at any time, or zero (dormant). Pressing an active MASTER button does not deactivate it (the master must always have an owner once assigned, unless the auto-demotion path triggers on stop/eject).
- [ ] MASTER button is implemented in `Source/Features/Sync/UI/MasterButton.h` and `MasterButton.cpp`, observing `IDs::isMaster` on the deck ValueTree.

### 1.4.4. SYNC Button Visual States

- [ ] SYNC button inactive state: white (`#f9f9f9`) background, black (`#000000`) label text, 1-pixel solid black border. Zero border-radius.
- [ ] SYNC button active state: black (`#000000`) background, white (`#f9f9f9`) label text, 1-pixel solid black border. Zero border-radius.
- [ ] The state change between inactive and active is instantaneous. No transitions.
- [ ] SYNC button is implemented in `Source/Features/Sync/UI/SyncButton.h` and `SyncButton.cpp`, observing `IDs::isSynced` on the deck ValueTree.
- [ ] Pressing the SYNC button on the master deck has no effect. The button remains in its inactive state and does not invert.

### 1.4.5. Audio Thread Safety and Cross-Thread Correctness

- [ ] No allocation, lock, or blocking operation occurs in any timer callback that reads `phaseOffset` or `masterBPM`.
- [ ] All ValueTree writes (toggling `isSynced`, `isMaster`) originate exclusively on the JUCE message thread.
- [ ] `phaseOffset` is read via `std::atomic<float>::load(std::memory_order_relaxed)` only.
- [ ] The master BPM is read from the same SeqLock-protected `MasterClockSnapshot` structure defined in PRD-0026, using the SeqLock retry loop on the message thread (the loop resolves in 1–2 iterations; its cost on the message thread is negligible).

## 1.5. Grey Areas

1. **Phase meter polling rate and visual noise.** At 60 Hz, each frame of the indicator represents approximately 16.7 ms of elapsed audio. A phase offset that oscillates at the correction controller's natural frequency (roughly one cycle per 375 ms correction window from PRD-0028) will produce visible but not distracting indicator movement. At lower rates (e.g., 20 Hz) the indicator would lag noticeably; at higher rates (e.g., 120 Hz) micro-jitter in the atomic would cause the tick to vibrate visibly. Resolution: 60 Hz is the defined polling rate. The 0.001-beat noise filter suppresses sub-pixel oscillation from floating-point rounding in the atomic without masking genuine drift events.

2. **Phase meter appearance when `isSynced = false`.** Three options exist: (a) hidden entirely, (b) rendered but greyed out, (c) shown at centre (offset = 0). A grayed-out state would imply the component is disabled but present, which may imply stale data is being shown. A centred display is misleading — it implies locked phase when sync is off. Resolution: the component is hidden (`setVisible(false)`) when `isSynced = false`. The parent layout collapses the reserved space so no ghost gap appears. This is the least ambiguous treatment under the 1-bit design system.

3. **Master BPM display during a master handoff transition.** The `MasterClockManager` runs on the message thread and completes the handoff in a single synchronous operation (no async gap). The SeqLock snapshot is updated atomically from the message thread before the next UI paint cycle. Because the timer fires at 60 Hz and the handoff completes in under one message-thread cycle (~16 ms), the master BPM display will either show the outgoing BPM or the incoming BPM — never an intermediate or blank value. Resolution: the display holds the last valid BPM reading during the handoff window. The `---.--` state only appears when the clock enters true dormant state (no playing deck at all), not during a live deck-to-deck handoff.

4. **Locked vs. converging visual distinction within the 1-bit constraint.** The design system prohibits colors and gradients. Two states need to be distinguishable: (a) converging (offset > 0.02 beats, tick-mark moving) and (b) locked (offset < 0.02 beats). Resolution: locked state uses a full solid black bar fill (inverted from the normal surface background), which reads as a distinct binary state change with no ambiguity. The converging state uses the tick mark on a white background. The transition between the two states is instantaneous, which within the 1-bit aesthetic is the correct treatment — it reads as a "snap to lock" signal rather than a fade, which matches the urgency of sync state changes.

5. **Update rate of the master BPM display vs. the phase meter.** Sharing a single 60 Hz timer between `PhaseMeterComponent` and the master BPM display reduces timer overhead and aligns repaint cadence. However, coupling them means a pause or stop of one cannot independently throttle the other. Resolution: both components use the same 60 Hz timer rate but each maintains its own `juce::Timer` instance. Shared timer infrastructure is an implementation convenience, not a requirement. The rate is the same to maintain visual consistency — both indicators update in the same frame so the DJ never sees a stale BPM while the phase meter has already updated.