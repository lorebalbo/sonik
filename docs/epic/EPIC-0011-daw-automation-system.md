---
name: "EPIC-0011: DAW Automation System"
status: Open
---

# 1. EPIC-0011: DAW Automation System

## 1.1. Goal and Vision

Add **automation** to the DAW: time-varying parameter motion drawn on lanes
beneath each channel group and the master, captured live during recording,
replayed during timeline playback, and freely editable afterwards. Where
EPIC-0009 captured *structure* (which part of which song plays where) and
EPIC-0010 made it *audible and editable*, this Epic captures and reproduces the
*performance gestures* — the filter sweeps, EQ moves, gain rides, tempo changes,
and mode toggles that make a mix feel alive.

The experience: while recording, the DJ slowly opens the filter on Deck A and
nudges the master tempo up; on playback those exact moves are reproduced and
their curves are visible as **automation lanes** under the channel/master. The
DJ can then redraw a sweep, soften a gain ride, or move a key-lock toggle to a
different beat. Two automation shapes exist: **continuous** (smooth value over
time) and **boolean** (on/off step).

Automated parameters in scope:

- **Master**: master tempo (the grid/clock tempo itself).
- **Per channel**: filter, high, mid, low, gain (continuous).
- **Per channel**: key-lock, pitch-stretch, key-stepper (boolean).

This Epic depends on EPIC-0009 (capture infrastructure / event bridge) and
EPIC-0010 (the timeline render engine that automation must drive during
playback). It owns automation **capture, storage, playback application, and
editing UI**.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- **Automation lanes** beneath each channel group (filter, high, mid, low,
  gain) and beneath the master (tempo), plus **boolean automation lanes**
  (key-lock, pitch-stretch, key-stepper) per channel, all DESIGN.md-compliant.
- **Live automation capture**: while recording (EPIC-0009), continuous parameter
  changes are recorded as breakpoint curves and boolean toggles as step events,
  time-stamped against the master grid.
- **Automation playback**: during DAW playback (EPIC-0010), recorded automation
  is applied so the mix reproduces the original gestures — including the
  **master-tempo automation driving the master clock** so the grid and any
  grid-aligned clips follow the recorded tempo moves.
- **Automation editing**: add / move / delete breakpoints, redraw continuous
  curves, change interpolation (at minimum linear and step/hold), and move
  boolean toggle points; all grid-snappable and undoable.
- **Show/hide and per-lane enable (bypass)** of automation lanes.

Foundational systems (non-user-facing):

- An **automation data model** as part of the `daw` `ValueTree`: lanes keyed by
  `(channelGroup | master, parameterId)`, each holding ordered breakpoints
  (`timelineSample`, `value`, `interpolation`) for continuous lanes or step
  events for boolean lanes.
- **Automation capture taps** that observe the relevant `ValueTree` parameters
  (mixer filter/EQ/gain from EPIC-0007; master tempo from EPIC-0003; deck
  key-lock/pitch-stretch/key-stepper from EPIC-0001/PRD-0025) and record changes
  through the EPIC-0009 event bridge while armed.
- An **automation playback applier** that, during timeline playback, evaluates
  each enabled lane at the current playhead and writes the resulting value to
  the correct target — through the **same single-source-of-truth paths** the
  live controls use (mixer `ValueTree` params, `MasterClockManager` for tempo,
  deck params for booleans), never a parallel back door.
- Integration with EPIC-0010's **arrangement snapshot** so automation values are
  available to the render engine coherently and click-free.

### 1.2.2. Out of Scope

- **Effects automation** (reverb/delay/beat-FX) — there is no effects Epic yet;
  automation is limited to the parameters enumerated above.
- **Crossfader / channel-fader automation** — not in the requested parameter
  set; can be a future enhancement (the model is designed to accommodate new
  parameter ids without migration).
- **MIDI-driven automation recording from external controllers as a separate
  stream** — automation is captured from the resulting parameter changes, not
  from raw MIDI; the MIDI layer (EPIC-0005/0006) already maps to those params.
- **Project save/load and export** → EPIC-0012 (which serialises automation as
  part of the project and renders it during bounce).
- **Structural capture and clip editing** → EPIC-0009 / EPIC-0010.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Continuous vs Boolean Lanes

- **Continuous lane** (filter, high, mid, low, gain, tempo): ordered breakpoints
  with an interpolation mode per segment (linear default; step/hold supported).
  Value range and units match the underlying parameter's existing definition
  (e.g., EQ/filter ranges from EPIC-0007, tempo in BPM from EPIC-0003).
- **Boolean lane** (key-lock, pitch-stretch, key-stepper): ordered step events
  toggling state; value is held until the next event. The key-stepper is
  modelled as boolean per the requested behaviour (its semitone state is owned
  by PRD-0025; this lane records its engaged/stepped transitions).

### 1.3.2. Capture Through Existing State, Not a Side Channel

Automation capture observes the **authoritative `ValueTree` parameters** that
the live UI and MIDI already write to:

- Mixer params (`mixer.channel.*.filter|eq.high|mid|low|gain`) — EPIC-0007.
- Master tempo (master clock BPM) — EPIC-0003.
- Deck booleans (key-lock, pitch-stretch, key-stepper) — EPIC-0001 / PRD-0025.

A change to one of these while recording appends a breakpoint/step to the
matching lane via the EPIC-0009 event bridge (message thread). This guarantees
captured automation is exactly what the DJ did, with no divergence between the
live control and the recorded lane.

### 1.3.3. Playback Application & the Single-Source-of-Truth Invariant

During playback the **automation applier** evaluates enabled lanes at the
playhead and writes values **back through the same channels** the live controls
use:

- Continuous mixer params → mixer `ValueTree` (smoothed, click-free per
  EPIC-0007).
- **Master tempo → `MasterClockManager`** (EPIC-0003), so the grid, synced
  decks, and grid-aligned clips all follow the recorded tempo automation from
  one tempo authority. The DAW never forks the tempo.
- Deck booleans → deck `ValueTree` params.

The applier runs on the message thread at a high cadence and/or feeds the
audio-thread arrangement snapshot (EPIC-0010 §1.3.2) for sample-accurate,
click-free parameter values where required. No automation writes ever occur on
the audio thread directly; the audio thread reads published snapshots.

### 1.3.4. Audio-Thread Safety

Per `AGENTS.md`: automation evaluation that must be sample-accurate is delivered
to the audio thread via the SeqLock-published arrangement/parameter snapshot
(EPIC-0010), not by walking lanes in `processBlock`. Continuous values are
smoothed to avoid zipper noise, consistent with EPIC-0007's parameter smoothing.
No allocation, locks, or I/O on the audio thread.

### 1.3.5. State, File Structure, Design

- Automation lives in the `daw` `ValueTree` under each track/master node;
  serialisation is handled by EPIC-0012.
- New code under `Source/Features/Daw/Automation/`
  (`AutomationModel`, `AutomationCaptureTaps`, `AutomationApplier`,
  `BooleanLane`, `ContinuousLane`) plus `Ui/` lane organisms.
- All edits flow through EPIC-0010's command layer (undo/redo).
- All UI complies with `DESIGN.md` (monochrome breakpoint curves via tonal
  layering and dithering, not colour).

## 1.4. PRD Roadmap

- [ ] PRD-0087: Automation Data Model (continuous + boolean lanes in the `daw` ValueTree, breakpoint/step structures)
- [ ] PRD-0088: Automation Capture Taps (observe mixer/tempo/deck params, append via EPIC-0009 event bridge while recording)
- [ ] PRD-0089: Master-Tempo Automation Lane & Capture (records master-clock BPM changes)
- [ ] PRD-0090: Per-Channel Continuous Lanes — Filter / High / Mid / Low / Gain (capture)
- [ ] PRD-0091: Per-Channel Boolean Lanes — Key-Lock / Pitch-Stretch / Key-Stepper (capture)
- [ ] PRD-0092: Automation Playback Applier (single-source-of-truth write-back; tempo → MasterClockManager; click-free snapshot delivery)
- [ ] PRD-0093: Automation Lane UI & Rendering (DESIGN.md breakpoint curves, show/hide, per-lane bypass)
- [ ] PRD-0094: Automation Editing (add/move/delete breakpoints, interpolation, boolean toggles) via the edit command layer
