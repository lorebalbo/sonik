---
status: Not Implemented
epic: EPIC-0011
depends-on:
  - PRD-0063
  - PRD-0079
---

# 1. PRD-0087: Automation Data Model

## 1.1. Problem

EPIC-0011 promises time-varying parameter motion — filter sweeps, EQ rides, gain
moves, tempo changes, and mode toggles — captured live, replayed during
playback, and freely editable afterwards. None of that can exist until there is
a place to *put* the data. Capture (PRD-0088 through PRD-0091), the playback
applier (PRD-0092), the lane UI (PRD-0093), and editing (PRD-0094) all read from
and write to a shared automation structure. Today that structure does not exist:
the `daw` `ValueTree` introduced by PRD-0063 holds the timeline and arrangement
nodes, but has no concept of an automation lane, a breakpoint, or a step event.

The model must serve two physically different parameter shapes from one coherent
schema. A filter sweep is a *continuous* gesture: a smooth value that the applier
must be able to evaluate at any sample position between two recorded points. A
key-lock toggle is a *boolean* gesture: a state that flips at an instant and is
held until the next flip. EPIC-0011 §1.3.1 enumerates exactly which parameters
fall into each shape (continuous: per-channel filter/high/mid/low/gain and
master tempo; boolean: per-channel key-lock/pitch-stretch/key-stepper), and the
data model is the first artefact that has to commit those shapes to a concrete
`ValueTree` layout.

Three cross-cutting constraints make the schema design non-trivial. First, lanes
are keyed by `(channelGroup | master, parameterId)` — a two-axis namespace that
must distinguish a per-channel parameter from a master-only parameter without
collision. Second, the set of automatable parameters is explicitly expected to
grow: EPIC-0011 §1.2.2 reserves crossfader and channel-fader automation as a
future enhancement and requires the model to "accommodate new parameter ids
without migration." A schema that hard-codes today's parameter list would break
that promise. Third, this PRD owns *only* the data model and its observer
contract; it must define a structure that the EPIC-0012 serializer can persist
later without redesign, while explicitly not implementing serialization itself.

## 1.2. Objective

The system provides an automation data model living inside the `daw` `ValueTree`
(PRD-0063) such that:

- Automation lanes are addressable by a two-axis key: an **owner** that is either
  a channel group (`A`, `B`, `C`, `D`) or `master`, and a **parameter id** (e.g.
  `filter`, `gain`, `tempo`, `keyLock`). The `(owner, parameterId)` pair uniquely
  identifies at most one lane.
- Each lane is one of two kinds. A **continuous lane** holds an ordered list of
  breakpoints, each carrying a `timelineSample` (the playhead position in the
  `daw` timeline's sample domain), a `value` in the underlying parameter's own
  units, and an `interpolation` mode (`linear` default, plus `step`/`hold`). A
  **boolean lane** holds an ordered list of step events, each carrying a
  `timelineSample` and a boolean `value` that is held until the next event.
- The in-scope parameter id set is fixed by this PRD but the schema is open:
  `master.tempo` (continuous, BPM per EPIC-0003); per-channel `filter`, `high`,
  `mid`, `low`, `gain` (continuous, ranges/units per EPIC-0007); per-channel
  `keyLock`, `pitchStretch`, `keyStepper` (boolean). New parameter ids can be
  added later by capture/applier PRDs **without any schema migration** — the
  model stores an opaque parameter id string and a kind discriminator, not a
  closed enum baked into the tree shape.
- Continuous values are stored in the **underlying parameter's raw units** (EQ /
  filter normalised range from EPIC-0007; tempo in BPM from EPIC-0003), not a
  generic normalised `0..1`, so that the applier writes back through the same
  single-source-of-truth path the live control uses with no unit translation.
- The **key-stepper lane is boolean**, recording engaged/stepped transitions
  only; its semitone state remains owned by PRD-0025 and is *not* duplicated in
  the automation model (see §1.5.7).
- Every lane maintains a **sorted-by-`timelineSample` invariant** for its
  breakpoints / step events. The model exposes insert / move / remove operations
  that preserve ordering, and the invariant is enforced (not merely assumed) so
  that the applier may binary-search and capture may append cheaply.
- The model exposes an **observer contract via JUCE `ValueTree::Listener`** so
  the lane UI (PRD-0093) and the editing command layer (PRD-0094) react to lane
  and breakpoint changes rather than polling.
- An **empty lane is distinct from an absent lane** (see §1.5.6): the model can
  represent a lane that exists but currently has zero breakpoints (e.g. armed but
  not yet captured) separately from a parameter that has no lane at all.
- **No serialization is implemented here.** The structure is shaped so that
  EPIC-0012 can persist it directly from the `ValueTree`, but file I/O, schema
  versioning, and project save/load are out of scope.
- **No audio-thread code** is added. The model lives on the message thread inside
  the `daw` `ValueTree`; the audio thread reads automation only via the
  EPIC-0010 published snapshot, never by walking lanes in `processBlock`.

## 1.3. Developer / Integration Flow

1. A new header group is added under `Source/Features/Daw/Automation/`:
   `AutomationModel.h` (the lane container + owner/parameter-id keying +
   `ValueTree` attachment), `ContinuousLane.h` (breakpoint list + interpolation),
   and `BooleanLane.h` (step-event list). These are message-thread types that
   wrap child nodes of the PRD-0063 `daw` `ValueTree`; they own no audio-thread
   state.

2. The `daw` `ValueTree` (PRD-0063) gains an `automation` container node. Under
   it, each lane is a child node of type `lane` with properties `owner`
   (`"A"|"B"|"C"|"D"|"master"`), `parameterId` (opaque string, e.g. `"filter"`,
   `"tempo"`, `"keyLock"`), and `kind` (`"continuous"|"boolean"`). The
   `(owner, parameterId)` pair is unique; `AutomationModel` enforces uniqueness on
   lane creation and refuses to create a duplicate.

3. A **continuous** lane's `lane` node carries ordered `breakpoint` child nodes,
   each with properties `timelineSample` (int64), `value` (double, raw parameter
   units), and `interpolation` (`"linear"|"step"`). `ContinuousLane` wraps the
   `lane` node and provides `addBreakpoint`, `moveBreakpoint`, `removeBreakpoint`,
   and `evaluateAt(timelineSample)` (linear or step/hold per the segment's
   leading breakpoint). Insertion finds the correct sorted position so the child
   order always reflects ascending `timelineSample`.

4. A **boolean** lane's `lane` node carries ordered `step` child nodes, each with
   properties `timelineSample` (int64) and `value` (bool). `BooleanLane` provides
   `addStep`, `moveStep`, `removeStep`, and `stateAt(timelineSample)` (the value
   of the most recent step at or before the query position; the lane's default
   before the first step is `false`). Ordering is maintained identically to the
   continuous lane.

5. `AutomationModel` exposes lane lookup and lifecycle: `getLane(owner,
   parameterId)` (returns the existing lane or none), `getOrCreateContinuousLane`
   / `getOrCreateBooleanLane`, `removeLane`, and enumeration of all lanes. The
   model validates that a requested `(owner, parameterId)` is consistent with the
   expected kind for in-scope ids (e.g. `filter` is continuous, `keyLock` is
   boolean) but does **not** reject unknown ids — an unknown id simply takes the
   caller-specified kind, which is what makes future parameters migration-free.

6. The ordering invariant is enforced inside the lane wrappers, not by callers:
   every mutating operation re-establishes ascending `timelineSample` order
   before returning. A debug-only assertion verifies the invariant after each
   mutation. Two breakpoints/steps at the *same* `timelineSample` are resolved per
   §1.5.5.

7. Observation is plain JUCE: `AutomationModel` is constructed over a reference to
   the `daw` `ValueTree` and clients attach a `ValueTree::Listener` to the
   `automation` node (or a specific `lane` node). Adding / removing a lane,
   adding / moving / removing a breakpoint, and changing an interpolation mode all
   surface as standard `valueTreeChildAdded` / `valueTreeChildRemoved` /
   `valueTreePropertyChanged` callbacks. No bespoke listener interface is
   introduced — the UI and command layers already speak `ValueTree::Listener`.

8. A new test file `Tests/AutomationModelTests.cpp` (registered with the existing
   `TestRunner`) covers: lane keying / uniqueness, continuous breakpoint ordering
   and `evaluateAt` for linear and step interpolation, boolean step ordering and
   `stateAt`, empty-vs-absent lane distinction, kind consistency for in-scope ids,
   acceptance of a novel parameter id without migration, and that the in-scope
   parameter id set (`master.tempo`; per-channel filter/high/mid/low/gain;
   per-channel keyLock/pitchStretch/keyStepper) round-trips through the model.

## 1.4. Acceptance Criteria

- [ ] A header group exists under `Source/Features/Daw/Automation/` containing
  `AutomationModel.h`, `ContinuousLane.h`, and `BooleanLane.h`. All three are
  message-thread types backed by the PRD-0063 `daw` `ValueTree`; none declare
  audio-thread state, allocate on the audio thread, or are reachable from
  `processBlock`.
- [ ] The `daw` `ValueTree` contains an `automation` container node. Each
  automation lane is a child `lane` node with properties `owner`
  (`"A"|"B"|"C"|"D"|"master"`), `parameterId` (string), and `kind`
  (`"continuous"|"boolean"`).
- [ ] `AutomationModel` keys lanes by the `(owner, parameterId)` pair and
  guarantees at most one lane per pair: a second `getOrCreate*` for the same pair
  returns the existing lane; an attempt to create a lane with a conflicting kind
  for an in-scope id is rejected (see §1.5.7).
- [ ] A continuous lane stores ordered `breakpoint` children, each with
  `timelineSample` (int64), `value` (double in the parameter's raw units), and
  `interpolation` (`"linear"` or `"step"`). `ContinuousLane::evaluateAt` returns
  the linearly interpolated value between bracketing breakpoints when the leading
  breakpoint's mode is `linear`, and the held leading value when it is `step`.
- [ ] A boolean lane stores ordered `step` children, each with `timelineSample`
  (int64) and `value` (bool). `BooleanLane::stateAt` returns the value of the most
  recent step at or before the query sample, and `false` before the first step.
- [ ] Continuous values are stored in the underlying parameter's raw units (EQ /
  filter range per EPIC-0007; tempo in BPM per EPIC-0003), not normalised
  `0..1`; a test asserts a tempo breakpoint stored as `128.0` reads back as
  `128.0` and an EQ breakpoint preserves its EPIC-0007 range value.
- [ ] The in-scope parameter id set is representable and round-trips through the
  model: `master.tempo` (continuous); per-channel `filter`, `high`, `mid`, `low`,
  `gain` (continuous); per-channel `keyLock`, `pitchStretch`, `keyStepper`
  (boolean). The key-stepper lane is boolean and stores only engaged/stepped
  transitions, never a semitone value.
- [ ] A novel parameter id not in the in-scope set (e.g. `"crossfader"`) can be
  added as a continuous or boolean lane through the same API with no schema change
  and no migration step; a test creates such a lane and reads it back.
- [ ] Every lane maintains ascending `timelineSample` order after any
  `add`/`move`/`remove`. Inserting an out-of-order breakpoint/step results in a
  correctly sorted child list; a debug assertion verifies the invariant after each
  mutation.
- [ ] An empty lane (exists, zero breakpoints/steps) is distinguishable from an
  absent lane (no `lane` node): `getLane` returns a present-but-empty lane in the
  former case and none in the latter; `evaluateAt`/`stateAt` on an empty lane
  return the documented defaults (no value / `false`) without error.
- [ ] Lane and breakpoint changes are observable through standard JUCE
  `ValueTree::Listener` callbacks attached to the `automation` node or a specific
  `lane` node; no bespoke listener interface is added. A test attaches a listener
  and asserts it fires on lane add, breakpoint add, breakpoint move, and
  interpolation-mode change.
- [ ] No serialization, file I/O, schema versioning, or project save/load is
  implemented by this PRD; the structure is shaped for EPIC-0012 to persist later,
  and a comment documents that boundary.
- [ ] `Tests/AutomationModelTests.cpp` is registered with the existing
  `TestRunner` and all assertions above pass under the standard `./build.sh`
  + test-runner flow.

## 1.5. Grey Areas

### 1.5.1. Breakpoint Storage: ValueTree Child Nodes vs Packed Buffer

Breakpoints could be stored as individual `ValueTree` child nodes (one node per
breakpoint) or packed into a single property (e.g. a binary blob or a delimited
string) on the `lane` node. Child nodes are heavier per breakpoint but integrate
natively with the JUCE observer model and the EPIC-0010 undo/redo command layer;
a packed buffer is compact but opaque to listeners and to the editing commands.

**Resolution:** Store breakpoints and step events as individual `ValueTree` child
nodes. The entire EPIC-0011 stack downstream — the lane UI (PRD-0093), the
editing command layer (PRD-0094), and the EPIC-0012 serializer — is built on the
assumption that automation participates in the standard `ValueTree` observer and
undo machinery. A packed blob would force every one of those consumers to
re-implement diffing and change notification by hand and would make a single
breakpoint move an opaque whole-property rewrite for undo. The per-breakpoint
node cost is acceptable: lanes are bounded by musical density (a few hundred
breakpoints over a typical set), this is message-thread data, and the audio
thread never touches the tree — it reads the EPIC-0010 snapshot. If profiling
ever shows the node count to be a problem, a packed *snapshot* representation can
be derived for the applier without changing the authoring tree.

### 1.5.2. Interpolation Modes: How Many at the Floor

The continuous lane needs at least linear (the default sweep shape) and a
step/hold mode (instant jump held until the next breakpoint). Richer curves —
exponential, S-curve, bézier handles — are tempting but expand the schema and the
editing UI surface dramatically.

**Resolution:** Ship exactly two interpolation modes in the data model: `linear`
(default) and `step` (hold the leading breakpoint's value until the next
breakpoint). These are the minimum EPIC-0011 §1.2.1 / §1.3.1 require and cover
the two gestures DJs actually need — a smooth ride and a hard jump. The
`interpolation` property is a string per breakpoint, so adding `exponential` or
`bezier` later is a value addition, not a structural migration: an unknown
interpolation string can fall back to `linear` in the applier. Storing the mode
*per breakpoint* (describing the segment that *starts* at it) rather than
per-lane is deliberate — it lets a single sweep mix a held intro with a smooth
body without splitting into multiple lanes.

### 1.5.3. Parameter-Id Namespacing: channelGroup vs master

Per-channel parameters (filter, gain) and master-only parameters (tempo) share a
parameter-id space but have different owners. The key could be a single flat
string (`"A.filter"`, `"master.tempo"`) or a structured pair
(`owner="A", parameterId="filter"`). A flat string is simpler to hash; a
structured pair keeps the two axes independently queryable.

**Resolution:** Use a structured `(owner, parameterId)` pair stored as two
separate `ValueTree` properties on the `lane` node, where `owner` is one of
`"A"|"B"|"C"|"D"|"master"` and `parameterId` is the bare parameter name
(`"filter"`, `"tempo"`, …). This keeps "all lanes for channel B" and "all lanes
for parameter `gain` across channels" both cheaply enumerable, matches the
EPIC-0011 §1.3.5 description of lanes "keyed by `(channelGroup | master,
parameterId)`", and avoids the parsing fragility of a flat composite string. The
master owner is a first-class value of the same `owner` axis rather than a
special case, so `master.tempo` and `A.filter` are addressed by exactly the same
mechanism. A convenience helper may render the pair as a display/debug string,
but the canonical key is always the structured pair.

### 1.5.4. Value Normalization: Raw Parameter Units vs Normalized 0..1

Continuous values could be stored normalised to `0..1` (uniform across all
parameters, convenient for a generic lane renderer) or in each parameter's raw
units (EQ range, BPM). Normalising decouples the model from each parameter's
definition; storing raw keeps the value identical to what the live control holds.

**Resolution:** Store raw parameter units. The single-source-of-truth invariant
(EPIC-0011 §1.3.2/§1.3.3) requires the applier to write the captured value back
through the *same* path the live control uses — mixer `ValueTree` params for
filter/EQ/gain (EPIC-0007 ranges), `MasterClockManager` for tempo in BPM
(EPIC-0003). Storing normalised values would force a normalise-on-capture /
denormalise-on-apply round trip that must agree perfectly with each parameter's
range definition forever; any drift between those two conversions silently
corrupts playback. Storing raw units means capture writes exactly what it read
and the applier writes exactly what it stored, with zero unit translation. The
lane renderer (PRD-0093) normalises *for display* using the parameter's known
range at draw time, which is a presentation concern, not a storage one. Tempo is
the clearest case: a `128.0` BPM breakpoint must read back as `128.0`, not as a
range-dependent fraction.

### 1.5.5. Ordering Invariant: Enforcement and Coincident Points

The applier wants breakpoints sorted by `timelineSample` so it can bracket the
playhead efficiently, and capture appends in time order naturally. But editing
(PRD-0094) can move a breakpoint past its neighbour, and two breakpoints can land
on the same `timelineSample`. The model must define both the invariant and the
coincident-point rule.

**Resolution:** The lane wrappers (`ContinuousLane` / `BooleanLane`) own and
enforce ascending `timelineSample` order: every `add`/`move` re-inserts at the
correct sorted position, and a debug assertion verifies the invariant after each
mutation. Callers never sort manually. For coincident `timelineSample` values the
model permits at most a stable, well-defined resolution: a newly inserted point
at an existing sample is placed *after* the existing one (stable insert), and the
applier's "most recent at or before" lookups therefore see the last-inserted
coincident point. Two coincident breakpoints are allowed (they express an instant
discontinuity — e.g. a hard cut implemented as two near-identical samples), but
the editing layer is free to collapse exact duplicates; that policy belongs to
PRD-0094, not here. The data model's only guarantees are: ascending order, stable
handling of ties, and a deterministic `evaluateAt`/`stateAt` at any query sample.

### 1.5.6. Empty Lane vs Absent Lane

When a parameter has been armed for automation but nothing has been captured yet,
is there a lane with zero breakpoints, or no lane at all? And what does the
applier do in each case?

**Resolution:** Empty and absent are distinct and both meaningful. An **absent**
lane (no `lane` node for the `(owner, parameterId)` pair) means the parameter is
not automated at all; the applier leaves the live value untouched. An **empty**
lane (a `lane` node exists but has zero breakpoints/steps) means automation is
*present but undefined* — e.g. the lane was created by arming or by the UI but no
gesture has been recorded yet. The model represents the empty lane as a real
node so the UI can show an empty lane track and capture can append into it
without a create-on-first-write race. `evaluateAt` on an empty continuous lane
returns "no value" (the applier then leaves the parameter alone, identical to
absent for playback purposes); `stateAt` on an empty boolean lane returns the
documented default `false`. The distinction matters for the UI and for capture
arming, not for the playback math — which is exactly why it must be representable
rather than collapsed.

### 1.5.7. Key-Stepper as Boolean vs Continuous Semitone

The key-stepper changes a deck's pitch in semitone steps, which *looks* like a
continuous (or integer-valued) parameter and could be modelled as a continuous
lane of semitone values. EPIC-0011 §1.3.1, however, specifies it as a **boolean**
lane that records engaged/stepped transitions, with the semitone state owned by
PRD-0025.

**Resolution:** Model the key-stepper as a **boolean** lane, per the Epic. The
automation lane records the *transitions* (engaged / stepped) as step events; it
does **not** store the resulting semitone value, which remains owned by PRD-0025
as the single source of truth for deck key state. Duplicating the semitone into
the automation model would create two authorities for the same value and risk
divergence on playback — exactly the anti-pattern the single-source-of-truth
invariant forbids. Consequently `AutomationModel` enforces kind-consistency for
in-scope ids: requesting a *continuous* lane for `keyStepper` (or any boolean id)
is rejected, as is requesting a *boolean* lane for `tempo` (or any continuous
id). Unknown / future ids carry no such constraint — the caller declares the kind
— which is what preserves migration-free extensibility (§1.2 / Grey Area for new
params). If a future PRD genuinely needs semitone-valued key automation, it
introduces a *new* parameter id with continuous kind rather than redefining the
boolean key-stepper lane, leaving this contract intact.
