---
status: Implemented
epic: EPIC-0011
depends-on:
  - PRD-0072
  - PRD-0071
  - PRD-0087
---

# 1. PRD-0088: Automation Capture Taps

## 1.1. Problem

EPIC-0011 (§1.3.2) is explicit that automation must be captured *through the
existing state, not a side channel*: while recording, a change to one of the
automated parameters must append a breakpoint or step to the matching automation
lane, and that captured automation must be *exactly* what the DJ did, with no
divergence between the live control and the recorded lane.

By the time this PRD is reached, the pieces exist but nothing connects them. The
automation data model (PRD-0087) defines the lanes — continuous lanes keyed by
`(channelGroup | master, parameterId)` holding ordered breakpoints, and boolean
lanes holding ordered step events — but no code writes into them during a
recording. The EPIC-0009 event bridge (PRD-0072) provides the message-thread
mechanism for appending captured events to the `daw` `ValueTree` coherently with
the rest of the structural capture. The recording-armed state (PRD-0071) tells
the system *when* capture is active. The authoritative parameters themselves
already exist and are already written by the live UI and MIDI: the mixer
`ValueTree` params (`mixer.channel.{A,B,C,D}.filter|eq.high|eq.mid|eq.low|gain`,
EPIC-0007 / PRD-0052+), the master clock BPM (EPIC-0003 / PRD-0026), and the
deck booleans key-lock / pitch-stretch / key-stepper (EPIC-0001 / PRD-0025).

What is missing is the *observation layer* that sits on top of those
authoritative parameters and, while a recording is in progress, translates each
change into an append to the correct lane via the event bridge. Without it,
either nothing is captured, or — far worse — a separate capture path duplicates
the parameter values and drifts out of sync with what the DJ actually heard. The
risk this PRD removes is precisely that divergence: a captured sweep that does
not match the live sweep because the capture observed a copy instead of the real
parameter.

## 1.2. Objective

The system provides generic *automation capture taps* — a reusable observation
and append mechanism under `Source/Features/Daw/Automation/AutomationCaptureTaps`
— such that:

- Each automated parameter is observed at its **authoritative** `ValueTree`
  source: the exact property the live UI and MIDI already write to. No copy, no
  mirror, no parallel value is introduced for the purpose of capture.
- A tap is registered per parameter id, mapping a source `ValueTree` property to
  the matching automation lane key `(channelGroup | master, parameterId)` defined
  by PRD-0087.
- **While, and only while, recording is armed** (PRD-0071), a change to an
  observed parameter appends an event to the matching lane through the EPIC-0009
  event bridge (PRD-0072), on the message thread.
- A change to a **continuous** parameter (mixer filter / EQ high / EQ mid /
  EQ low / gain; master tempo BPM) appends a *breakpoint* `(timelineSample,
  value, interpolation)` to the matching continuous lane.
- A change to a **boolean** parameter (deck key-lock / pitch-stretch /
  key-stepper) appends a *step event* to the matching boolean lane.
- The timeline timestamp of every appended event is the **record playhead at the
  instant of the change**, so the captured event lands at the position the gesture
  actually occurred.
- When recording is **not** armed, taps observe nothing into the model: parameter
  changes flow to the live audio path exactly as today, with zero automation
  writes and zero added latency.
- The capture path runs entirely on the **message thread**; it adds no
  audio-thread code, takes no locks on the audio thread, and never writes the
  `daw` `ValueTree` from `processBlock`.
- This PRD delivers the **generic** capture-tap infrastructure only. The
  parameter-specific lane wiring plugs in via later PRDs: master-tempo
  (PRD-0089), per-channel continuous (PRD-0090), and per-channel boolean
  (PRD-0091).

## 1.3. Developer / Integration Flow

1. A new component pair `AutomationCaptureTaps.h` / `AutomationCaptureTaps.cpp`
   is added under `Source/Features/Daw/Automation/`. It owns a registry of taps,
   each describing: the source `ValueTree` and property id to observe, the lane
   key `(channelGroup | master, parameterId)` to append to, and the lane shape
   (continuous breakpoint vs boolean step).
2. `AutomationCaptureTaps` is constructed with explicit dependencies (no
   singletons, per `AGENTS.md`): the source `ValueTree`s that hold the
   authoritative params (mixer state, master-clock state, the deck `ValueTree`s),
   the PRD-0071 recording-armed state, the PRD-0072 event bridge, and the
   PRD-0087 automation model accessor. Dependencies are passed via the
   constructor.
3. Each tap is registered by parameter id (§1.5.7). A registration entry binds
   one authoritative source property to one lane key. The registration table is
   the single place that knows "this property feeds that lane"; the
   parameter-specific PRDs (0089/0090/0091) populate it with their concrete ids.
4. Observation is implemented by attaching a `juce::ValueTree::Listener` per
   observed sub-tree (§1.5.1), so that a property change on an authoritative
   param fires `valueTreePropertyChanged`. The tap looks up whether the changed
   property is registered; if not, it ignores it.
5. On a registered change, the tap first checks the PRD-0071 recording-armed
   state. If recording is not armed, it returns immediately (no append). This is
   the only-while-recording gate (§1.5.5).
6. If recording is armed, the tap reads the **current authoritative value** from
   the property that just changed (never a cached copy), captures the **record
   playhead timeline sample** for the change instant (§1.5.4), and constructs the
   append: a breakpoint for continuous lanes, a step for boolean lanes.
7. The append is dispatched through the EPIC-0009 event bridge (PRD-0072) so it
   lands in the `daw` `ValueTree` on the message thread, coherently with the rest
   of structural capture. The tap never mutates the automation model directly; it
   always goes through the bridge.
8. Continuous changes are coalesced / thinned (§1.5.2, §1.5.6) before append so a
   single physical sweep produces a sane number of breakpoints rather than
   thousands. Boolean steps are not thinned (every toggle matters).
9. A new test file `Tests/AutomationCaptureTapsTests.cpp` drives the tap with a
   synthetic source `ValueTree`, a synthetic armed-state, a fake playhead, and a
   recording event-bridge spy. It asserts: no append when disarmed; a breakpoint
   appended at the correct timeline sample and value when a continuous param
   changes while armed; a step appended when a boolean param toggles while armed;
   and correct thinning/coalescing of a rapid continuous sweep.

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Daw/Automation/AutomationCaptureTaps.{h,cpp}` exists and
      compiles into the app and the test target.
- [ ] `AutomationCaptureTaps` receives all dependencies (source `ValueTree`s,
      recording-armed state from PRD-0071, event bridge from PRD-0072, automation
      model accessor from PRD-0087) via its constructor; it contains no singleton
      access and no global mutable state.
- [ ] A tap can be **registered per parameter id**, binding one authoritative
      `ValueTree` property to one lane key `(channelGroup | master, parameterId)`
      and a lane shape (continuous vs boolean).
- [ ] Each tap observes the **authoritative** property — the same property the
      live UI and MIDI write to — and reads the current value directly from it on
      change; no copy or mirror value is created for capture purposes.
- [ ] When recording is **not** armed (PRD-0071), a change to any observed
      parameter produces **zero** appends to the automation model.
- [ ] When recording **is** armed, a change to a continuous-shaped param
      (mixer `filter`/`eq.high`/`eq.mid`/`eq.low`/`gain`; master BPM) appends a
      breakpoint `(timelineSample, value, interpolation)` to the matching
      continuous lane via the PRD-0072 event bridge.
- [ ] When recording **is** armed, a toggle of a boolean-shaped param (deck
      `key-lock`/`pitch-stretch`/`key-stepper`) appends a step event to the
      matching boolean lane via the PRD-0072 event bridge.
- [ ] The appended event's `timelineSample` equals the **record playhead at the
      change instant**; a change at playhead `S` appends at `S` (within the
      tolerance defined in §1.5.4).
- [ ] All appends are dispatched through the EPIC-0009 event bridge (PRD-0072);
      `AutomationCaptureTaps` never mutates the PRD-0087 model directly.
- [ ] Rapid continuous changes are **coalesced / thinned** (§1.5.2, §1.5.6) so a
      single sweep produces a bounded breakpoint count rather than one breakpoint
      per raw `ValueTree` change; boolean steps are **never** thinned.
- [ ] No audio-thread code is added: all observation, gating, timestamping, and
      appending happen on the message thread. `processBlock` is unchanged; no
      allocation, lock, or I/O is added to any audio-thread path.
- [ ] No automation lane *type* logic is duplicated here; lane shape and storage
      come from PRD-0087. This PRD only routes changes to the correct lane.
- [ ] The parameter-specific registration for tempo (PRD-0089), continuous
      (PRD-0090), and boolean (PRD-0091) plugs into the registration table
      without changing the generic tap contract.
- [ ] `Tests/AutomationCaptureTapsTests.cpp` covers, at minimum: disarmed →
      no append; armed continuous change → one breakpoint at correct sample/value;
      armed boolean toggle → one step; rapid armed sweep → thinned breakpoint
      count within the configured bound.
- [ ] The playback applier (PRD-0092), the lane UI (PRD-0093), and lane editing
      (PRD-0094) are **not** touched by this PRD.

## 1.5. Grey Areas

### 1.5.1. Observation Mechanism: Per-Param Listener vs Central Change Hub

Taps could observe parameters either by attaching a `juce::ValueTree::Listener`
to each observed sub-tree, or by subscribing to a central "parameter changed"
hub that the mixer / clock / deck already emit through.

**Resolution:** Attach a `juce::ValueTree::Listener` per observed sub-tree (one
listener per mixer-channel node, one for the master-clock node, one per deck
node), and route `valueTreePropertyChanged` through the registration table.
This is the lowest-coupling option: it observes the *authoritative* state
directly (satisfying EPIC-0011 §1.3.2 with no intermediary), requires no new
broadcast contract from EPIC-0001/0003/0007, and naturally ignores unregistered
properties. A central hub would add a new cross-Epic interface that every source
feature must opt into and keep in sync — more surface area, more drift risk, and
a second thing that can diverge from the real param. The listener approach keeps
the source-of-truth invariant intact: the only value the tap ever sees is the
one already committed to the authoritative tree.

### 1.5.2. Breakpoint Thinning / Decimation for Continuous Lanes

A continuous knob dragged through a sweep can emit hundreds of `ValueTree`
property changes per second. Recording every raw change as a breakpoint produces
thousands of near-collinear points: heavy to store, slow to edit, and visually
noisy in the lane. The alternative — recording every change verbatim — is the
most faithful but impractical.

**Resolution:** Thin continuous captures. Append a breakpoint only when the new
value departs from the last-appended breakpoint by more than a small value
threshold (deadband), or when a minimum time has elapsed since the last
breakpoint, whichever the configured policy dictates. The threshold is chosen so
a human-speed sweep yields a smooth, faithful curve while collapsing
imperceptible jitter. Faithfulness is preserved at the perceptual level: the
captured curve, when replayed and smoothed (EPIC-0007 / PRD-0092), is
indistinguishable from the live gesture. Boolean lanes are exempt — every toggle
is semantically significant and is always recorded. The thinning policy lives in
this generic tap so the parameter-specific PRDs inherit it rather than each
re-inventing it.

### 1.5.3. Capture Cadence vs Source-of-Truth Change Events

Capture could run on a fixed timer (poll each observed param every N ms and
append if changed) or be purely **event-driven** (append in response to the
authoritative param's own change notification).

**Resolution:** Event-driven, off the authoritative param's change
notification — never a poll. Polling would (a) sample at a cadence unrelated to
when the gesture actually moved, smearing the timestamp; (b) risk missing fast
transitions between polls; and (c) burn cycles when nothing changes. Driving
capture from the real change event means the captured timeline position is the
position the parameter actually moved, and capture cost is exactly proportional
to DJ activity. Thinning (§1.5.2) and coalescing (§1.5.6) bound the cost of
bursty events; they are decimation *of real events*, not a sampling clock.

### 1.5.4. Timeline Timestamp: Record Playhead at the Change Instant

The appended event needs a timeline sample. Candidates: the record playhead read
at the moment the change notification is handled, a timestamp carried on the
change itself, or the playhead at the next bridge flush.

**Resolution:** Use the **record playhead at the change instant** — read at the
moment the tap handles the authoritative change notification, before any bridge
queueing. Because capture is event-driven on the message thread (§1.5.3), the
handling instant is effectively the gesture instant; reading the playhead there
yields the position the DJ actually moved the control. Using the playhead at a
later flush would shift events by the flush latency and bunch coalesced bursts
onto the same sample. A small, bounded tolerance (sub-block) is acceptable and is
the same order as the message-thread scheduling granularity; tighter sample
accuracy is unnecessary for human-speed automation gestures and is reserved for
the audio-thread snapshot path (PRD-0092 / EPIC-0010).

### 1.5.5. Only-While-Recording Gating

Taps observe the authoritative params at all times (the listeners are always
attached), but must only append when recording is armed. The gate could live in
the tap (check armed-state on every change) or be implemented by attaching /
detaching listeners on arm / disarm.

**Resolution:** Keep listeners permanently attached and gate on the PRD-0071
recording-armed state inside the change handler: if disarmed, return before any
timestamp read or append. Permanently-attached listeners avoid the race and
bookkeeping of attaching exactly at the arm edge (and the risk of missing the
first change right after arming). The cost is a single boolean check per
observed change while disarmed, which is negligible. This guarantees the
disarmed path is a no-op against the model (acceptance criterion) while keeping
the wiring simple and edge-safe.

### 1.5.6. Coalescing Rapid Continuous Changes

Even with the §1.5.2 deadband, a fast sweep can still produce closely-spaced
changes within a single message-thread cycle or bridge window. These could each
become a separate breakpoint, or be coalesced to the latest value within the
window.

**Resolution:** Coalesce rapid continuous changes within a short window to the
latest value before append: when multiple qualifying changes for the same lane
arrive within the same coalescing window, only the most recent value is appended
at the most recent playhead. This complements thinning (which is value-based) by
bounding the *temporal* density of breakpoints and preventing duplicate points
at the same timeline sample. Boolean steps are never coalesced — two distinct
toggles are two distinct events and must both survive. Coalescing is applied per
lane key so independent parameters moving simultaneously each get their own
faithful stream.

### 1.5.7. Tap Registration Per Parameter Id

The mapping from authoritative property to lane could be hard-coded in the tap,
or expressed as a registration table populated by the parameter-specific PRDs.

**Resolution:** A registration table keyed by parameter id, populated externally
by the parameter-specific PRDs (master-tempo 0089, continuous 0090, boolean
0091). Each entry binds one authoritative `(ValueTree, propertyId)` to one lane
key `(channelGroup | master, parameterId)` plus a lane shape. This keeps the
generic tap free of any knowledge of *which* concrete params exist: it knows only
how to observe, gate, timestamp, thin, coalesce, and append. New automated
parameters (the future crossfader/fader enhancement noted in EPIC-0011 §1.2.2)
plug in by adding a registration entry, with no change to the generic contract or
the model. The table is the single source of the property-to-lane mapping,
mirroring the per-target registry discipline already used elsewhere in the
codebase (e.g. PRD-0042's control target registry).
