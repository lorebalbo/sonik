---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0063
  - PRD-0079
---

# 1. PRD-0083: Edit Command Layer & Undo/Redo

## 1.1. Problem

EPIC-0010 makes the recorded arrangement editable: the DJ moves clips, trims and
uncrops edges, splits, deletes, and adjusts per-clip gain. The non-destructive
clip model (PRD-0063) defines every clip as a pure value object — a crop window
(`sourceStartSample`, `sourceEndSample`) into a source of known length
(`sourceLengthSamples`) placed on the timeline at `timelineStartSample` with a
`gain` trim — and the arrangement snapshot compiler (PRD-0079) republishes a
fresh audio-thread schedule every time that model changes. What does not yet
exist is the **mutation discipline** that sits between the editing UI and the
`daw` `ValueTree`.

Without a single, enforced mutation path, each edit interaction (PRD-0084's drag
handles, PRD-0085's uncrop/split, PRD-0086's delete/gain) would be free to poke
the `ValueTree` directly. That has three fatal consequences. First, **undo/redo
becomes impossible to implement consistently** — every feature would have to
hand-roll its own inverse, and a drag that emits hundreds of intermediate
`timelineStartSample` writes would flood the undo history with un-coalesced
micro-steps. Second, **clamping and validation get duplicated** — every edit
site would independently re-derive the `[0, sourceLengthSamples]` bounds, and
they would inevitably drift apart, letting one code path write a crop window the
audio thread cannot honour. Third, **the recompile trigger gets forgotten** —
an edit that mutates the tree but neglects to ask PRD-0079 to recompile leaves
the audio thread playing a stale schedule, producing edits that are visible but
inaudible (or vice versa).

The same discipline is needed beyond this Epic. Recording (EPIC-0009) appends
captured clips, and future automation editing (EPIC-0011) will mutate lane and
clip parameters. If every one of those subsystems invents its own mutation
mechanism, the arrangement has no coherent undo history and no single point at
which validation and recompilation are guaranteed. This PRD builds the
**command layer and undo stack** that all clip mutation flows through — the
reversible spine of the entire editing Epic.

## 1.2. Objective

The system provides a command-pattern editing layer over the `daw` model such
that:

- Every mutation of the `daw` `ValueTree` that represents a clip edit is
  expressed as an **edit command** — `MoveClip`, `TrimClip`, `UncropClip`,
  `SplitClip`, `DeleteClip`, `SetClipGain` — and no editing feature mutates the
  tree outside this layer.
- Each command runs exclusively on the **message thread**, performs its mutation
  against `juce::UndoManager` (the model is already a `ValueTree`, so its
  property/child writes are natively undoable), and on completion triggers a
  schedule recompile via PRD-0079.
- The command interface exposes a uniform `perform` / `undo` / `redo` contract
  (delegated to `juce::UndoManager` transactions), so undo and redo restore the
  model to the exact pre- and post-edit states, byte-for-byte at the
  `ValueTree`-property level.
- A **continuous drag is one undoable transaction**: a gesture's begin /
  continue / end stream of intermediate writes is coalesced so that a single
  undo reverts the whole drag, not each intermediate sample step.
- All clamping and validation live **inside the command** — every command
  clamps its target crop window and timeline position into
  `[0, sourceLengthSamples]` (and rejects degenerate zero-length results) before
  committing, so no downstream consumer ever sees an out-of-bounds clip.
- After each committed command (and each undo/redo), the layer requests a
  **single debounced recompile** from PRD-0079, coalescing bursts of commits
  into one republished schedule rather than recompiling per intermediate write.
- Issuing a **new edit after one or more undos discards the redo stack**, so the
  history is always a single linear timeline with no orphaned redo branches.
- The layer is exercised by a unit-test suite that drives commands directly
  (no UI) and asserts the resulting `ValueTree` state, the undo/redo round-trip,
  drag coalescing, clamping, and redo invalidation.

This PRD owns the command/undo **infrastructure**. The specific edit behaviours
that plug into it — move/trim drag semantics (PRD-0084), uncrop/split
(PRD-0085), delete/gain and editing validation (PRD-0086) — are defined in their
own PRDs and are out of scope here.

## 1.3. Developer / Integration Flow

1. A new `Source/Features/Daw/Editing/` pair, `EditCommands.h/.cpp`, defines an
   `EditCommand` base contract (`perform()`, `undo()`, `redo()`, plus a
   human-readable `getDescription()` for the history) and the six concrete
   commands `MoveClip`, `TrimClip`, `UncropClip`, `SplitClip`, `DeleteClip`,
   `SetClipGain`. Each concrete command is constructed with the target clip's
   `ValueTree` (or its stable clip id) and the parameters of the edit, and holds
   a reference to the shared `juce::UndoManager`.
2. A new `UndoStack.h/.cpp` wraps a single `juce::UndoManager` instance owned by
   the `daw` editing session. It exposes `beginTransaction(description)`,
   `perform(EditCommand)`, `undo()`, `redo()`, `canUndo()`, `canRedo()`, and a
   change broadcaster so the UI (a future PRD) can enable/disable its undo/redo
   affordances. The `UndoManager` is constructed with the `daw` `ValueTree`'s
   `UndoManager` slot so that all property/child writes made through that tree
   are recorded automatically.
3. Each command's `perform()` opens an `UndoManager` transaction (named by
   `getDescription()`), mutates the `daw` model **only** through `ValueTree`
   setters bound to that `UndoManager`, clamps every written sample value into
   `[0, sourceLengthSamples]` (and clamps `timelineStartSample >= 0`), and
   returns. `undo()` / `redo()` delegate to the `UndoManager`'s own
   `undo()` / `redo()`.
4. For continuous gestures, the interaction layer (PRD-0084) calls
   `UndoStack::beginNewTransaction(description)` once at gesture start, issues
   many intermediate `MoveClip` / `TrimClip` / `UncropClip` mutations during the
   drag (each of which writes into the **same** open transaction without opening
   a new one), and the transaction closes at gesture end — yielding exactly one
   undoable step for the whole drag (see §1.5.2).
5. On every committed transaction (and on every `undo()` / `redo()`), the
   `UndoStack` notifies a `RecompileScheduler` collaborator that requests a
   recompile from PRD-0079. The scheduler debounces requests on the message
   thread (a short timer, see §1.5.6) so a burst of intermediate drag writes
   produces one republished schedule, while the final gesture-end state is
   always recompiled.
6. Issuing a brand-new transaction after an undo automatically discards the
   `UndoManager`'s redo branch (native `juce::UndoManager` behaviour); the
   `UndoStack` re-broadcasts its `canUndo` / `canRedo` state so the UI reflects
   the now-empty redo stack (see §1.5.7).
7. A new test file, `Tests/EditCommandLayerTests.cpp`, constructs an in-memory
   `daw` `ValueTree` with one or more clips (per the PRD-0063 schema), performs
   each command, and asserts: the post-edit `ValueTree` properties, that a
   single `undo()` restores the exact prior state and a single `redo()` reapplies
   it, that a simulated multi-step drag collapses to one undoable transaction,
   that out-of-bounds parameters are clamped into `[0, sourceLengthSamples]`,
   and that a new edit after an undo clears `canRedo()`.

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Daw/Editing/EditCommands.h/.cpp` and
  `Source/Features/Daw/Editing/UndoStack.h/.cpp` exist and compile, matching the
  file layout in EPIC-0010 §1.3.7.
- [ ] An `EditCommand` interface defines a uniform `perform()`, `undo()`,
  `redo()`, and `getDescription()` contract, and the six concrete commands
  `MoveClip`, `TrimClip`, `UncropClip`, `SplitClip`, `DeleteClip`, and
  `SetClipGain` each implement it.
- [ ] Every clip mutation in the editing Epic is routed through an `EditCommand`;
  no editing code mutates the `daw` `ValueTree` directly outside this layer
  (enforced by code review and by the fact that the `daw` tree is constructed
  with the shared `UndoManager`).
- [ ] All commands execute on the message thread only; no command performs work
  on, or is invoked from, the audio thread. No command allocates on, locks
  against, or blocks the audio thread.
- [ ] `UndoStack` wraps a single `juce::UndoManager` bound to the `daw`
  `ValueTree`, and exposes `perform`, `undo`, `redo`, `canUndo`, `canRedo`, and a
  change broadcaster.
- [ ] A single `undo()` after any one command restores the `daw` model to its
  exact pre-command `ValueTree` state (all properties and child clips identical);
  a single `redo()` reapplies it identically.
- [ ] A simulated continuous drag (one `beginNewTransaction` followed by N ≥ 2
  intermediate `MoveClip`/`TrimClip`/`UncropClip` writes) produces exactly one
  undoable transaction: a single `undo()` reverts the whole drag to its
  pre-gesture state, and `canUndo()` returns to its pre-gesture value afterwards.
- [ ] Every command clamps written sample values into `[0, sourceLengthSamples]`
  and clamps `timelineStartSample >= 0` before committing; a command invoked with
  out-of-bounds parameters commits a clamped (never an out-of-bounds) result, and
  a command that would produce a zero-length crop is rejected (no-op, no
  transaction committed).
- [ ] Each committed command, and each `undo()` / `redo()`, triggers a recompile
  request to PRD-0079; a burst of intermediate drag writes is debounced into a
  single republished schedule, while the gesture-end state is always recompiled.
- [ ] Performing a new command after one or more `undo()` calls discards the redo
  stack: `canRedo()` returns `false` immediately after the new command, and the
  change broadcaster fires so the (future) UI updates.
- [ ] `SplitClip` replaces one clip with two contiguous clips sharing the source
  and partitioned at the cut sample; a single `undo()` restores the original
  single clip exactly (verifying the command's reversibility for child-structure
  changes, not just property changes).
- [ ] `DeleteClip` removes the clip node; a single `undo()` re-inserts the clip
  with all its properties and at its original child index.
- [ ] `Tests/EditCommandLayerTests.cpp` exercises each command, the undo/redo
  round-trip, drag coalescing, clamping (including zero-length rejection), and
  redo invalidation, and is registered in the test runner.
- [ ] No audio-thread code is added or modified by this PRD; the command layer is
  message-thread-only and reaches the audio thread exclusively through PRD-0079's
  existing SeqLock-published schedule.

## 1.5. Grey Areas

### 1.5.1. juce::UndoManager + ValueTree vs Custom Command Stack

The command layer could be built on `juce::UndoManager` (JUCE's built-in undo
manager, which records every `ValueTree` property and child mutation made
through a tree bound to it) or on a hand-rolled command stack where each command
captures its own inverse.

**Resolution:** Use `juce::UndoManager`. The `daw` model **is** a `ValueTree`
(PRD-0063), and that is precisely the case JUCE's `UndoManager` is designed for:
binding the tree to an `UndoManager` makes every `setProperty` / `addChild` /
`removeChild` automatically and correctly reversible, including structural edits
like split and delete, with no hand-written inverse logic to get wrong. A custom
stack would re-implement, less safely, exactly what JUCE already provides, and
would risk the inverse of a structural edit (e.g. re-inserting a deleted clip at
the wrong child index) diverging from the forward edit. The `EditCommand`
classes still exist — they give each edit a named, testable, coalesce-able unit
and a description for the history — but they delegate the actual reversal to the
`UndoManager`. This is the smallest, most robust design and is consistent with
JUCE's `APVTS` precedent already used elsewhere in the codebase.

### 1.5.2. Drag Coalescing: Begin / Continue / End → One Transaction

A clip drag emits a continuous stream of intermediate positions. Each could open
its own `UndoManager` transaction (hundreds of undo steps per drag) or all could
fold into one transaction (a single undo reverts the gesture).

**Resolution:** One transaction per gesture. The interaction layer (PRD-0084)
calls `UndoStack::beginNewTransaction(description)` exactly once at gesture
start; every intermediate write during the drag lands in that still-open
transaction (JUCE coalesces successive writes into the current transaction until
the next `beginNewTransaction`); the transaction closes implicitly at gesture
end when the next `beginNewTransaction` is issued (or on an explicit flush). The
result is exactly one undoable step for the whole drag, which is what every DAW
user expects ("undo" puts the clip back where it started, not one pixel back).
The intermediate writes still drive live recompiles (debounced, §1.5.6) so the
drag is audible/visible as it happens; only the **undo granularity** is
coalesced, not the live feedback.

### 1.5.3. Undo Granularity: Per-Clip vs Per-Gesture

Undo could be scoped per affected clip (undo reverts the last clip touched) or
per user gesture (undo reverts the last logical action, which may touch one or
several clips — e.g. a split touches one clip and creates a sibling).

**Resolution:** Per-gesture. A single `UndoManager` transaction corresponds to
one user action regardless of how many clip nodes it mutates. A split (one node
out, two nodes in) is one undoable step; a move of one clip is one undoable step.
This matches user mental model ("undo my last action") and is the natural unit of
a `juce::UndoManager` transaction. Multi-clip operations (e.g. a future
"delete selection" spanning several clips) will likewise wrap all their
mutations in one `beginNewTransaction` so the whole selection edit is a single
undo — but that multi-select edit is a future-PRD concern; this PRD's six
commands each touch a single logical clip operation.

### 1.5.4. Undo Across Recording: Can You Undo a Captured Clip?

Recording (EPIC-0009) appends captured clips to the same `daw` model. A question
is whether those recording-appended clips participate in this PRD's undo history
— i.e. whether the DJ can press undo and remove a clip the live capture wrote.

**Resolution:** Out of scope for this PRD, with a deliberate boundary. This PRD's
`UndoManager` records mutations made **through the editing command layer** on the
message thread. Whether EPIC-0009's capture commits its appends through this same
`UndoManager` (making a recorded clip undoable) or through a separate
non-undoable path (so undo only reverts post-recording *edits*) is an EPIC-0009
integration decision, not an EPIC-0010 one. The infrastructure this PRD builds
**supports** either choice: if recording routes its append through an
`EditCommand`/`UndoManager` transaction, the captured clip is undoable for free;
if it does not, the editing undo stack simply never contains those appends. This
PRD does not mandate recording's policy; it only guarantees that everything which
*does* go through the command layer is reversible. The default expectation
(documented for the recording Epic to honour or override) is that undo reverts
**edits**, and that clearing/discarding a recording is a separate, explicit
action rather than an undo step.

### 1.5.5. Clamping and Validation Live in the Command

Bounds clamping (crop window into `[0, sourceLengthSamples]`, timeline position
`>= 0`, zero-length rejection) could live in the UI interaction layer, in the
command, or in the recompile step.

**Resolution:** In the command. The command is the single chokepoint every
mutation passes through, so placing clamping there guarantees no edit — from any
present or future caller — can commit an out-of-bounds crop window, regardless of
whether the UI happened to clamp first. The UI may still clamp for live visual
feedback (so the drag handle visibly stops at the source boundary), but that is a
presentation nicety; the **authoritative** clamp is the command's, applied
immediately before the `ValueTree` write. A command asked to produce a
degenerate zero-length crop performs no mutation and opens no transaction (so it
does not pollute the undo history with a no-op). The detailed per-edit clamp
formulas (e.g. trim-start must not cross trim-end) are specified in PRD-0084 /
PRD-0085 / PRD-0086; this PRD mandates only that the clamp lives in the command
and that the universal `[0, sourceLengthSamples]` / non-zero-length invariants
hold.

### 1.5.6. Recompile Debounce After Each Command

A recompile (PRD-0079) could fire synchronously on every single `ValueTree`
write (including each intermediate drag step) or be debounced so a burst of
writes produces one republished schedule.

**Resolution:** Debounce on the message thread. A `RecompileScheduler`
collaborator coalesces recompile requests with a short message-thread timer
(target ≈ one UI frame, e.g. 16 ms) so an in-progress drag emitting many
intermediate writes triggers at most a few recompiles per second rather than one
per sample-position update — keeping the message thread responsive and avoiding
redundant schedule republication. The invariant is that the **final** state of
any gesture is always recompiled: when the gesture ends (or the timer fires with
no further writes pending), a recompile reflecting the latest model state is
guaranteed. A discrete (non-drag) command — delete, split, gain set — also goes
through the debounce, but since it is a single write its recompile fires on the
next timer tick, which is imperceptible. PRD-0079 owns the recompile itself; this
PRD owns only the request/debounce trigger.

### 1.5.7. Redo Invalidation on New Edit

After one or more undos, issuing a new edit could either discard the redo stack
(linear history) or attempt to preserve it as a branch (tree history).

**Resolution:** Discard the redo stack — linear history. This is the native
`juce::UndoManager` behaviour and the universal DAW convention: undo three steps,
make a new edit, and the three redo steps are gone. Branching undo trees are a
niche power-user feature that adds substantial UI and model complexity for little
benefit in a DJ-arrangement editor, and would surprise users who expect the
standard linear behaviour. When the new edit's `beginNewTransaction` fires after
an undo, the `UndoManager` drops the redo branch automatically; the `UndoStack`
re-broadcasts `canUndo()` / `canRedo()` so the (future) UI immediately disables
its redo affordance. A future Epic may revisit branching history if real demand
emerges, but it is explicitly not a requirement here.
