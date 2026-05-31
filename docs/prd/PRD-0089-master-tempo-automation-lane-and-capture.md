---
status: Not Implemented
epic: EPIC-0011
depends-on:
  - PRD-0026
  - PRD-0087
  - PRD-0088
---

# 1. PRD-0089: Master-Tempo Automation Lane & Capture

## 1.1. Problem

EPIC-0011's vision is that a DJ's live performance gestures — including tempo
moves — are captured during recording and reproduced on playback. The master
tempo is the most consequential of these gestures: when the DJ nudges the global
BPM up during a transition, every synced deck, the beat grid, and every
grid-aligned clip on the DAW timeline shifts with it. If that move is not
captured, a recorded arrangement replayed later would run at a single static
tempo and the entire mix would drift out of alignment with the performance the
DJ actually played.

The master tempo has exactly one authority in Sonik: `MasterClockManager` owns
`masterBPM` and publishes it through the `MasterClockSnapshot` SeqLock
(PRD-0026). There is no second tempo source anywhere in the application, and the
automation system must not introduce one. The capture layer therefore cannot
"drive" or "fork" the tempo — it can only *observe* the authoritative BPM as it
changes and write those changes down as breakpoints in a lane. Today no such
observation exists: the automation data model (PRD-0087) defines lane and
breakpoint structures, and the recording lifecycle (PRD-0071/PRD-0088) defines
when capture is armed, but nothing connects a `masterBPM` change to a breakpoint
append on the `master.tempo` lane.

There is also a definitional gap. Master BPM changes originate from several
distinct sources — the sync engine matching a master deck's analysed tempo, a
manual master-tempo control the DJ turns, and a deck becoming master and
imposing its own BPM. The capture layer needs an unambiguous rule for which of
these count as recordable tempo automation and which are structural consequences
of other captured events, so the recorded lane reflects the DJ's intent rather
than every incidental tempo recomputation.

## 1.2. Objective

The system provides a master-tempo automation lane and its live capture such
that:

- A single continuous automation lane keyed `(master, tempo)` exists in the
  `daw` ValueTree under the master node, using PRD-0087's continuous-lane
  structure: ordered breakpoints of `(timelineSample, value, interpolation)`
  where `value` is the master BPM stored as a raw `double` and the default
  interpolation is linear.
- While recording is armed and running (PRD-0071/PRD-0088), every change to the
  authoritative `masterBPM` owned by `MasterClockManager` (PRD-0026) is recorded
  as a breakpoint `(timelineSample, bpm)` appended to the `master.tempo` lane
  via the EPIC-0009 event bridge on the message thread.
- Capture *observes* `masterBPM`; it never writes to `MasterClockManager`, never
  computes its own tempo, and never becomes a second tempo source. The single
  authority invariant (EPIC-0011 §1.3.3, PRD-0026) is preserved.
- At record start, the lane is seeded with an initial breakpoint capturing the
  master BPM in force at the moment recording begins, so playback (PRD-0092) has
  a defined tempo from `timelineSample = 0` of the recording.
- The capture tap classifies the *origin* of each `masterBPM` change and records
  only those changes that represent tempo automation (manual master-tempo
  moves and sync-driven master tempo motion), per the resolutions in §1.5.
- Dense streams of tempo changes (e.g. a continuous ramp delivered as many small
  steps) are thinned so the lane stores a faithful but compact breakpoint set
  rather than one breakpoint per sub-BPM increment, per §1.5.
- The lane definition and capture contract are testable end-to-end via synthetic
  `masterBPM` changes injected on the message thread while a mock recording
  session is armed, asserting the resulting breakpoint set.

This PRD owns the **lane definition** and **capture** only. Playback application
of the `master.tempo` lane back onto `MasterClockManager` is PRD-0092 and is
explicitly out of scope here.

## 1.3. User Flow

1. The DJ has two tracks loaded; Deck A is master at 124.0 BPM. The DJ arms the
   DAW recording (PRD-0071/PRD-0088). At the instant recording starts, the
   capture layer reads the current authoritative `masterBPM` (124.0) from the
   `MasterClockSnapshot` / master ValueTree state and seeds the `master.tempo`
   lane with an initial breakpoint `(timelineSample = recordStart, bpm = 124.0)`.
2. The DJ turns the manual master-tempo control, raising the global BPM from
   124.0 to 126.0 over roughly two seconds. `MasterClockManager` updates
   `masterBPM` repeatedly as the control moves. The capture tap observes each
   change on the message thread, thins the stream, and appends breakpoints to
   the `master.tempo` lane at the sample positions where the changes landed —
   ending with a breakpoint at `(t, 126.0)`.
3. The DJ activates SYNC on Deck B; Deck B follows the master and no new
   `masterBPM` value is produced (the master tempo is unchanged), so no tempo
   breakpoint is recorded. The follower's speed adjustment is not master-tempo
   automation and is not captured by this lane.
4. The DJ presses MASTER on Deck B, whose analysed BPM is 128.0.
   `MasterClockManager` reassigns master and publishes a new snapshot with
   `masterBPM = 128.0`. Per §1.5, this master-handover BPM jump is recorded as a
   single step breakpoint at the handover sample so the recorded tempo timeline
   stays faithful to what the grid actually did.
5. The DJ pauses the master deck. `MasterClockManager` writes
   `masterIsPlaying = false` but does **not** change `masterBPM` (PRD-0026 §1.3
   step 7). Because the BPM value did not change, no tempo breakpoint is
   appended.
6. The DJ stops recording (PRD-0088). The capture tap is disarmed; no further
   breakpoints are appended. The `master.tempo` lane now holds the seed
   breakpoint plus the captured tempo moves, ready for PRD-0092 to drive the
   master clock on playback and for PRD-0093 to render.

### 1.3.1. Edge Cases

- If the DJ never touches the tempo during the entire recording, the lane
  contains exactly the single seed breakpoint from step 1, giving playback a
  flat, well-defined tempo equal to the BPM at record start.
- If recording is armed while the clock is dormant (`masterDeckIndex = -1`),
  the seed breakpoint uses the last-known non-zero `masterBPM` that PRD-0026
  retains in dormant state, so the lane is never seeded with `0.0`.
- If two `masterBPM` changes land on the same `timelineSample` (multiple updates
  within one capture tick), the thinning logic collapses them to the last value
  at that sample so the lane never holds two breakpoints at one timeline
  position.
- If the DJ disarms and re-arms recording within one session, each new recording
  region produces its own seed breakpoint at its own `timelineSample`; this PRD
  does not merge regions (region lifecycle is owned by PRD-0088).

## 1.4. Acceptance Criteria

- [ ] A single continuous automation lane keyed `(master, tempo)` is defined
      using PRD-0087's continuous-lane structure, stored under the master node of
      the `daw` ValueTree, with breakpoint fields `(timelineSample (int64_t),
      value (double), interpolation)` and a default interpolation of linear.
- [ ] The lane's `value` field stores the master BPM as a raw `double` in BPM
      units (no normalisation, no scaling, no unit conversion).
- [ ] A capture tap observes the authoritative `masterBPM` owned by
      `MasterClockManager` (PRD-0026) — via the master ValueTree state and/or the
      `MasterClockSnapshot` — and never writes to `MasterClockManager` or any
      tempo source.
- [ ] The capture tap appends breakpoints to the `master.tempo` lane exclusively
      through the EPIC-0009 event bridge on the message thread; no breakpoint is
      ever appended from the audio thread.
- [ ] When recording starts (PRD-0071/PRD-0088), the lane is seeded with exactly
      one breakpoint at the record-start `timelineSample` whose value is the
      master BPM in force at that instant.
- [ ] If the clock is dormant at record start, the seed breakpoint uses the
      last-known non-zero `masterBPM` retained by PRD-0026, never `0.0`.
- [ ] While recording is armed and running, a change to `masterBPM` that the tap
      classifies as tempo automation (per §1.5.3) appends a breakpoint
      `(timelineSample, bpm)` at the sample position of the change.
- [ ] A `masterBPM` change classified as a follower-side or non-automation event
      (per §1.5.3) does not append a breakpoint.
- [ ] A master handover that changes `masterBPM` (a deck becoming master with a
      different analysed BPM) is recorded as a single breakpoint at the handover
      sample, per §1.5.5.
- [ ] A pause / resume of the master deck that leaves `masterBPM` unchanged
      (PRD-0026 §1.3) appends no breakpoint.
- [ ] Dense tempo changes are thinned (per §1.5.4) so the lane stores a compact
      breakpoint set; two breakpoints never occupy the same `timelineSample`.
- [ ] When capture is disarmed (recording stopped, PRD-0088), no further
      breakpoints are appended to the lane.
- [ ] No playback application of the `master.tempo` lane is implemented by this
      PRD; the lane is captured but not yet read back to drive the clock
      (PRD-0092).
- [ ] No new tempo authority is introduced: the only writer of `masterBPM`
      remains `MasterClockManager`; this PRD's code only reads it.
- [ ] No audio-thread code is added by this PRD; the capture tap runs on the
      message thread and performs no allocations, locks, or I/O on the audio
      thread.
- [ ] At least one test under `Tests/` injects synthetic `masterBPM` changes on
      the message thread while a mock recording session is armed and asserts the
      resulting `master.tempo` breakpoint set, including the seed breakpoint, a
      captured ramp (post-thinning), and a master-handover step.

## 1.5. Grey Areas

### 1.5.1. BPM Value Range and Resolution

The lane stores BPM as a `double`, but the captured value range and the
resolution at which changes are considered "a change worth recording" are not
defined by PRD-0087 (which is value-type agnostic).

**Resolution:** Store the raw `double` BPM with no clamping in the lane itself —
the lane is a faithful record of whatever `MasterClockManager` published, and
range enforcement is the authority's job, not the recorder's. For *change
detection*, treat two BPM values as equal if they differ by less than an epsilon
of `0.001` BPM; changes below this threshold are noise from floating-point
recomputation and are not recorded. This keeps the lane honest (it mirrors the
authority) while preventing sub-millibeat jitter from polluting the breakpoint
set. The display/range concerns (min/max BPM shown on the lane) belong to the
rendering PRD (PRD-0093), not here.

### 1.5.2. Units Stored: Raw BPM vs Normalised

Per-channel continuous lanes (PRD-0090) store normalised `0..1` parameter
values. The tempo lane could follow that convention for uniformity, or store raw
BPM for fidelity to the single source of truth.

**Resolution:** Store **raw BPM** (`double`, BPM units). The master tempo has no
natural `0..1` normalisation — there is no fixed min/max the way an EQ knob has —
and PRD-0092 will write the captured value straight back to
`MasterClockManager`, which speaks raw BPM. Storing normalised values would
require an arbitrary BPM range to denormalise against on playback, introducing a
second implicit "tempo range" authority and a lossy round-trip. Raw BPM keeps the
capture → playback path exact and aligned with EPIC-0011 §1.3.1 ("tempo in BPM
from EPIC-0003"). The lane model (PRD-0087) is value-type agnostic, so a raw-BPM
continuous lane coexists with normalised per-channel lanes without migration.

### 1.5.3. Which `masterBPM` Changes Count as Automation

`masterBPM` can change because (a) the DJ turns a manual master-tempo control,
(b) the sync engine moves the master tempo, or (c) a deck becomes master and
imposes its analysed BPM. Recording *every* recomputation would capture
incidental motion the DJ did not intend as a gesture; recording too little would
lose real tempo rides.

**Resolution:** The lane records a master-BPM change whenever the authoritative
`masterBPM` value actually changes (beyond the §1.5.1 epsilon) while recording
is armed — regardless of which UI control triggered it — with two scoping rules:
(1) **follower-side speed adjustments do not count**: a slave deck's speed
multiplier changing to match an unchanged master tempo produces no `masterBPM`
change and therefore no breakpoint, which falls out naturally because only the
*master* BPM is observed; (2) **master handovers are recorded** as a single step
(see §1.5.5). This rule is deliberately origin-agnostic about manual vs
sync-driven master-tempo motion: both are genuine changes to the global tempo
the grid follows, both should be reproduced on playback, and the capture layer
intentionally observes the *authoritative resulting value* rather than the
input device (consistent with EPIC-0011 §1.2.2, which captures from resulting
parameter changes, not from raw MIDI). The DJ's intent is encoded in the master
BPM itself; the recorder is faithful to that single number.

### 1.5.4. Thinning Dense Tempo Changes

A continuous tempo ramp (manual sweep or a sync ramp) arrives as many small
`masterBPM` updates. Storing one breakpoint per update would bloat the lane with
near-collinear points and produce visually noisy curves on render (PRD-0093).

**Resolution:** Apply two cheap thinning passes at capture time. First,
**same-sample coalescing**: if a new change lands on a `timelineSample` already
holding a breakpoint, overwrite that breakpoint's value rather than adding a
second (guarantees one breakpoint per sample). Second, **collinearity /
deadband thinning**: drop an incoming breakpoint if it lies within a small
tolerance of the linear interpolation between the previous two breakpoints
(i.e. it adds no shape), using a deadband of `0.05` BPM. This preserves the
endpoints and inflection points of a ramp while discarding redundant
intermediate samples. Thinning is intentionally lossy in a bounded, perceptually
negligible way; the standard linear interpolation between the retained
breakpoints reconstructs the ramp faithfully on playback. Heavier curve
simplification (e.g. Douglas–Peucker) is deferred to editing (PRD-0094) and is
not needed for capture.

### 1.5.5. First Breakpoint at Record Start (Initial-BPM Snapshot)

If the lane is empty until the DJ first touches the tempo, playback (PRD-0092)
has no defined tempo from `timelineSample = 0` of the recording until the first
captured move — leaving an undefined leading region.

**Resolution:** Always seed the lane with an initial breakpoint at the
record-start sample whose value is the master BPM in force at that instant
(snapshotting the authoritative `masterBPM`). This guarantees the lane defines a
tempo for the entire recorded region from its first sample, makes playback
deterministic, and matches how the grid actually sounded at the start of the
recording. The seed is captured from the same authoritative source as every
subsequent breakpoint, so there is no special "initial value" code path beyond
emitting one breakpoint at arm time. In dormant state the seed uses the
last-known non-zero BPM (§1.5.1 / PRD-0026 dormant policy), never `0.0`.

### 1.5.6. Deck-Driven Master Tempo (a Deck Becoming Master)

When the DJ presses MASTER on a different deck, `MasterClockManager` publishes
that deck's analysed BPM, which is typically a discontinuous jump (e.g. 124.0 →
128.0). Is that jump tempo automation, or a structural consequence of a master
handover that should be captured elsewhere?

**Resolution:** Record it as tempo automation — a single **step** breakpoint at
the handover sample. The master grid genuinely jumps to the new BPM at that
moment, and PRD-0092 must reproduce that jump for the recorded arrangement to
stay aligned; therefore it belongs in the `master.tempo` lane. To avoid implying
a tempo *ramp* across the discontinuity, the handover breakpoint and its
predecessor use **step/hold** interpolation for that single segment (the prior
tempo holds until the handover sample, then steps), rather than the default
linear interpolation that would draw a slope between the two BPM values. The
*identity* of which deck is master is structural metadata owned by EPIC-0009's
structural capture, not by this lane; this lane records only the resulting BPM
value and its discontinuity. This keeps the tempo lane a pure value timeline
while remaining faithful to the audible result of the handover.

### 1.5.7. Interaction with Pause / Resume and Dormant Transitions

Pausing the master deck sets `masterIsPlaying = false` without changing
`masterBPM` (PRD-0026 §1.3). A naive "publish on every snapshot" tap could
mistake the snapshot churn for a tempo change and emit spurious breakpoints.

**Resolution:** The tap keys exclusively off the `masterBPM` *value*, not off
snapshot publication or `masterIsPlaying` transitions. A snapshot whose
`masterBPM` equals the last recorded value (within the §1.5.1 epsilon) appends
nothing, so pause, resume, and play-state churn that preserve BPM produce no
breakpoints. Only an actual BPM delta crosses the threshold and is recorded.
This makes the capture robust to the many non-tempo reasons a new
`MasterClockSnapshot` is published, and keeps the lane a clean record of tempo
motion alone.
