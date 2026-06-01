---
status: Implemented
epic: EPIC-0009
depends-on:
  - PRD-0026
  - PRD-0063
  - PRD-0064
  - PRD-0069
---

# 1. PRD-0071: Recording Session Controller & Record Playhead Clock

## 1.1. Problem

EPIC-0008 delivered a live, read-only DAW timeline: a `daw` `ValueTree` model,
a master-grid service (PRD-0064 / MasterGridService) that projects musical time,
the master clock (PRD-0026 / MasterClockManager), and a read-only timeline view
(PRD-0069). Nothing in that stack can *record*. There is no owner of an
armed/recording/stopped lifecycle, no notion of a "record playhead", and no
defined reference against which a future capture engine could stamp the instant
at which a deck started playing.

EPIC-0009 turns that read-only surface into a recordable performance surface,
and every later PRD in the Epic — the performance-event bridge (PRD-0072), the
clip-placement engine (PRD-0073), the alignment resolver (PRD-0074), and the
record UI (PRD-0078) — depends on a single coherent answer to two questions:
**"are we recording?"** and **"what timeline sample are we at right now?"**.
Without a single source of truth for those, each consumer would invent its own
clock and its own state, and captured clip positions would drift apart from the
musical grid they are supposed to be expressed in.

Two subtleties make this non-trivial. First, recording must begin *writing
clips* only when a deck actually produces audio (a play / unmute event), never
on silence — otherwise pressing Record and waiting produces phantom empty clips.
Second, the record playhead must keep advancing in real time even when no deck
is playing and the master clock is dormant (the "press Record, do nothing for
30 s, then press Play on Deck A" case), so that the eventual clip correctly lands
at timeline 0:30 and not at timeline 0:00. The playhead clock and the
write-gating are therefore two distinct concerns that this PRD must reconcile.

## 1.2. Objective

The system provides a message-thread `RecordingSessionController` that owns the
recording lifecycle and the record playhead clock such that:

- A single state machine owns the recording state with exactly three states:
  `Stopped`, `Armed`, and `Recording`, plus the documented transitions between
  them (§1.3).
- The controller exposes the **record playhead position** as a project-sample
  count (`currentTimelinePosition`) that any message-thread consumer
  (PRD-0072 / PRD-0073) can sample at an event instant.
- The record playhead advances in **project samples**, anchored to the master
  clock via PRD-0064's MasterGridService when a deck is driving the grid, so
  captured positions are coherent musical time rather than wall-clock time.
- When the master clock is **dormant** (no playing deck), the playhead still
  advances in real time at the project sample rate, faithfully representing the
  silence/gap so a later play event lands at the correct timeline offset.
- On the `Armed` → `Recording` transition (the first real audio event), the
  controller sets `timelineOrigin` and begins advancing the playhead; every
  consumer reads `currentTimelinePosition` as `(now − timelineOrigin)` expressed
  in project samples.
- The controller writes its lifecycle state and the live playhead position into
  the existing `daw` `ValueTree` (no parallel model), so the UI (PRD-0078) and
  the capture engine observe one source of truth via JUCE listeners.
- The controller introduces **no audio-thread code**: it runs entirely on the
  message thread and reads the master clock through the existing lock-free
  snapshot mechanisms that PRD-0026 / PRD-0064 already expose.

This PRD owns the **clock and the state machine only**. It does not consume
performance events (PRD-0072), does not create or place clips (PRD-0073), does
not implement grid alignment (PRD-0074), and adds no UI (PRD-0078).

## 1.3. Developer / Integration Flow

1. A new feature slice is created under `Source/Features/Daw/Recording/` with
   `RecordingSessionController.h` / `RecordingSessionController.cpp`. The
   controller is constructed with explicit dependencies (no singletons):
   a reference to the `daw` `ValueTree` (PRD-0063), the MasterGridService
   (PRD-0064), and the MasterClockManager (PRD-0026).

   ```text
   Source/Features/Daw/Recording/
   ├── RecordingSessionController.h
   └── RecordingSessionController.cpp
   ```

2. The controller defines a `RecordingState` enum (`Stopped`, `Armed`,
   `Recording`) and a `recordPlayheadSample` (an `int64` project-sample count).
   The state and the playhead are mirrored into the `daw` `ValueTree` under a
   `recording` child node (`recording.state`, `recording.timelineOrigin`,
   `recording.playheadSample`) so listeners react rather than poll.

3. Transitions are defined as explicit methods that mutate state on the message
   thread:

   ```text
   arm()      : Stopped  -> Armed       (sets up clock source, playhead = 0 or
                                          current scroll position; see §1.5.1)
   stop()     : Armed    -> Stopped     (no clip work; nothing was captured)
              : Recording-> Stopped     (finalisation delegated to PRD-0073)
   beginCapture(): Armed -> Recording   (first audio event; sets timelineOrigin,
                                          starts advancing the playhead)
   ```

   `beginCapture()` is invoked by the performance-event bridge (PRD-0072) on the
   first deck play/unmute event; this PRD defines the transition and its clock
   side effects, not the event source.

4. A message-thread timer (or the existing DAW projection cadence from PRD-0064)
   ticks the playhead while in `Recording`. On each tick the controller computes
   the new `recordPlayheadSample`:
   - If the master clock is **running** (a deck is driving it), the playhead is
     derived from the MasterGridService's projected position so it stays in lock
     with musical time.
   - If the master clock is **dormant**, the playhead is advanced by
     `elapsedWallClockSeconds * projectSampleRate`, so silence advances at the
     project rate (see §1.5.2 and §1.5.5).

5. The controller exposes `currentTimelinePosition()` returning the live
   `recordPlayheadSample`. PRD-0072 / PRD-0073 call this at the instant an event
   is drained to stamp each captured event's `timelineStartSample`.

6. The controller writes `recording.playheadSample` into the `daw` `ValueTree`
   on each tick (throttled to the projection cadence) so PRD-0078's record
   playhead and PRD-0069's timeline view can render the moving playhead from
   state, with no direct coupling to the controller.

7. A new test file `Tests/RecordingSessionControllerTests.cpp` is added to the
   `SonikTests` target verifying the state machine transitions, the
   `timelineOrigin` semantics, the dormant-clock real-time advance, and the
   master-clock-anchored advance using a fake clock source.

## 1.4. Acceptance Criteria

- [ ] A `RecordingSessionController` class exists under
      `Source/Features/Daw/Recording/` and is constructed with explicit
      references to the `daw` `ValueTree` (PRD-0063), the MasterGridService
      (PRD-0064), and the MasterClockManager (PRD-0026); it holds no global
      state and is not a singleton.
- [ ] A `RecordingState` enum defines exactly `Stopped`, `Armed`, and
      `Recording`; the controller starts in `Stopped`.
- [ ] `arm()` transitions `Stopped` → `Armed`, establishes the clock source, and
      initialises the playhead per §1.5.1; calling `arm()` while not `Stopped`
      is a no-op (or asserts in debug) and does not corrupt state.
- [ ] `beginCapture()` transitions `Armed` → `Recording`, sets
      `timelineOrigin` to the current playhead reference, and begins advancing
      the record playhead; calling it from any state other than `Armed` is a
      no-op.
- [ ] `stop()` transitions `Armed` → `Stopped` (nothing captured) or
      `Recording` → `Stopped`; in the `Recording` case the controller signals a
      stop but delegates open-clip finalisation to PRD-0073 (see §1.5.4) and
      performs no clip work itself.
- [ ] `currentTimelinePosition()` returns the live record playhead as an
      `int64` project-sample count, equal to `0` at `timelineOrigin` and
      increasing monotonically while in `Recording`.
- [ ] While `Recording` with a running master clock, the playhead is derived
      from the MasterGridService projected position so it stays coherent with
      musical time (verified against a fake clock source advancing a known
      number of beats).
- [ ] While `Recording` with a dormant master clock (no playing deck), the
      playhead advances in real time at the project sample rate (verified: after
      ~30 s of dormant time the playhead equals `30 * projectSampleRate` within
      one projection-tick tolerance).
- [ ] The controller mirrors `recording.state`, `recording.timelineOrigin`, and
      `recording.playheadSample` into the existing `daw` `ValueTree`; a JUCE
      listener attached to that subtree observes state and playhead changes
      without polling the controller directly.
- [ ] Recording **writing of clips** is not triggered by `arm()` alone; the
      `Armed` → `Recording` transition (and therefore any clip writing) occurs
      only on a real audio event surfaced by PRD-0072, never on silence. This
      PRD does not write clips; it only exposes the transition and the clock.
- [ ] No audio-thread code is added by this PRD. The controller runs on the
      message thread, reads the master clock via the existing lock-free
      snapshot mechanisms (PRD-0026 / PRD-0064), and performs no allocation,
      locking, or I/O on any audio-thread path.
- [ ] No UI is added by this PRD; PRD-0078 owns the record control and the
      visible playhead. This PRD only writes the state/playhead into the model.
- [ ] `Tests/RecordingSessionControllerTests.cpp` covers: each valid transition,
      each invalid transition (no-op), `timelineOrigin` correctness, the
      dormant-clock real-time advance, and the running-clock anchored advance,
      using an injected fake clock source.

## 1.5. Grey Areas

### 1.5.1. Record Start Position: Always 0 vs Current Scroll Position

When the DJ arms recording, the playhead could always begin at timeline sample
`0`, or it could begin at the timeline view's current scroll/insert position
(as a DAW does when you place the cursor mid-arrangement and hit record).

**Resolution:** For this Epic, recording always starts at timeline `0`. EPIC-0009
captures a performance from the beginning of a take; there is no edit cursor, no
existing arrangement to overdub into, and no playback transport (EPIC-0010) that
would establish a meaningful "current position". `arm()` initialises the playhead
to `0`, and `timelineOrigin` is set at the `beginCapture()` instant. A future
overdub/insert-record workflow (EPIC-0010 territory, once DAW Play exists) can
introduce a start-position parameter to `arm()` without breaking this contract:
the default remains `0`. Keeping the start fixed avoids surfacing a scroll
position that has no transport meaning yet and keeps captured `timelineStartSample`
values directly interpretable as "seconds into the take".

### 1.5.2. Playhead Advance When No Deck Is Playing

If the playhead were anchored *only* to the master clock, it would freeze
whenever no deck drives the grid (the master clock is dormant), so a deck that
starts playing 30 s after Record would incorrectly land at timeline `0:00`.

**Resolution:** Yes — the playhead advances in real time at the project sample
rate while the master clock is dormant, faithfully representing the silence/gap.
This is mandated by EPIC-0009 §1.3.2. The controller therefore has two advance
modes: master-clock-anchored when a deck is running (musical time), and
wall-clock × project-sample-rate when dormant (real time). Both produce a
project-sample count in the same units, so consumers never need to know which
mode is active. The cost — that "silence at the project tempo" is a slightly
fictional musical interval when no tempo is established (see §1.5.5) — is
accepted because the alternative (a frozen playhead) breaks the core
"press Record, do nothing, then play" use case.

### 1.5.3. Arm vs Record Distinction: Auto-Start vs Immediate

Two models are possible: (a) `arm` and `record` are distinct — arming readies
the session and the first audio event auto-starts capture; or (b) pressing
Record immediately enters `Recording` and the playhead runs from that instant.

**Resolution:** Distinct states with auto-start on first audio
(`Stopped → Armed → Recording`). This is the model EPIC-0009 §1.2.1 describes:
the playhead begins advancing when Record is pressed, but **nothing is written
until the DJ acts**. The clean way to encode "playhead running, but not yet
capturing clips" is the `Armed` state: in `Armed` the playhead is initialised
and the controller is ready, and the `beginCapture()` transition (driven by
PRD-0072's first play/unmute event) flips to `Recording` and stamps
`timelineOrigin`. Conceptually the DJ presses one button ("Record"); internally
that arms, and the first real audio promotes to `Recording`. Collapsing the two
into a single immediate `Recording` state would lose the ability to represent
the pre-first-audio window distinctly and would complicate the
"don't write on silence" guarantee.

### 1.5.4. Stop Semantics: Finalising Open Clips

When `stop()` is called during `Recording`, any clip that is still open (a deck
still playing) must be finalised — its `sourceEndSample` / `timelineEndSample`
set — before the session ends.

**Resolution:** Delegate clip finalisation to PRD-0073 (the clip-placement
engine). This PRD's `stop()` is responsible only for the state transition
(`Recording → Stopped`) and for emitting a single, well-ordered "stop" signal
that PRD-0073 consumes to close any open clips at the current playhead. The
controller deliberately does not reach into the clip model: clip lifecycle is
PRD-0073's contract, and duplicating finalisation logic here would create two
owners of clip state. The acceptance criteria assert only that `stop()` produces
the transition and the stop signal; the actual finalisation behaviour is tested
in PRD-0073.

### 1.5.5. Master-Clock-Dormant Tempo for Playhead Advance

While dormant, the playhead advances by wall-clock seconds × project sample
rate. But "project sample rate" is unambiguous (it is the audio device rate),
whereas the *musical* interpretation of that dormant span depends on a tempo
that, by definition, no deck is currently asserting.

**Resolution:** During dormant advance the controller advances strictly in
**project samples** (wall-clock × sample rate) and assigns **no musical/tempo
interpretation** to that span. Musical meaning (bars/beats) is derived later by
PRD-0064's grid service and PRD-0074's alignment resolver from the master tempo
that becomes active once a deck plays. The dormant span is therefore stored as a
pure sample offset; if the project tempo is `120 BPM` and 30 s of dormancy
elapse, those samples *happen* to equal a certain number of bars at that tempo,
but the controller never computes or persists that bar count. This keeps the
clock free of tempo assumptions it cannot justify and leaves musical
interpretation to the single component that owns it (the grid service).

### 1.5.6. Re-Record / Append Behaviour

After a `Recording → Stopped` cycle, what happens if the DJ arms and records
again — does the new take append after the previous one, overwrite it, or start
a fresh timeline?

**Resolution:** Each `arm()` starts a fresh take from timeline `0`; there is no
append or multi-take management in this Epic. EPIC-0009 captures a single
performance into the `daw` model; take management, comping, and append-record are
arrangement-editing features that belong to EPIC-0010. For this PRD, a second
`arm()` resets the playhead to `0` and `timelineOrigin` is re-established on the
next `beginCapture()`. Whether a prior take's clips are cleared on re-arm is a
PRD-0073 / EPIC-0010 concern (clip model ownership), not a clock concern; this
PRD only guarantees that the *clock* restarts cleanly from `0`. Surfacing a
"clear previous take?" decision is deferred to the UI/editing Epics.

### 1.5.7. Relationship to DAW Play Transport

A full DAW has a shared transport where Play and Record move the same playhead.
EPIC-0010 will introduce DAW Play (arrangement playback). Should this PRD's
record playhead already account for, or share state with, that future transport?

**Resolution:** No coupling yet. There is no DAW Play transport in EPIC-0009;
the record playhead is the only timeline cursor and it is owned solely by the
`RecordingSessionController`. This PRD deliberately does not introduce a shared
"transport" abstraction, because designing it without the playback requirements
(EPIC-0010) would be speculative. When EPIC-0010 adds playback, it can introduce
a shared transport that both Play and Record drive, and the record playhead
defined here becomes one input to it. For now, keeping the record clock
self-contained avoids premature abstraction and keeps this PRD's contract narrow:
own the recording lifecycle and the record playhead, nothing more.
