---
status: Implemented
epic: EPIC-0011
depends-on:
  - PRD-0026
  - PRD-0079
  - PRD-0082
  - PRD-0087
---

# 1. PRD-0092: Automation Playback Applier

## 1.1. Problem

The automation data model (PRD-0087) stores, for every channel group and the
master, ordered breakpoint curves (continuous lanes: filter, high, mid, low,
gain, master tempo) and ordered step events (boolean lanes: key-lock,
pitch-stretch, key-stepper). The capture stack (PRD-0088 through PRD-0091)
fills those lanes with exactly what the DJ did while recording. The DAW
timeline transport (PRD-0082) advances a playhead across the arrangement during
playback. But nothing yet *reproduces* the recorded gestures: at playback time
the lanes are inert data. There is no component that evaluates each lane at the
moving playhead and re-applies the resulting value to the live system so the mix
sounds the way it was performed.

The naive implementation would be to read the lane value and poke the DSP or the
clock directly — a parallel "automation back door" that bypasses the same state
paths the live UI and MIDI controls already write through. That would be a
disaster for this codebase. Sonik's entire architecture (CLAUDE.md, EPIC-0007)
rests on a single source of truth: continuous mixer parameters flow through the
mixer `ValueTree` (smoothed, click-free), the master tempo is owned by exactly
one authority (`MasterClockManager`, PRD-0026), and deck booleans flow through
the deck `ValueTree`. If automation forked any of these — wrote tempo somewhere
other than `MasterClockManager`, or pushed a raw filter value past the EPIC-0007
smoother — the grid, synced decks, grid-aligned clips, MIDI LED feedback, and
the on-screen controls would all desynchronise from what the automation is
doing, and continuous moves would produce audible zipper noise and clicks.

There is also a thread-safety hazard. Some automated parameters tolerate
message-thread cadence (a filter sweep updated every few milliseconds and then
smoothed sounds perfectly continuous); others — anything the render engine must
read sample-accurately at a clip boundary — need their values delivered through
the audio-thread arrangement snapshot (PRD-0079), never evaluated by walking
lanes inside `processBlock`. The applier must straddle both delivery paths
correctly without ever allocating, locking, or doing I/O on the audio thread.

## 1.2. Objective

A new automation playback applier
(`Source/Features/Daw/Automation/AutomationApplier.h/.cpp`) drives the live
system from recorded automation during timeline playback, such that:

- During DAW timeline playback (PRD-0082), every **enabled** (non-bypassed)
  automation lane (PRD-0087) is evaluated at the current transport playhead and
  the resulting value is written to the correct target **through the same
  single-source-of-truth path the live controls use** — never a parallel back
  door.
- Continuous mixer parameters (`mixer.channel.*.filter|eq.high|eq.mid|eq.low|gain`)
  are written to the **mixer `ValueTree` params**, where they pass through the
  existing EPIC-0007 parameter smoothing and reach the DSP click-free.
- Master-tempo automation is written to **`MasterClockManager`** (PRD-0026) as
  the one and only tempo authority, so the grid, synced decks, and grid-aligned
  clips all follow the recorded tempo from a single source. The DAW never forks
  tempo and never writes a second tempo value anywhere else.
- Deck booleans (key-lock, pitch-stretch, key-stepper) are written to the
  **deck `ValueTree` params**, the same properties the deck UI and MIDI toggle.
- The applier runs on the **message thread at a high cadence** for parameters
  that tolerate it, **and/or** publishes values into the **audio-thread
  arrangement snapshot** (PRD-0079) for parameters that require sample-accurate,
  click-free delivery. No automation value is ever evaluated or written on the
  audio thread; the audio thread only reads published snapshots.
- Continuous values are **smoothed to avoid zipper noise**, consistent with and
  reusing EPIC-0007's parameter smoothing rather than introducing a second
  conflicting smoother.
- When a lane is bypassed (per-lane enable from PRD-0087), the applier writes
  nothing for that lane and the live control retains whatever value it currently
  holds; re-enabling resumes application at the next evaluation.
- When automation is *not* playing (transport stopped, or no enabled lanes), the
  applier is inert and the live controls behave exactly as they do today.

Out of scope: capture (PRD-0088 through PRD-0091), the data model itself
(PRD-0087), the automation lane UI / rendering (PRD-0093), and automation
editing (PRD-0094).

## 1.3. Developer / Integration Flow

1. A new `AutomationApplier` class is created under
   `Source/Features/Daw/Automation/`. It is constructed with explicit
   dependencies (no singletons, per CLAUDE.md): a reference to the automation
   model (PRD-0087), the mixer `ValueTree` state, the per-deck `ValueTree`
   state, the `MasterClockManager` (PRD-0026), the DAW transport (PRD-0082), and
   the arrangement-snapshot publisher (PRD-0079).
2. The applier registers a message-thread callback driven at a high, fixed
   cadence (a `juce::Timer` or the existing DAW playback tick from PRD-0082).
   On each tick, if and only if the transport is playing, it reads the current
   playhead sample position from the transport.
3. For each lane in the model, the applier first checks the lane's
   **enabled / bypassed** flag (PRD-0087). Bypassed lanes are skipped entirely —
   no evaluation, no write.
4. For each enabled **continuous** lane, the applier evaluates the breakpoint
   curve at the playhead using the lane's per-segment interpolation mode (linear
   or step/hold, PRD-0087 §1.3.1). The evaluated normalised value is written to
   the lane's target:
   - Mixer continuous params (filter / EQ bands / gain) → the corresponding
     `mixer.channel.*` `ValueTree` property, where EPIC-0007 smoothing already
     applies. The applier does **not** add its own smoother on top; it writes the
     evaluated value at message-thread cadence and lets the existing smoother
     ramp the DSP value.
   - Master tempo → `MasterClockManager::setTempo(...)` (or the equivalent
     existing setter, PRD-0026). This is the single tempo authority; the applier
     never writes tempo to any other location.
5. For each enabled **boolean** lane, the applier evaluates the step events at
   the playhead (the value held from the last step before or at the playhead)
   and writes the boolean to the deck `ValueTree` property (key-lock /
   pitch-stretch / key-stepper) only when the evaluated state differs from the
   property's current value, so redundant writes and listener churn are avoided.
6. For any parameter that the render engine requires **sample-accurately** at a
   clip / segment boundary, the applier instead (or additionally) supplies the
   evaluated value into the **arrangement snapshot** (PRD-0079) published to the
   audio thread via the existing SeqLock mechanism. The audio thread reads the
   published value; it never walks lanes. Which parameters take the snapshot
   path versus the message-thread `ValueTree` path is enumerated in §1.5.1.
7. To prevent a feedback loop into capture (PRD-0088 through PRD-0091), every
   write the applier performs during playback is tagged as automation-originated
   (a re-entrancy guard / "applying" flag) so the capture taps ignore
   playback-driven parameter changes and do not re-record them. Capture only
   records genuine live user gestures.
8. A new test file (`Tests/AutomationApplierTests.cpp`) verifies: lane
   evaluation at a known playhead returns the interpolated value; bypassed lanes
   produce no write; continuous mixer values reach the mixer `ValueTree`; tempo
   values reach `MasterClockManager` and nowhere else; boolean lanes write the
   deck `ValueTree` only on change; the re-entrancy guard prevents capture
   feedback; and the snapshot path publishes the expected sample-accurate value
   for the parameters that require it.

## 1.4. Acceptance Criteria

- [ ] `AutomationApplier` exists under `Source/Features/Daw/Automation/` and is
  constructed via explicit dependency injection (model, mixer `ValueTree`, deck
  `ValueTree`, `MasterClockManager`, DAW transport, arrangement-snapshot
  publisher); it uses no singletons and holds no global mutable state.
- [ ] On each message-thread tick during playback, the applier reads the current
  transport playhead (PRD-0082) and evaluates every enabled automation lane at
  that playhead; when the transport is stopped it performs no evaluation and no
  writes.
- [ ] Continuous mixer lane values (filter, high, mid, low, gain) are written to
  the corresponding `mixer.channel.*` `ValueTree` property — the same path the
  live UI and MIDI write — and reach the DSP through EPIC-0007's existing
  smoothing, click-free. The applier adds no second smoother that would conflict
  with EPIC-0007.
- [ ] Master-tempo lane values are written **only** to `MasterClockManager`
  (PRD-0026); a test asserts that no other tempo field is written by the applier
  and that the grid / synced decks observe the automated tempo from that single
  authority.
- [ ] Deck boolean lane values (key-lock, pitch-stretch, key-stepper) are written
  to the corresponding deck `ValueTree` property, and only when the evaluated
  state differs from the current property value (no redundant writes).
- [ ] Bypassed (disabled) lanes produce no evaluation and no write; the live
  control retains its current value, and re-enabling the lane resumes application
  on the next tick.
- [ ] Continuous lane evaluation honours the per-segment interpolation mode
  (linear and step/hold) defined by PRD-0087; a test asserts the interpolated
  value at a playhead between two breakpoints matches the expected linear or
  step result.
- [ ] Parameters requiring sample-accurate delivery are supplied through the
  PRD-0079 arrangement snapshot (SeqLock-published) and read by the audio thread
  from that snapshot; no automation lane is ever walked, evaluated, or written on
  the audio thread.
- [ ] No audio-thread code added by this PRD allocates memory, takes a lock, or
  performs I/O; all cross-thread communication uses the existing `std::atomic` /
  lock-free snapshot mechanisms.
- [ ] Writes performed by the applier during playback are tagged
  automation-originated so the capture taps (PRD-0088 through PRD-0091) ignore
  them; a test confirms that automation playback does not re-record into the
  lanes (no capture feedback loop).
- [ ] When no lanes are enabled or the transport is stopped, the applier is
  inert and the live controls behave identically to the no-automation baseline
  (a test asserts no `ValueTree` mutation and no `MasterClockManager` call occur
  in this state).
- [ ] `Tests/AutomationApplierTests.cpp` covers continuous mixer write, tempo
  write to `MasterClockManager`, boolean deck write-on-change, bypass gating,
  interpolation correctness, snapshot publication, and capture re-entrancy
  suppression.
- [ ] This PRD adds no automation capture, no data-model structure, no UI, and no
  editing logic; those remain owned by PRD-0087 / PRD-0088–0091 / PRD-0093 /
  PRD-0094 respectively.

## 1.5. Grey Areas

### 1.5.1. Message-Thread Cadence vs Audio-Thread Snapshot per Parameter

Some automated parameters are tolerant of message-thread cadence with smoothing;
others need sample-accurate audio-thread delivery. Writing everything through the
snapshot is more complex; writing everything on the message thread risks
audible imprecision at boundaries.

**Resolution:** Split by need. Continuous **mixer** params (filter, EQ bands,
gain) take the **message-thread `ValueTree`** path: they are perceptually
continuous controls already protected by EPIC-0007 smoothing, so a high-cadence
message-thread write plus the existing smoother is click-free and indistinguishable
from sample-accurate application. **Deck booleans** also take the message-thread
`ValueTree` path (a key-lock toggle is an event, not a per-sample ramp; see
§1.5.7). **Master tempo** is written to `MasterClockManager` on the message
thread (the clock is the authority and already publishes its tempo to the audio
thread coherently — the applier must not duplicate that publication). The
**arrangement-snapshot path (PRD-0079)** is reserved for any value the render
engine must read sample-accurately at a clip/segment boundary — concretely, any
parameter whose exact value at the boundary sample affects how a clip is
rendered. If, during implementation, a mixer param proves to need boundary
sample-accuracy, it is *added* to the snapshot path without removing its
`ValueTree` write; the two are not mutually exclusive. The default for everything
in the enumerated parameter set is the message-thread path; the snapshot path is
opt-in per parameter and documented where used.

### 1.5.2. Tempo Automation Driving the Clock Without Feedback into Capture

The master-tempo lane is captured *from* `MasterClockManager` changes
(PRD-0090) and applied *to* `MasterClockManager` during playback. Without a
guard, applying a recorded tempo would look like a live tempo change and get
re-captured, corrupting the lane.

**Resolution:** A re-entrancy guard. Before the applier writes any value
(tempo, mixer param, or deck boolean) it sets an "applying automation" flag that
the capture taps (PRD-0088–0091) check and respect: any parameter change observed
while the flag is set is treated as automation-originated and is **not**
recorded. The flag is cleared after the write batch. Because both the applier and
the capture taps run on the message thread, the flag is a simple non-atomic bool
(no cross-thread hazard). This guarantees the single tempo authority can be both
the capture source and the playback sink without a feedback loop, and it
generalises to every automated parameter, not just tempo.

### 1.5.3. Enabled / Bypassed Lane Gating

A lane can be bypassed (PRD-0087's per-lane enable). The question is what
"bypassed" means at the moment of bypass: does the live control snap back to a
manual value, hold the last automation value, or stay wherever the user last left
it?

**Resolution:** Bypass means "the applier ignores this lane entirely." It writes
nothing for a bypassed lane, so the target retains whatever value it currently
holds — which is the last automation value if the lane was just bypassed, or the
last manual value if the user has since touched the control. The applier does not
restore a "pre-automation" value and does not snap. Re-enabling resumes
application on the next tick, at which point the control moves to the lane's
evaluated value (smoothed for continuous params, so no click). This is the least
surprising behaviour and requires no stored "manual baseline." Latching a manual
override is handled by §1.5.6, not by bypass.

### 1.5.4. Interpolation Evaluation at the Playhead

Continuous lanes carry a per-segment interpolation mode (linear or step/hold,
PRD-0087). The applier must evaluate the curve at an arbitrary playhead sample
that usually falls between two breakpoints.

**Resolution:** Evaluate using the interpolation mode of the segment that
*ends* at the next breakpoint (i.e., the mode recorded on the left breakpoint of
the segment, per PRD-0087's convention). For a **linear** segment, the value is
the linear interpolation between the two bounding breakpoints by sample position.
For a **step/hold** segment, the value is the left breakpoint's value held until
the next breakpoint. Before the first breakpoint, the value is the first
breakpoint's value (held); after the last breakpoint, the value is the last
breakpoint's value (held). Evaluation is a pure function of `(lane, playheadSample)`
with no side effects, which makes it directly unit-testable and reusable by the
UI renderer (PRD-0093) and the snapshot publisher alike.

### 1.5.5. Smoothing and Conflict with EPIC-0007 Smoothing

Continuous automation written at message-thread cadence could produce zipper
noise if applied raw, but adding a second smoother on top of EPIC-0007's existing
parameter smoothing would double-smooth (sluggish, laggy response) or fight it.

**Resolution:** Reuse EPIC-0007 smoothing; do **not** add a second smoother for
mixer params. The applier writes the evaluated value to the mixer `ValueTree` at a
high cadence and relies on the DSP-side smoother that already protects those exact
parameters from live UI/MIDI moves. Because the automation cadence is comparable
to a fast manual knob turn, the existing smoother handles it identically — the
audio engine cannot tell an automated filter sweep from a hand-performed one,
which is precisely the single-source-of-truth goal. For any value delivered via
the PRD-0079 snapshot, smoothing is whatever that snapshot path already provides
for boundary-accurate values; the applier supplies evaluated values, not ramps.
Tempo changes are smoothed/ramped by `MasterClockManager` per its own existing
contract (PRD-0026), not by the applier.

### 1.5.6. Conflict Between Live Control and Automation During Playback

If the DJ touches a control (mouse or MIDI) while automation is playing that same
lane, who wins — does automation keep overwriting the live move (automation
wins), or does the live touch take over until playback restarts (latch)?

**Resolution:** Automation wins by default for this PRD. While a lane is enabled
and the transport is playing, the applier authoritatively writes that lane's value
every tick; a simultaneous live touch is immediately overwritten on the next tick.
This keeps playback faithful to the recording and keeps behaviour simple and
predictable. A **latch / write-takeover** mode (live touch suspends the lane until
playback stops or the user re-arms) is a natural enhancement but belongs with the
editing/recording Epic surface, not the playback applier; it would be implemented
by having the live-touch path set a per-lane "suspended" flag that the applier
honours exactly like bypass (§1.5.3). The model already accommodates this without
migration, so deferring it costs nothing. For now: enabled lane + playing
transport ⇒ automation is the source of truth for that parameter.

### 1.5.7. Boolean Apply (Key-Lock Toggle) Click-Free Timing

A boolean parameter such as key-lock or pitch-stretch can cause an audible
discontinuity (a DSP mode switch) if toggled at an arbitrary sample. Continuous
smoothing does not apply to a boolean.

**Resolution:** The applier writes the boolean to the deck `ValueTree` exactly as
the live UI/MIDI toggle does, and only on change (§1.3 step 5). Click-free
handling of the *mode switch itself* is the responsibility of the underlying deck
DSP that already implements key-lock / pitch-stretch toggling for live use
(EPIC-0001 / PRD-0025) — that path already crossfades or otherwise avoids a click
when the user toggles by hand, and an automated toggle uses the identical path, so
it inherits the same click-free behaviour for free. The applier therefore adds no
special boolean ramping; it routes the toggle through the same single source of
truth and lets the existing deck DSP handle the transition. The only applier-side
requirement is write-on-change (not write-every-tick) so the deck does not receive
a stream of identical "toggle to same value" events that could retrigger a
transition.
