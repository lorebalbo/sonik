---
status: Implemented
epic: EPIC-0011
depends-on:
  - PRD-0055
  - PRD-0056
  - PRD-0087
  - PRD-0088
---

# 1. PRD-0090: Per-Channel Continuous Lanes — Filter / High / Mid / Low / Gain

## 1.1. Problem

EPIC-0011 promises that the gestures a DJ performs on the mixer during a
recording — the slow filter opens, the EQ rides, the gain trims — are captured
as automation and reproduced verbatim on playback. The foundational pieces for
this are landing in sibling PRDs: PRD-0087 defines the automation data model
(continuous lanes keyed by `(channelGroup, parameterId)`, each holding ordered
breakpoints of `timelineSample`, `value`, `interpolation`), and PRD-0088
defines the capture taps and their connection to the EPIC-0009 event bridge that
appends breakpoints while recording is armed. What does not yet exist is the
concrete wiring of the five per-channel continuous mixer parameters into that
machinery.

The five parameters are already authoritative in the mixer state from EPIC-0007:
`mixer.channel.{A,B,C,D}.filter` (bipolar `[-1, +1]`, centre detent, PRD-0056),
`mixer.channel.{A,B,C,D}.eq.{high,mid,low}` (dB, `-inf … +6 dB`, full CCW = true
cut, PRD-0055), and `mixer.channel.{A,B,C,D}.gain` (the trim, dB,
`-inf … +12 dB`, default `0 dB`, PRD-0054). Until each of these is registered as
a continuous automation lane and tapped for capture, a DJ's filter sweeps and
EQ moves are heard live but vanish the moment the timeline replays — exactly the
divergence between live control and recorded performance that this Epic exists
to eliminate.

A naive capture also creates two failure modes. First, a fast filter sweep moved
by hand or by a MIDI knob (EPIC-0006) can emit hundreds of `ValueTree` change
callbacks per second; recording every one produces a bloated, jagged lane that
is expensive to store, render (PRD-0093), and edit (PRD-0094). Second, the
bipolar filter's centre detent snaps writes inside `±kDetentEpsilon` to exactly
`0.0` (PRD-0056 §1.5.6); the lane must record the post-snap authoritative value,
not the raw pre-snap controller value, or the recorded curve will not reproduce
the detent behaviour the DJ actually heard.

## 1.2. Objective

The system registers and captures the five per-channel continuous mixer
parameters as automation lanes such that:

- For each of the four channels (A, B, C, D), five continuous lanes exist —
  `filter`, `eq.high`, `eq.mid`, `eq.low`, and `gain` — keyed in the PRD-0087
  data model by the channel group and the parameter id, twenty lanes in total.
- While recording is armed (PRD-0088 / EPIC-0009), any change to one of the
  twenty source parameters appends a breakpoint `(timelineSample, value,
  interpolation)` to the matching continuous lane via the EPIC-0009 event
  bridge, on the message thread, exactly as PRD-0088 prescribes.
- Each captured breakpoint's `value` is the post-write authoritative `ValueTree`
  value in the parameter's own units and range: filter as bipolar `[-1, +1]`
  with the detent snap already applied; each EQ band as dB in `[-inf, +6]`; gain
  as dB in `[-inf, +12]`. No normalisation or unit conversion is performed at
  capture time (see §1.5.5).
- Capture is decimated to suppress redundant breakpoints during fast sweeps: a
  new breakpoint is appended only when the value has moved beyond a per-parameter
  threshold since the last captured breakpoint, or when a minimum time has
  elapsed, bounding lane density without losing the gesture's shape (§1.5.1).
- At the moment recording starts (the arm transition), the current value of each
  of the twenty source parameters is captured as the lane's initial breakpoint at
  the record-start `timelineSample`, so the reproduced automation begins from the
  state the mix was actually in (§1.5.6).
- The default interpolation written on each captured breakpoint is `linear`,
  consistent with EPIC-0011 §1.3.1; per-segment interpolation editing is owned by
  PRD-0094.
- No audio-thread code is added. Capture observes the authoritative `ValueTree`
  parameters on the message thread; the audio engine is untouched.

## 1.3. User Flow

1. The DJ has tracks loaded on decks A and B (mapped to channel groups A and B —
   see §1.5.4) and arms recording in the DAW (PRD-0088 / EPIC-0009).
2. At the arm transition, the system reads the current value of all twenty
   per-channel continuous parameters and writes an initial breakpoint into each
   corresponding lane at the record-start `timelineSample`. Channels with no
   loaded track still capture their (default) parameter values; the lanes simply
   stay flat.
3. The DJ slowly opens the filter on channel A by sweeping
   `mixer.channel.A.filter` from `0.0` toward `+1.0`. The message-thread capture
   tap observes each `ValueTree` change, applies the decimation rule (§1.5.1),
   and appends breakpoints — the post-detent-snap bipolar values — to channel A's
   `filter` lane.
4. Simultaneously the DJ rides `mixer.channel.B.eq.low` down to cut the bass on
   channel B during the transition; those dB values are captured into channel B's
   `eq.low` lane.
5. The DJ nudges `mixer.channel.A.gain` up by a couple of dB to compensate; the
   gain (dB) values are captured into channel A's `gain` lane.
6. The DJ stops recording. The twenty lanes now hold the captured curves. The
   curves are visible once the lane UI ships (PRD-0093) and reproduced once the
   playback applier ships (PRD-0092); this PRD's responsibility ends at correct,
   decimated, correctly-valued capture into the PRD-0087 model.
7. Re-arming and recording again over the same region behaves per PRD-0088's
   capture-session semantics (overwrite / merge policy is owned there); this PRD
   does not define record-over behaviour beyond appending through the same
   bridge.

## 1.4. Acceptance Criteria

- [ ] For every channel group in `{A, B, C, D}`, the PRD-0087 automation model
      contains five continuous lanes keyed by parameter id `filter`, `eq.high`,
      `eq.mid`, `eq.low`, and `gain` — twenty continuous lanes in total — each
      created (or lazily creatable) without requiring a recording to have run.
- [ ] Each lane's source parameter is bound to its authoritative `ValueTree`
      path: `mixer.channel.{A,B,C,D}.filter`,
      `mixer.channel.{A,B,C,D}.eq.{high,mid,low}`, and
      `mixer.channel.{A,B,C,D}.gain`. No parallel or shadow parameter is
      introduced.
- [ ] Each EQ band is captured into its own separate lane (high, mid, low are
      three distinct lanes per channel), not a single combined EQ lane (§1.5.3).
- [ ] While recording is armed, a change to any of the twenty source parameters
      appends a breakpoint to the matching lane through the EPIC-0009 event
      bridge on the message thread; while not armed, no breakpoints are appended.
- [ ] Each captured breakpoint stores `(timelineSample, value, interpolation)`
      with `interpolation = linear` by default, where `timelineSample` is the
      master-grid-aligned sample position supplied by PRD-0088's capture context.
- [ ] Each breakpoint `value` is the post-write authoritative parameter value in
      the parameter's native units: filter in bipolar `[-1, +1]`, EQ bands in dB
      within `[-inf, +6]`, gain in dB within `[-inf, +12]`. No unit conversion or
      normalisation is applied at capture (§1.5.5).
- [ ] For the filter parameter, the captured value is the value after the
      PRD-0056 §1.5.6 centre-detent snap; a sweep that lands inside the detent is
      recorded as exactly `0.0`, never an un-snapped value with
      `0 < |value| < kDetentEpsilon` (§1.5.2).
- [ ] Capture is decimated: a new breakpoint is appended only when the parameter
      value has changed by more than the per-parameter decimation threshold since
      the last captured breakpoint, or when the minimum sampling interval has
      elapsed, whichever comes first (§1.5.1). A continuous full-range sweep
      performed over one second produces a bounded, configuration-defined maximum
      number of breakpoints rather than one per `ValueTree` callback.
- [ ] At the record-arm transition, one initial breakpoint per lane is captured
      at the record-start `timelineSample` holding each parameter's current value,
      so reproduced automation starts from the correct state (§1.5.6).
- [ ] The channel-id-to-channel-group mapping (`A→A, B→B, C→C, D→D`) is explicit
      and documented; the lane key's channel group matches the mixer channel
      letter one-to-one (§1.5.4).
- [ ] No audio-thread code is added or modified by this PRD; all capture occurs
      on the message thread via `ValueTree` observation and the EPIC-0009 bridge.
      No `processBlock` path performs allocation, locks, or I/O as a result of
      this PRD.
- [ ] Playback application of these lanes is explicitly out of scope and owned by
      PRD-0092; no write-back to the mixer parameters occurs in this PRD.
- [ ] At least one unit test under `Tests/` (e.g.
      `ContinuousLaneCaptureTests.cpp`) verifies: (a) twenty lanes exist with the
      correct keys; (b) an armed filter sweep on channel A appends decimated
      breakpoints with post-detent values into channel A's `filter` lane and
      nothing into other lanes; (c) the same change while not armed appends
      nothing; (d) an EQ ride is captured into the correct per-band lane in dB;
      (e) the initial breakpoint is captured at record-start for every lane.

## 1.5. Grey Areas

### 1.5.1. Breakpoint Decimation Threshold for Fast Sweeps

A hand- or MIDI-driven filter sweep can fire `ValueTree` change callbacks far
faster than a useful lane needs. Recording one breakpoint per callback bloats
storage and rendering and makes later editing (PRD-0094) painful.

**Resolution:** Decimate at capture using a hybrid value-and-time rule. A new
breakpoint is appended only when either (a) the parameter has moved beyond a
per-parameter value threshold since the last captured breakpoint, or (b) a
minimum time interval (target ≈ 20–30 ms, i.e. roughly 30–50 Hz of lane
resolution) has elapsed since the last breakpoint while the value is still
changing. The value threshold is expressed in the parameter's own units:
`0.01` for the bipolar filter, `0.25 dB` for each EQ band, `0.25 dB` for gain —
all comfortably finer than perceptual resolution. The final breakpoint at the
end of a sweep (the value at which the knob comes to rest) is always captured so
the lane terminates on the exact resting value rather than a decimated
approximation. The thresholds live in one configuration constant block so they
can be tuned without touching capture logic. Thinning is a capture-time concern
only; PRD-0094's editing may add its own simplification independently.

### 1.5.2. Bipolar Filter Detent Interaction With Captured Values

The filter has a centre detent: PRD-0056 §1.5.6 snaps any write with
`|filter| < kDetentEpsilon` to exactly `0.0` inside the state setter, so the
audio thread never sees an un-snapped near-centre value. The question is whether
the lane records the raw incoming value or the post-snap authoritative value.

**Resolution:** Capture the post-snap authoritative value — the value actually
present in the `ValueTree` after the setter runs. Because the capture tap
observes the `ValueTree` property *after* it is written, it naturally reads the
snapped value with no special-casing: a sweep that lands inside the detent is
recorded as `0.0`. This guarantees the reproduced automation (PRD-0092)
reproduces the exact detent behaviour the DJ heard, and it keeps the single
source of truth honest — the lane records what the mixer actually did, never a
pre-snap intermediate. No detent logic is duplicated in the capture layer.

### 1.5.3. Per-Band vs Combined EQ Lane

The three EQ bands could be modelled as one combined "EQ" lane carrying a
composite value, or as three independent lanes (high, mid, low).

**Resolution:** Three separate lanes per channel — one each for `eq.high`,
`eq.mid`, `eq.low`. Each band is an independent authoritative `ValueTree`
parameter (PRD-0055) with its own dB value, its own MIDI binding (PRD-0061), and
its own future edit curve. A combined lane would force an artificial composite
value type, complicate decimation (a move on one band would touch the others'
stored values), and break the clean `(channelGroup, parameterId)` keying of
PRD-0087. Separate lanes also match how a DJ thinks about the controls and how
PRD-0093 will render them (stacked under the channel group). This costs three
lanes instead of one per channel but is the only model consistent with the data
model and the underlying parameters.

### 1.5.4. Channel-Id Mapping A–D to Channel Groups

The mixer exposes four channels `A, B, C, D`; EPIC-0011 keys lanes by
`channelGroup`. The mapping between the two must be fixed and unambiguous.

**Resolution:** A one-to-one identity mapping: mixer channel `A` → channel group
`A`, `B` → `B`, `C` → `C`, `D` → `D`. The automation channel group *is* the
mixer channel letter; there is no indirection through deck identity, even though
decks are conventionally loaded onto matching channels. This keeps the lane key
stable regardless of which deck (or no deck) is loaded on a channel, and it
matches the mixer's own addressing used by EPIC-0007 and the DDM4000 profile
(PRD-0061). If a future Epic introduces deck-to-channel reassignment, the lane
key remains anchored to the channel, which is the correct stable home for a
mixer-parameter automation curve.

### 1.5.5. Value Units Stored: Raw Parameter vs Normalised

Breakpoints could store the parameter's native value (bipolar `[-1,+1]`, dB) or
a normalised `[0, 1]` representation common to all lanes.

**Resolution:** Store the raw native parameter value in the parameter's own
units and range, exactly as it appears in the `ValueTree`. This makes capture a
trivial copy (no conversion, no rounding error, no inversion of the dB or detent
mapping), makes playback (PRD-0092) a trivial write-back through the same
authoritative path, and keeps the lane human-readable for editing (PRD-0094) and
serialisation (EPIC-0012). The cost — that different lanes carry different
ranges and a renderer must know each parameter's range to scale the curve — is
borne once by PRD-0093, which already needs per-parameter range knowledge to
draw axis bounds. Normalising at capture would only move that knowledge to a
worse place and introduce a lossy round-trip.

### 1.5.6. Capturing the Initial Value at Record Start

Without an initial breakpoint, a lane that the DJ never touches during a take has
no data, and a lane the DJ touches mid-take begins abruptly at the first move —
losing the state the parameter held before that move.

**Resolution:** At the record-arm transition, capture one breakpoint per lane at
the record-start `timelineSample` holding the parameter's current value. This
anchors every lane to a known starting state so reproduced automation begins
correctly even if the parameter is first moved seconds into the take, and it
makes "flat, untouched" lanes explicit (a single breakpoint) rather than empty.
The initial capture is driven by the same arm signal PRD-0088 already raises; it
is a one-shot snapshot of all twenty parameters, not a per-parameter callback,
so it adds negligible cost at the arm boundary.

### 1.5.7. Gain Lane Range and Units (dB vs Linear)

The channel gain parameter is defined in dB (`-inf … +12 dB`, PRD-0054) but the
DSP applies it as a linear multiplier. The lane could store either.

**Resolution:** Store the gain lane in dB, matching the authoritative
`mixer.channel.{A,B,C,D}.gain` `ValueTree` value (which is dB, per PRD-0054). The
dB-to-linear conversion is a DSP-layer concern that already happens inside the
channel strip's message-thread handler; the automation lane sits *above* that
conversion and must mirror the user-facing, `ValueTree`-resident value so that
capture is a direct copy and playback is a direct write-back through the same
setter. Storing dB also makes the lane perceptually linear for editing (equal
vertical distances are equal loudness changes), which is what a DJ expects when
redrawing a gain ride. The `-inf` end is represented by the same sentinel the
mixer state already uses for full-CCW gain; the lane introduces no new encoding.
