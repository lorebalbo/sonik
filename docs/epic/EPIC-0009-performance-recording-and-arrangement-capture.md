---
name: "EPIC-0009: Performance Recording & Arrangement Capture"
status: Implemented
---

# 1. EPIC-0009: Performance Recording & Arrangement Capture

## 1.1. Goal and Vision

Turn the live, read-only timeline of EPIC-0008 into a **recordable performance
surface**. This Epic delivers the DAW's own **Record** control and the capture
engine that, while armed, translates everything the DJ does on the decks and
mixer into **grid-aligned, non-destructive clips** written onto the channel
groups. The result is a reconstructable arrangement of a DJ set built entirely
from references into the original source files — never a DAC capture.

The experience: the DJ presses **Record** in the DAW. The timeline playhead
begins advancing from 0 (or the current position), exactly as if someone hit
Play in a DAW. Nothing is written until the DJ acts. Thirty seconds in, the DJ
presses Play on Deck A while that deck sits at the 2-minute mark of its track —
the DAW writes a clip that *starts at timeline 0:30* and whose source crop
*begins at the track's 2-minute mark*, growing as the deck plays. Pressing a
hot cue makes the deck jump; the capture engine closes the current clip and
opens a new one at the new source position. A loop produces a repeated source
segment (the same crop placed back-to-back). When the deck's BPM matches the
master tempo, the clip snaps to the channel grid; when it does not, only the
clip's first beat anchors to the grid and the rest plays free.

This Epic captures **structural performance events** (transport, hot cues, beat
jumps, loops, source-mode changes) into the clip model. It does **not** capture
continuous knob automation (filter/EQ/gain/tempo) — that is EPIC-0011 — and it
does **not** play the arrangement back or edit it — those are EPIC-0010.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- A **Record (arm) control** in the DAW panel with clear armed/recording state
  (DESIGN.md-compliant), plus a moving **record playhead**.
- **Record transport semantics**: start, stop, and the recording position
  reference; recording begins writing clips only when a deck actually produces
  audio (a play/unmute event), not on silence.
- **Automatic clip creation from deck playback**: pressing Play on a deck during
  recording writes a clip on the correct lane(s) (per the active source mode
  from EPIC-0002), starting at the deck's current source position and at the
  current timeline position.
- **Grid alignment behaviour**:
  - If the deck's BPM equals the master tempo (within tolerance) and the deck is
    sync-locked/phase-aligned, the clip is **grid-aligned** to the channel grid.
  - If the BPM differs, the clip is **first-beat-anchored** only: its first
    downbeat lands on a grid line, the remainder is placed free (no per-beat
    snapping).
- **Hot-cue & beat-jump capture**: a cue/jump during recording closes the
  active clip at the jump-out point and opens a new clip at the jump-in source
  position, on the same lane(s), contiguous on the timeline.
- **Loop capture**: an active loop writes the looped source segment repeatedly
  for as long as the loop is engaged, producing back-to-back identical crops
  (so the section audibly repeats in the arrangement).
- **Source-mode capture**: switching a deck between Original and stems (or
  toggling a stem) during recording switches which lane(s) subsequent clips are
  written to, with no gap on the timeline.
- **Multi-deck capture**: all active decks (1–4) are captured simultaneously,
  each into its own channel group.

Foundational systems (non-user-facing):

- A **recording session controller** (message-thread state machine) owning the
  armed/recording state, the record playhead clock (derived from the master
  clock + project sample rate), and start/stop transitions.
- A **performance-event bridge**: a lock-free path by which deck/mixer
  structural events (play, stop, cue jump, beat jump, loop in/out, source-mode
  change) are surfaced to the recorder on the message thread without touching
  the audio thread.
- A **clip-placement engine** that converts an open performance event into a
  growing `DawClip`, applies the alignment rule, and finalises clips on
  close events.
- An **alignment resolver** implementing the BPM-match-vs-first-beat policy
  using the master clock phase origin and the deck beatgrid (PRD-0008).

### 1.2.2. Out of Scope

- **Continuous parameter automation capture** (tempo, filter, high/mid/low,
  gain) and **boolean automation** (key-lock, pitch-stretch, key-stepper) →
  EPIC-0011. (This Epic captures *structure*; automation captures *parameter
  motion*.)
- **Playing the recorded arrangement back / DAW Play-Pause for listening** →
  EPIC-0010.
- **Editing clips after capture** (move/trim/split/uncrop/delete) → EPIC-0010.
- **Project save/load and audio export** → EPIC-0012.
- **Time-stretching a clip to force-fit a non-matching BPM onto the grid** —
  out of scope by design; non-matching tracks are first-beat-anchored and play
  at their own tempo (stretch-to-grid is a possible future enhancement).
- **The deck-side original-vs-stems toggle** itself → EPIC-0002 PRD. This Epic
  reads the resulting source mode.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Event-Based Capture, Not Audio Capture

The recorder never records audio samples. It records **what happened and when**,
expressed against the master grid, and stores the result as non-destructive
`DawClip` crops (EPIC-0008 §1.3.1). A captured clip is fully described by:
`(laneId, sourceFileId, sourceStartSample, sourceEndSample, timelineStartSample)`.
Audio is only ever read from the source later (EPIC-0010 playback / EPIC-0012
export). This keeps capture allocation-light and guarantees lossless fidelity.

### 1.3.2. The Record Playhead Clock

The record playhead advances in project samples, anchored to the master clock so
that captured positions are expressed coherently in musical time:

- Timeline position is sampled from the master-grid service (EPIC-0008 §1.3.2),
  not from any single deck.
- Recording start sets `timelineOrigin`; every captured event's
  `timelineStartSample` is `currentTimelinePosition` at the event instant.
- If the master clock is dormant (no playing deck) the playhead still advances
  in real time at the project sample rate so silence/gaps are represented
  faithfully (the "press record, do nothing for 30 s" case).

### 1.3.3. Performance-Event Bridge (thread safety)

Structural events originate from deck/mixer interactions that may be triggered
on the audio thread (e.g., a quantized cue firing) or the message thread (a UI
click). Per `CLAUDE.md`:

- Audio-thread-originated events are pushed into a pre-allocated lock-free FIFO
  (`juce::AbstractFifo`) carrying small POD event records (event type, deck id,
  source sample position, timestamp). No allocation, no locks, no I/O on the
  audio thread.
- A message-thread drain (the recording session controller) reads the FIFO and
  mutates the `daw` `ValueTree`. All clip creation/finalisation happens on the
  message thread.
- Message-thread-originated events are enqueued through the same path for a
  single ordered stream.

### 1.3.4. Clip Lifecycle: Open → Grow → Close

- **Open**: a play/unmute/jump-in/source-mode event creates a new open clip with
  `sourceStartSample = deck source position`, `timelineStartSample = playhead`.
- **Grow**: while open, the clip's `sourceEndSample` advances with the deck's
  source position (driven by the same projection cadence as EPIC-0008's live
  projection, now persisted into the recording rather than discarded).
- **Close**: a stop/mute/jump-out/source-mode-change/loop-boundary event
  finalises `sourceEndSample` and `timelineEndSample`.
  A jump or source-mode change closes one clip and opens the next contiguously.

### 1.3.5. Alignment Resolver

Given a deck's beatgrid (PRD-0008) and the master clock phase origin:

```text
if |deckBPM - masterBPM| <= tolerance AND deck is phase-aligned:
    clip.timelineStartSample = snap(playhead, gridLine)   // grid-aligned
    grid lines under the clip coincide with the deck's beats
else:
    anchor = nearest gridLine to the clip's first captured downbeat
    clip.timelineStartSample = anchor                     // first-beat only
    remainder placed free (no per-beat snap)
```

Tolerance and "phase-aligned" reuse EPIC-0003's definitions; the resolver does
not introduce a competing sync notion.

### 1.3.6. Loop, Hot-Cue & Beat-Jump Rendering

- **Loop**: while a loop is engaged, the capture engine emits a repeated crop
  `[loopStart, loopEnd]` placed back-to-back on the timeline for each pass,
  reflecting that the section is heard multiple times. Exiting the loop resumes
  a single growing clip from the live source position.
- **Hot-cue / beat-jump**: a discontinuity in the deck's source position closes
  the current clip at the out-point and opens a new one at the in-point,
  contiguous on the timeline (the set keeps moving forward in time even though
  the source jumped).

### 1.3.7. State, File Structure, Design, Audio Safety

- Capture writes into the existing `daw` `ValueTree` from EPIC-0008; no parallel
  model is introduced.
- New code lives under `Source/Features/Daw/Recording/`
  (`RecordingSessionController`, `PerformanceEventFifo`, `ClipPlacementEngine`,
  `AlignmentResolver`) plus `Ui/` additions for the record control/playhead.
- All UI complies with `DESIGN.md`.
- Strict `CLAUDE.md` audio-thread compliance: audio thread only *enqueues* POD
  events into a pre-allocated FIFO; all model work is message-thread.

## 1.4. PRD Roadmap

- [x] PRD-0071: Recording Session Controller & Record Playhead Clock (arm/record/stop state machine, master-clock-anchored timeline position)
- [x] PRD-0072: Performance-Event Bridge (lock-free FIFO + message-thread drain for deck/mixer structural events)
- [x] PRD-0073: Clip Placement Engine & Clip Lifecycle (open/grow/close into the `daw` model)
- [x] PRD-0074: Grid Alignment Resolver (BPM-match snap vs first-beat anchoring)
- [x] PRD-0075: Hot-Cue & Beat-Jump Capture (clip split at jump boundaries, contiguous timeline)
- [x] PRD-0076: Loop Capture (repeated source-segment rendering)
- [x] PRD-0077: Source-Mode Capture (Original ↔ stems lane switching mid-record)
- [x] PRD-0078: Record Control UI, Record Playhead & Multi-Deck Capture Validation
