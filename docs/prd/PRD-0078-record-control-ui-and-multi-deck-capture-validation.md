---
status: Not Implemented
epic: EPIC-0009
depends-on:
  - PRD-0066
  - PRD-0069
  - PRD-0071
  - PRD-0073
  - PRD-0074
  - PRD-0075
  - PRD-0076
  - PRD-0077
---

# 1. PRD-0078: Record Control UI, Record Playhead & Multi-Deck Capture Validation

## 1.1. Problem

By the time this PRD is reached, the entire capture stack of EPIC-0009 exists in
its own right but has never been driven from a real human action nor proven to
work across all four decks at once. The recording session controller (PRD-0071)
owns the arm/record/stop state machine and the master-clock-anchored record
playhead clock, but nothing in the DAW panel (PRD-0066) actually surfaces a
**Record control** the DJ can press, and nothing draws a **moving record
playhead** on the timeline. A DJ today cannot start a capture without a unit
test harness.

There is also no end-to-end proof that the foundational pieces compose
correctly. The performance-event bridge (PRD-0069), clip placement engine
(PRD-0073), alignment resolver (PRD-0074), hot-cue / beat-jump capture
(PRD-0075), loop capture (PRD-0076), and source-mode capture (PRD-0077) were
each validated in isolation against synthetic event streams. They have never
been exercised **simultaneously across four playing decks**, each captured into
its own channel group, while the DJ performs real transport, cue, jump, loop,
and source-mode actions. The interleaving of four concurrent event streams
draining through one message-thread state machine is the single highest-risk
integration point of the Epic and is currently unproven.

Finally, the record playhead must be **visually distinct** from EPIC-0008's live
"now-line" (the E8 read-only timeline cursor). The two cursors share the
timeline surface but mean different things: the now-line shows where live
playback is, the record playhead shows where capture is writing. Conflating them
in the brutalist monochrome language of `DESIGN.md` — where colour is forbidden
and the only tools are pixel density, inversion, and dithering — is a real
design problem that this PRD must resolve, not hand-wave.

## 1.2. Objective

The system surfaces a working Record control and record playhead, wires them to
the recording session controller, and proves the full EPIC-0009 capture pipeline
end-to-end across all active decks simultaneously, such that:

- The DAW panel (PRD-0066) contains a single **global Record (arm) control** —
  a `DESIGN.md`-compliant tactile button — that arms and disarms the recording
  session controller (PRD-0071). Record arming is DAW-level and applies to every
  deck at once; there is no per-deck record button (see §1.5.1).
- The Record button renders three visually distinct, colour-free states:
  **idle/disarmed** (inactive fill), **armed-but-idle** (waiting, no deck has
  produced audio yet), and **actively-recording** (at least one deck is writing
  a clip). The recording state uses a **dithered / inverted** treatment per
  `DESIGN.md`; no red, no colour of any kind is introduced (see §1.5.2).
- A **moving record playhead** is drawn on the timeline, advancing in project
  samples driven by the record playhead clock (PRD-0071). It is **visually
  distinct** from EPIC-0008's live now-line (see §1.5.3).
- Pressing Record arms the session; the record playhead begins advancing from
  the record origin even if no deck is playing (the "press record, do nothing
  for 30 s" case from EPIC-0009 §1.3.2). Pressing Record again stops the session
  per the stop policy (see §1.5.4).
- With the session recording, all active decks (1–4) are captured
  **simultaneously**, each into its own channel group, with each deck beginning
  to write only when **it** produces audio (per-deck staggered start, see
  §1.5.5).
- The full capture pipeline is validated end-to-end across the four decks
  exercising: play/stop (PRD-0071 transport semantics), hot-cue and beat-jump
  (PRD-0075), loop (PRD-0076), source-mode switching (PRD-0077), and alignment
  (PRD-0074), producing **contiguous, grid-aligned or first-beat-anchored**
  clips that match the documented behaviour.
- A manual test plan and an automated test suite under `Tests/` prove that
  multi-deck simultaneous capture produces correct contiguous clips with no lost
  events, no cross-deck clip leakage, and correct alignment per the resolver.
- No new audio-thread code is introduced. The audio thread continues only to
  enqueue POD events into the pre-allocated `PerformanceEventFifo` (PRD-0069);
  all UI and model work is on the message thread.

## 1.3. User Flow

1. The DJ opens the DAW panel (PRD-0066). The global Record button renders in
   its **idle/disarmed** state (inactive fill, `2px solid #2d2d2d` border).
2. The DJ presses Record. The button transitions to its **armed-but-idle**
   state. The recording session controller (PRD-0071) arms; the record playhead
   appears at the record origin and begins advancing in real time even though no
   deck is playing.
3. Thirty seconds later, the DJ presses Play on Deck A (sitting at the 2-minute
   mark of its track). The Record button transitions to its **actively-recording**
   state (dithered / inverted). A clip opens on Deck A's lane(s) starting at
   timeline `0:30`, source-cropped from the 2-minute mark, growing as the deck
   plays.
4. The DJ presses Play on Decks B, C, and D at different moments. Each deck
   begins writing its own clip into its own channel group at the timeline
   position of *its* play event — staggered, not synchronised.
5. The DJ fires a hot cue on Deck B: the active Deck B clip closes at the
   jump-out point and a new contiguous clip opens at the jump-in source position
   (PRD-0075), with no effect on Decks A, C, or D.
6. The DJ engages a loop on Deck C: the looped source segment is written
   back-to-back for each pass (PRD-0076) while the other decks keep capturing
   their own streams uninterrupted.
7. The DJ toggles Deck D from Original to a stem (PRD-0077): subsequent Deck D
   clips switch to the stem lane(s) with no timeline gap.
8. The DJ presses Record again. The session stops, the record playhead freezes,
   all open clips finalise, and the Record button returns to idle. The captured
   arrangement is intact in the `daw` `ValueTree` (playback is EPIC-0010).

## 1.4. Acceptance Criteria

- [ ] The DAW panel (PRD-0066) contains exactly one **global Record (arm)
  control**, a tactile button compliant with `DESIGN.md` (massive square,
  mandatory `2px solid #2d2d2d` border at all times). There is no per-deck
  record button.
- [ ] The Record button renders three distinct, colour-free states: idle
  (inactive: `#fdfdfd` fill, `#2d2d2d` text/border), armed-but-idle (a distinct
  treatment from both idle and recording — see §1.5.2), and actively-recording
  (a **dithered / inverted** treatment using `#2d2d2d` fill and dithering, no
  red, no colour).
- [ ] Pressing the Record button when idle arms the recording session controller
  (PRD-0071); the controller transitions `Disarmed → Armed`. Pressing it again
  stops the session (`Armed`/`Recording → Disarmed`) per the stop policy in
  §1.5.4.
- [ ] When armed, the **record playhead** is drawn on the timeline and advances
  in project samples driven by the PRD-0071 record playhead clock, including when
  no deck is playing (real-time advance over silence).
- [ ] The record playhead is **visually distinct** from EPIC-0008's live now-line
  (distinct glyph / weight / dithering per §1.5.3); the two cursors are never
  confusable on the monochrome timeline.
- [ ] With the session recording and all four decks active, each deck is captured
  into its **own channel group**; a clip written by Deck A never appears on Deck
  B/C/D's lanes and vice versa.
- [ ] Each deck begins writing its first clip only when **that** deck produces
  audio (play/unmute), at the timeline position of its own play event; decks that
  start later have clips that start later (staggered start, see §1.5.5).
- [ ] A hot cue or beat jump on one deck (PRD-0075) closes that deck's active clip
  at the jump-out and opens a new contiguous clip at the jump-in, with **zero**
  effect on the other decks' capture streams.
- [ ] A loop on one deck (PRD-0076) writes the looped source segment back-to-back
  for each pass while the other decks capture uninterrupted.
- [ ] A source-mode switch on one deck (PRD-0077) redirects that deck's
  subsequent clips to the new lane(s) with no timeline gap, with no effect on the
  other decks.
- [ ] Alignment (PRD-0074) is applied per deck: a deck whose BPM matches the
  master tempo and is phase-aligned produces grid-aligned clips; a deck whose BPM
  differs produces first-beat-anchored clips. Decks with different alignment
  outcomes capture correctly in the same session.
- [ ] Across a full multi-deck session, captured clips are **contiguous** on each
  deck's lane(s) (no spurious gaps or overlaps beyond those produced by an actual
  stop/mute), and the total captured event count equals the count produced by the
  decks (no dropped events through the FIFO drain).
- [ ] A test file under `Tests/` (e.g. `RecordingCaptureTests.cpp`) exercises the
  message-thread state machine and the performance-event drain: it injects
  synthetic interleaved events from four decks through the `PerformanceEventFifo`
  (PRD-0069), drains them on the message thread, and asserts the resulting `daw`
  `ValueTree` contains the correct per-channel-group clips with correct
  `(laneId, sourceStartSample, sourceEndSample, timelineStartSample)` tuples.
- [ ] The Record control is registered as a future MIDI-mappable target in
  PRD-0042's control target registry (e.g. `daw.record.arm`, a `Momentary`/`Toggle`
  target) so a controller can drive it in a later Epic; no MIDI binding is wired
  by this PRD (see §1.5.7).
- [ ] No new audio-thread code is added: the audio thread continues only to
  enqueue POD events into the pre-allocated `PerformanceEventFifo`; the UI reads
  state on the message thread; no allocation, lock, or I/O occurs on the audio
  thread.
- [ ] Recording over a region that already contains clips **appends** new clips
  from the record origin forward; overwrite / replace semantics are explicitly
  deferred (see §1.5.6).

## 1.5. Grey Areas

### 1.5.1. Record Button Placement: Global vs Per-Deck

The Record control could live globally at the DAW-panel level (one button arms
capture for every deck) or per-deck (each deck has its own record-arm so the DJ
chooses which decks to capture).

**Resolution:** Global, DAW-level record arming all decks. EPIC-0009 §1.2.1
defines multi-deck capture as "all active decks (1–4) are captured
simultaneously, each into its own channel group" — the model is a single
arrangement timeline with one record origin shared by every deck. A single
global Record matches a DAW's transport record button, keeps the state machine
(PRD-0071) singular, and avoids the confusion of four independent record states
racing one record playhead. Per-deck record-enable arming (DAW-style track arm)
is a reasonable future refinement, but it belongs to EPIC-0010/0012 once
playback and project structure exist; this Epic ships the global control. A deck
that is simply not playing produces no clips, which already gives the DJ
selective capture without a per-deck button.

### 1.5.2. Armed-But-Idle Visual vs Actively-Writing Visual

The Record button has two "on" conditions that must read differently: **armed
but no deck is producing audio yet** (the playhead is moving over silence) versus
**actively writing at least one clip**. `DESIGN.md` forbids colour, so the
distinction must come from pixel density, inversion, or dithering.

**Resolution:** Three states, all colour-free. **Idle/disarmed** is the standard
inactive button (`#fdfdfd` fill, `#2d2d2d` text, `2px` border). **Armed-but-idle**
uses a **dithered (50% checkerboard) fill** of `#2d2d2d` over the `#fdfdfd`
ground — the button reads as "primed" without being fully solid. **Actively-
recording** uses the full **inverted active state** (`#2d2d2d` solid fill,
`#fdfdfd` text/glyph). This gives a clear density ramp idle → stippled → solid
that maps intuitively to off → armed → writing, stays strictly monochrome, and
reuses `DESIGN.md`'s dithered-density vocabulary (§"Dithered Gradients",
§"Tactile Buttons"). No red, no blink-as-alarm; a slow 1-bit block pulse on the
armed-but-idle state is permitted but optional.

### 1.5.3. Record Playhead vs Live Now-Line Visual Distinction

The timeline already draws EPIC-0008's live now-line (read-only playback
cursor). The record playhead is a second cursor on the same surface and must not
be confusable with it, again without colour.

**Resolution:** The live now-line stays as EPIC-0008 defines it (a thin `1px`
solid `#2d2d2d` vertical line). The record playhead is rendered as a **thicker,
dithered vertical band** — a `2px`–`3px` wide vertical stripe filled with a 50%
checkerboard pattern of `#2d2d2d`, optionally capped with a small solid square
"record head" glyph at the top ruler. The density and width difference makes the
record playhead unmistakably heavier than the hairline now-line while remaining
monochrome. When the session is disarmed the record playhead is not drawn at
all; only the now-line remains. This keeps the two semantics — "where playback
is" vs "where capture writes" — visually separable at a glance.

### 1.5.4. Stop Confirmation / Discard vs Keep

When the DJ presses Record a second time to stop, the session could (a) stop and
**keep** the captured arrangement silently, (b) stop and prompt a confirmation
(keep / discard), or (c) stop and discard.

**Resolution:** Stop and **keep**, no confirmation dialog. EPIC-0009 captures a
non-destructive arrangement into the `daw` `ValueTree`; keeping it is the
expected outcome and matches every DAW's stop behaviour (stopping the transport
does not erase the take). A confirmation prompt on every stop would be hostile in
a live performance context where the DJ may stop and re-arm frequently. Discard
of a take is an **editing** operation and belongs to EPIC-0010 (which owns
clip move/trim/split/delete); a future "clear arrangement" action can live there.
For this Epic, stop simply finalises all open clips and freezes the playhead.

### 1.5.5. Multi-Deck Start Staggering

When several decks are active, capture could start every deck's clip at the
record origin (synchronised) or start each deck's clip only when that deck
actually plays (staggered).

**Resolution:** Staggered — each deck writes its first clip when **it** produces
audio. EPIC-0009 §1.2.1 is explicit: "recording begins writing clips only when a
deck actually produces audio (a play/unmute event), not on silence." A deck
parked at a cue point during arming contributes nothing until the DJ plays it; a
deck started 45 s into the session produces a clip beginning at timeline `0:45`.
This faithfully represents the performance (the arrangement mirrors what was
actually heard and when) and falls out naturally from the clip-open-on-play
semantics already built in PRD-0071/PRD-0073. Synchronised start would
fabricate clips for silence and is incorrect.

### 1.5.6. Recording Over Existing Clips: Append vs Overwrite

If the DJ re-arms and records over a timeline region that already contains clips
from a prior take, the engine could **append** the new clips alongside the
existing ones or **overwrite** the overlapped region.

**Resolution:** Append from the record origin forward; overwrite is deferred to
EPIC-0010. This Epic's job is *capture*, not *editing*. Overwriting (punching
in, replacing an overlapped region, comping takes) is destructive-by-intent
clip editing and squarely an EPIC-0010 concern. For EPIC-0009, a second take
simply appends additional clips onto the lanes from the record origin; any
visual overlap is left for the editing Epic to resolve. This keeps the capture
engine simple and lossless and avoids inventing an editing model prematurely. If
the record origin defaults to timeline 0 on each arm, successive takes stack from
0; the origin policy itself is owned by PRD-0071.

### 1.5.7. MIDI-Mappable Record Control

The Record control is a prime candidate for hardware mapping (most controllers
have a transport Record button), but EPIC-0009 does not include the MIDI wiring.

**Resolution:** Register the target now, wire it later. This PRD adds a control
target registry entry (PRD-0042) for the Record control (e.g. `daw.record.arm`,
modelled as a `Momentary` or `Toggle` target, `softTakeover = false`) so the
registry is aware of it and a future mapping Epic can bind a controller button
to it without re-architecting. No bundled profile binding and no feedback-engine
LED wiring is added by this PRD — those follow the same pattern PRD-0061
established for mixer targets and belong to a later MIDI-integration PRD. This
keeps the registry the single source of truth for mappable targets while
deferring the actual binding work.
