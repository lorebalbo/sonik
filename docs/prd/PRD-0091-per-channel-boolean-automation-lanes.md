---
status: Not Implemented
epic: EPIC-0011
depends-on:
  - PRD-0011
  - PRD-0025
  - PRD-0087
  - PRD-0088
---

# 1. PRD-0091: Per-Channel Boolean Lanes — Key-Lock / Pitch-Stretch / Key-Stepper

## 1.1. Problem

EPIC-0011 captures the DJ's *performance gestures* and reproduces them on
playback. The continuous side of that capture (filter, EQ, gain) is covered by
PRD-0090; the storage primitives and capture-tap infrastructure are owned by the
foundational PRDs in this Epic. What remains uncaptured is the set of per-deck
*mode toggles* that materially change how a deck sounds: **key-lock** (decouple
speed from pitch, PRD-0011), **pitch-stretch** (engage the time-stretch path),
and **key-stepper** (semitone transposition, PRD-0025). When a DJ flips key-lock
on while opening a filter, or steps the key up during a build, those moves are
just as much part of the performance as the filter sweep — but today nothing
records them. On playback the mix would reproduce the filter motion while
silently dropping the key-lock toggle and the key change, so the rendered result
would not match what the DJ actually performed.

The deck booleans are authoritative `ValueTree` parameters that the live UI and
MIDI already write to (`keyLockEnabled` from PRD-0011, `keyShift` and the
stretch-engage state from PRD-0025). EPIC-0011 §1.3.1 specifies that these are
captured as **boolean step lanes**: ordered on/off step events whose value is
held until the next event. Crucially, the key-stepper is modelled as boolean
*by design* — this lane records when the stepper is *engaged / steps occur* as
transitions; the actual signed semitone value remains owned by PRD-0025 and is
never duplicated into the automation model. Without this PRD there is no lane,
no capture tap, and no recorded step stream for any of the three booleans, and
the automation playback applier (PRD-0092) would have nothing to replay.

This PRD is **capture only**. It defines the three per-channel boolean lanes and
records toggle transitions into them while the DAW is recording. Playback
write-back is PRD-0092; the lane UI is PRD-0093; editing is PRD-0094.

## 1.2. Objective

The system provides, per channel/deck, three boolean automation lanes
(`key-lock`, `pitch-stretch`, `key-stepper`) and captures live toggles into them
such that:

- Each channel group exposes exactly three boolean lanes keyed by
  `(channelGroup, parameterId)` for `parameterId ∈ { keyLock, pitchStretch,
  keyStepper }`, stored using the boolean-lane structure defined by PRD-0087
  (ordered step events: `timelineSample`, `state` on/off).
- While the DAW is armed and recording (PRD-0071 / PRD-0088), a change to the
  corresponding authoritative deck `ValueTree` parameter appends an ordered step
  event to the matching boolean lane via the EPIC-0009 event bridge on the
  message thread — never via a side channel and never on the audio thread.
- The `keyLock` lane records transitions of PRD-0011's `keyLockEnabled` deck
  property (`false → true` ⇒ on step; `true → false` ⇒ off step).
- The `pitchStretch` lane records transitions of the deck's time-stretch engage
  state as clarified in §1.5.5 (the boolean that gates whether the Rubber Band
  stretch path is active for the deck, distinct from `keyLock`).
- The `keyStepper` lane records the **engaged/stepped transitions** of PRD-0025's
  key-stepper as boolean step events: an off→on transition when the stepper
  leaves its neutral state (`keyShift` moves away from `0`) and an on→off
  transition when it returns to neutral (`keyShift` returns to `0`). The signed
  semitone value itself is owned by PRD-0025 and is not stored in this lane
  (§1.5.1, §1.5.7).
- Each lane's value is a held step: the recorded `state` persists until the next
  step event. Capturing a step does not interpolate.
- The initial state of each lane at record start is captured as an explicit step
  event at `timelineSample = recordStart` reflecting the parameter's value at the
  moment recording begins (§1.5.4), so the lane is self-describing without
  reference to pre-record history.
- Rapid successive toggles are debounced/coalesced per §1.5.6 so the lane does
  not accumulate degenerate zero-duration step pairs.
- No audio-thread code is added: capture observes `ValueTree` changes on the
  message thread and appends through the EPIC-0009 bridge; the audio thread is
  untouched by this PRD.

## 1.3. User Flow

1. The DJ has Deck A loaded and playing in the DAW timeline. Recording is armed
   (PRD-0088). Key-lock is currently off, the stretch path is inactive, and the
   key-stepper is neutral (`keyShift = 0`).
2. The DJ presses Record (PRD-0071). At `timelineSample = recordStart` the
   capture layer writes an initial step into each of Deck A's three boolean
   lanes: `keyLock = off`, `pitchStretch = off`, `keyStepper = off` — the values
   sampled at record start.
3. Two bars in, the DJ clicks the Key Lock button on Deck A. PRD-0011's
   `keyLockEnabled` flips to `true`. The capture tap observes the change and
   appends an `on` step to Deck A's `keyLock` lane at the current
   `timelineSample`.
4. The DJ then steps the key up: pressing `>` on the Key Stepper moves
   `keyShift` from `0` to `+1` (PRD-0025). Because the stepper has left neutral,
   the capture tap appends an `on` step to Deck A's `keyStepper` lane at the
   current `timelineSample`. The DJ presses `>` twice more (`+2`, `+3`); the
   stepper is already engaged, so no further `keyStepper` step is appended (the
   semitone value is PRD-0025's concern, not this lane's — §1.5.7).
5. The DJ engages the pitch-stretch path on Deck A. The deck's stretch-engage
   boolean flips to `true`; the capture tap appends an `on` step to the
   `pitchStretch` lane.
6. Later the DJ steps the key back down to `0`. As `keyShift` returns to neutral,
   the capture tap appends an `off` step to the `keyStepper` lane.
7. The DJ clicks Key Lock off. The `keyLock` lane receives an `off` step at the
   current `timelineSample`.
8. The DJ accidentally double-clicks the stretch button (on then off within a few
   milliseconds). Per the debounce rule (§1.5.6) the two transitions coalesce and
   no zero-duration on/off pair is written to the `pitchStretch` lane.
9. The DJ stops recording. Each boolean lane now holds an ordered list of step
   events from `recordStart` onward, each value held until its successor. Deck B
   and Deck C, untouched during the take, each hold only their single initial
   step. The captured lanes are now available to PRD-0092 for playback and to
   PRD-0093 for rendering.

## 1.4. Acceptance Criteria

- [ ] For every channel group, the `daw` `ValueTree` contains exactly three
      boolean automation lanes keyed by `(channelGroup, parameterId)` with
      `parameterId ∈ { keyLock, pitchStretch, keyStepper }`, each using the
      boolean-lane step-event structure defined by PRD-0087.
- [ ] Each boolean lane stores ordered step events of the form
      `(timelineSample, state)` where `state` is a boolean and events are sorted
      strictly ascending by `timelineSample`.
- [ ] A boolean lane's evaluated value at any sample is the `state` of the most
      recent step event at or before that sample (held / step semantics, no
      interpolation).
- [ ] While the DAW is not recording (disarmed or stopped), no step events are
      appended to any boolean lane regardless of deck-parameter changes.
- [ ] On record start, the capture layer appends to each of the recording deck's
      three boolean lanes a single initial step event at
      `timelineSample = recordStart` whose `state` equals the current value of
      the corresponding deck parameter at that instant.
- [ ] While recording, a transition of PRD-0011's `keyLockEnabled` deck property
      appends a step event to that deck's `keyLock` lane with `state` equal to the
      new value, at the current `timelineSample`.
- [ ] While recording, a transition of the deck's pitch-stretch engage boolean
      (as defined in §1.5.5) appends a step event to that deck's `pitchStretch`
      lane with `state` equal to the new value, at the current `timelineSample`.
- [ ] While recording, the `keyStepper` lane receives an `on` step when
      PRD-0025's `keyShift` transitions away from `0` (neutral → engaged) and an
      `off` step when `keyShift` returns to `0` (engaged → neutral), at the
      current `timelineSample`.
- [ ] Changes to `keyShift` that do not cross the neutral boundary (e.g. `+1 → +2`,
      `+3 → +1`, `−2 → −4`) append no step event to the `keyStepper` lane; the
      semitone magnitude is owned by PRD-0025 and is never written into this lane.
- [ ] Capture appends are performed on the message thread via the EPIC-0009 event
      bridge; no automation write occurs on the audio thread.
- [ ] No allocation, lock, or I/O is added to any audio-thread path by this PRD;
      `processBlock` and everything it calls are unchanged.
- [ ] Rapid successive toggles that produce a degenerate zero-duration step pair
      (an `on` and an `off`, or vice versa, within the debounce window defined in
      §1.5.6) are coalesced so the lane does not retain the redundant pair.
- [ ] The capture taps observe only the authoritative deck `ValueTree`
      parameters (`keyLockEnabled`, the stretch-engage boolean, `keyShift`); they
      do not duplicate, mirror, or re-derive deck state into a parallel store.
- [ ] Each deck/channel captures into its own lanes independently; a toggle on
      Deck A never appends to Deck B's or Deck C's lanes.
- [ ] No playback write-back, no lane UI, and no editing behaviour is added by
      this PRD (those are PRD-0092, PRD-0093, PRD-0094 respectively); only
      capture and storage of step events are in scope.
- [ ] New capture code resides under
      `Source/Features/Daw/Automation/` (boolean capture tap alongside the
      PRD-0090 continuous taps), consistent with EPIC-0011 §1.3.5.
- [ ] At least one test under `Tests/` drives a recording session with synthetic
      toggle sequences (key-lock on/off, stretch on/off, key-stepper across and
      not-across neutral) and asserts the exact ordered step events recorded in
      each boolean lane, including the initial record-start step and the absence
      of debounced zero-duration pairs.

## 1.5. Grey Areas

### 1.5.1. Key-Stepper as Boolean vs Multi-State Semitone

The key-stepper carries a signed semitone value (`keyShift`, −12..+12 per
PRD-0025), so a literal automation lane could store the full integer trajectory.
But EPIC-0011 §1.3.1 explicitly models the key-stepper as a **boolean** lane that
records engaged/stepped *transitions*.

**Resolution:** This lane is boolean. It records only the neutral⇄engaged
transitions (`keyShift` leaving / returning to `0`). The semitone value is owned
by PRD-0025 and is the single source of truth for *how many* semitones; this lane
records *that* the stepper is engaged, not *by how much*. Duplicating the
semitone trajectory here would create two authorities for the same value and risk
divergence — exactly the side-channel anti-pattern EPIC-0011 §1.3.2 forbids. If a
future enhancement wants a continuous semitone automation lane, it can add a
separate continuous lane (the model accommodates new `parameterId`s without
migration) without changing this boolean lane's contract.

### 1.5.2. Deck → Channel Mapping

Automation lanes are keyed by `channelGroup`, but the booleans live on deck
state. A channel group and a deck are not always one-to-one in every conceivable
routing.

**Resolution:** For this Epic, each channel group maps to exactly one deck (the
deck whose audio is routed into that channel strip), matching the existing
EPIC-0007 channel-to-deck association. The boolean lanes are keyed by the
`channelGroup` for UI/locality consistency with the continuous lanes (PRD-0090),
but the capture tap reads the deck `ValueTree` parameters of the deck bound to
that channel. The mapping is resolved once at capture-tap construction from the
established channel↔deck routing; no new routing concept is introduced. If a
future Epic allows many-to-one deck↔channel routing, the keying can be
revisited, but that is out of scope here.

### 1.5.3. Step vs Pulse Semantics (Held State vs Momentary)

A boolean lane could store *held* state (each step latches a value until the next
step) or *momentary pulses* (each event is an instantaneous trigger).

**Resolution:** Held step semantics, per EPIC-0011 §1.3.1 ("value is held until
the next event"). Every step event carries an explicit `state` and the lane's
value between events is the last `state`. This matches how the underlying deck
booleans actually behave (key-lock stays on until toggled off) and lets the
playback applier (PRD-0092) reconstruct the parameter by reading the most recent
step. Momentary-pulse semantics would lose the "what is the current state"
information that playback needs and is rejected.

### 1.5.4. Initial State at Record Start

When recording begins, a lane with no events would have an undefined value until
the first toggle, so playback could not know the parameter's starting state.

**Resolution:** Capture writes an explicit initial step at
`timelineSample = recordStart` for each boolean lane, sampling the deck
parameter's value at that instant. The lane is thus self-describing: PRD-0092 can
evaluate it at any point in the take without consulting pre-record history. This
also makes re-records and partial-range edits (PRD-0094) well-defined. The
alternative — implying "off until first toggle" — would silently corrupt takes
that start with key-lock already on.

### 1.5.5. What "Pitch-Stretch" Boolean Means Precisely

EPIC-0011 lists `key-lock`, `pitch-stretch`, and `key-stepper` as three distinct
booleans, but PRD-0011 frames key-lock as the toggle that *engages* the Rubber
Band time-stretch path, which makes "pitch-stretch" appear to overlap with
key-lock. PRD-0025 adds the stepper's `pitchScale`, which is yet another use of
the same stretcher. The three names must be disambiguated before capture can read
the right parameter.

**Resolution:** The three booleans map to three *distinct* authoritative deck
parameters:

- `keyLock` ⇒ PRD-0011's `keyLockEnabled` — decouples speed from pitch (vinyl vs
  pitch-locked playback).
- `pitchStretch` ⇒ the deck's *stretch-engage* boolean: whether the time-stretch
  (Rubber Band) processing path is active at all for the deck, independent of
  whether key-lock is the reason. In the PRD-0011/0025 design the stretcher is
  active whenever key-lock is on **or** `keyShift ≠ 0`; `pitchStretch` captures
  that effective "stretcher engaged" condition as its own observable boolean.
- `keyStepper` ⇒ the neutral⇄engaged condition of PRD-0025's `keyShift` (§1.5.1).

If the codebase does not yet expose a single `stretchEngaged` boolean, this PRD's
capture tap derives it on the message thread from the existing parameters
(`keyLockEnabled || keyShift != 0`) and observes *that* derived boolean's
transitions; it does not add audio-thread state. The precise source property is
finalised against the PRD-0011/PRD-0025 implementation at build time, but the
*meaning* (stretcher path active) is fixed here.

### 1.5.6. Debouncing Rapid Toggles

A double-click, a bouncing MIDI button, or a fast key-stepper sweep across
neutral can produce two opposite transitions within a few milliseconds, which
would litter the lane with zero-duration step pairs that are inaudible on
playback but bloat the data and complicate editing.

**Resolution:** Coalesce opposite transitions that fall within a short debounce
window (default `≈ 5 ms`, configurable as a capture constant). If an `on` step is
followed by an `off` step (or vice versa) within the window, the pair is dropped
and the lane returns to its pre-pair state. Same-direction repeats are already
no-ops (the state is unchanged). The window is short enough to preserve any
intentional fast toggle a human could perform musically (tens of ms apart) while
absorbing mechanical/electrical bounce. This mirrors PRD-0025 §1.5.3's stance
that rapid stepper input needs no extra smoothing beyond coalescing.

### 1.5.7. Key-Stepper Steps as Separate Booleans vs One Engaged Flag

The key-stepper has two arrows (semitone up / down). Capture could model each
press as its own boolean event stream (an "up" lane and a "down" lane) or use a
single engaged flag.

**Resolution:** A single engaged flag, exactly as §1.5.1 establishes. The lane
records only neutral⇄engaged transitions of `keyShift`; individual up/down
presses that stay on the same side of neutral are not separate booleans and are
not captured here. Modelling per-arrow booleans would re-encode the semitone
trajectory that PRD-0025 already owns, violating the single-source-of-truth
invariant (EPIC-0011 §1.3.2). The engaged flag is sufficient for PRD-0092 to know
*when* the stepper was active; PRD-0092 reads the semitone magnitude from
PRD-0025's parameter when it needs the exact transposition.
