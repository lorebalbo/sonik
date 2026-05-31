---
status: Not Implemented
epic: EPIC-0011
depends-on:
  - PRD-0083
  - PRD-0087
  - PRD-0092
  - PRD-0093
---

# 1. PRD-0094: Automation Editing

## 1.1. Problem

By the time this PRD is reached, the entire EPIC-0011 automation stack works in one direction: the data model exists (PRD-0087), live gestures are captured into lanes during recording (PRD-0088 through PRD-0091), the applier reproduces those gestures on playback through the single-source-of-truth paths (PRD-0092), and the lanes render the captured curves beneath each channel group and the master (PRD-0093). What the DJ still cannot do is **change** what was captured. A filter sweep recorded a beat too late, a gain ride that is too aggressive, a key-lock toggle that landed on the wrong bar — all of these are baked in. The DJ's only recourse today is to re-arm, re-record the whole pass, and hope the second take is cleaner. That is exactly the workflow EPIC-0011's vision promised to eliminate: "The DJ can then redraw a sweep, soften a gain ride, or move a key-lock toggle to a different beat."

The lanes from PRD-0093 are render-only surfaces: they draw breakpoints and step events but accept no editing gestures. There is no way to click an empty lane to add a breakpoint, drag an existing breakpoint to a new time or value, delete one, change a segment from a linear ramp to a stepped hold, or slide a boolean toggle to a different beat. Crucially, even if those gestures existed, they must not mutate the `daw` ValueTree directly: EPIC-0011 §1.3.5 mandates that **all edits flow through EPIC-0010's command layer** so that every automation edit is undoable and redoable alongside every clip edit, on one shared history stack. A breakpoint drag that bypasses the command layer would be a non-undoable mutation and a single-source-of-truth violation.

This is also the **capstone PRD of EPIC-0011**. With editing in place, the full round trip closes: capture a live gesture, watch playback reproduce it, edit the resulting curve, and confirm playback reflects the edit — all click-free and all undoable. This PRD owns the editing interactions and their command wiring; it does not own the model, the capture, the applier, or the lane rendering it builds upon.

## 1.2. Objective

The system makes the automation lanes from PRD-0093 directly editable, with every edit routed through the EPIC-0010 command layer (PRD-0083) for undo/redo, such that:

- On a **continuous** lane (filter, high, mid, low, gain, master tempo) the DJ can: add a breakpoint by clicking an empty region of the lane; move a breakpoint by dragging it (snappable to the master grid per PRD-0065); delete a breakpoint; redraw or drag a curve segment; and change the interpolation of a segment, supporting at minimum **linear** and **step/hold**.
- On a **boolean** lane (key-lock, pitch-stretch, key-stepper) the DJ can: add a toggle/step point, move a toggle point in time (grid-snappable), and delete a toggle point.
- Every editing gesture is expressed as one EPIC-0010 edit command — `AddBreakpoint`, `MoveBreakpoint`, `DeleteBreakpoint`, `SetInterpolation`, and `MoveBooleanStep` — pushed onto the shared undo/redo stack (PRD-0083). Undo restores the exact prior lane state; redo reapplies the edit.
- All time-domain edits are **grid-snappable** against the master grid (PRD-0065), with snap toggleable consistently with the rest of the timeline's snap behaviour.
- Editing a lane mutates only the **automation data model** (PRD-0087); the applier (PRD-0092) reads the mutated model on its next playback evaluation, so an edit made while playing back is reflected from the next evaluation onward without restarting playback and without audible clicks.
- New breakpoint and toggle values are **clamped to the underlying parameter's range** (EQ/filter/gain ranges from EPIC-0007, BPM from EPIC-0003) so no edit can write an out-of-range value into the model.
- The EPIC-0011 **acceptance loop** is validated end to end: record a filter sweep plus a master-tempo nudge, confirm playback reproduces both gestures, edit the captured curves (move/add/delete breakpoints, change a segment to step/hold, move a boolean toggle), and confirm the next playback reflects the edits click-free.
- No automation editing code runs on the audio thread; all editing happens on the message thread against the ValueTree model, and the audio thread continues to read only the published snapshot (PRD-0092 / EPIC-0010).

## 1.3. User Flow

1. The DJ has a recorded arrangement with automation lanes visible beneath a channel group and the master (PRD-0093). A filter lane shows a captured sweep; the master-tempo lane shows a small nudge; a key-lock boolean lane shows one toggle.
2. The DJ clicks an empty region of the filter lane between two existing breakpoints. An `AddBreakpoint` command is constructed (lane id, timeline sample at the click x snapped to the master grid, value at the click y clamped to the filter range) and dispatched through the EPIC-0010 command layer (PRD-0083). The model gains a breakpoint; the lane redraws with the new point; the command lands on the undo stack.
3. The DJ drags an existing breakpoint to a new time and value. While dragging, the lane previews the moved point; on mouse-up a single `MoveBreakpoint` command (lane id, breakpoint id, new snapped sample, new clamped value) is dispatched. Dragging with snap enabled locks the time to grid lines (PRD-0065); the value is clamped to the parameter range.
4. The DJ right-clicks (or uses the per-segment affordance — see §1.5.3) on the segment between two breakpoints and chooses `Step / Hold`. A `SetInterpolation` command changes that segment's interpolation from `linear` to `step`; the lane redraws the segment as a held step.
5. The DJ deletes a stray breakpoint (selects it and presses delete, or right-click → delete). A `DeleteBreakpoint` command removes it; the adjacent segment re-forms; the command is undoable.
6. On the key-lock boolean lane, the DJ drags the toggle point to a later beat. A `MoveBooleanStep` command moves the step event, grid-snapped; the lane redraws the step at the new position.
7. The DJ presses undo (the shared EPIC-0010 shortcut). The most recent command is reverted — the toggle returns to its prior beat; the lane redraws. Redo reapplies it. Automation edits interleave on the same history stack as clip edits from EPIC-0010.
8. The DJ presses play. The applier (PRD-0092) evaluates the now-edited lanes at the playhead and writes the edited values through the single-source-of-truth paths (mixer ValueTree, `MasterClockManager` for tempo). The mix reproduces the edited gestures, not the originals, with no clicks at the edited breakpoints.
9. The DJ edits a breakpoint **while playback is running**. The model mutates immediately; from the applier's next evaluation tick onward the new value is used; playback does not stop and no click is heard (§1.5.7).

## 1.4. Acceptance Criteria

- [ ] Clicking an empty region of a continuous lane adds a breakpoint at the clicked time (snapped to the master grid when snap is on, per PRD-0065) and value (clamped to the parameter range), via an `AddBreakpoint` command dispatched through the EPIC-0010 command layer (PRD-0083).
- [ ] Dragging an existing breakpoint moves it in time and value via a single `MoveBreakpoint` command emitted on drag end (not one command per mouse-move); time is grid-snapped when snap is on; value is clamped to the parameter range.
- [ ] Deleting a breakpoint (keyboard delete on a selected point and/or context-menu delete) emits a `DeleteBreakpoint` command; the adjacent segment re-forms across the gap.
- [ ] Changing a segment's interpolation emits a `SetInterpolation` command supporting at minimum `linear` and `step` (hold); the lane redraws the segment accordingly, and the applier (PRD-0092) honours the new interpolation on the next evaluation.
- [ ] Redrawing / dragging a curve segment is expressed through the breakpoint commands above (moving the segment's bounding breakpoints, or adding intermediate breakpoints), not a separate non-undoable path; the resulting model change is fully undoable.
- [ ] On a boolean lane, adding a toggle point emits `AddBreakpoint` (or the boolean-step equivalent), moving a toggle point emits `MoveBooleanStep` (grid-snapped), and deleting a toggle emits `DeleteBreakpoint`; the lane redraws steps correctly after each.
- [ ] Every automation edit command lands on the **shared** EPIC-0010 undo/redo stack (PRD-0083); undo restores the exact prior lane state and redo reapplies the edit; automation and clip edits interleave correctly on one history.
- [ ] All time-domain edits snap to the master grid (PRD-0065) when snap is enabled and move freely when snap is disabled, consistent with the rest of the timeline's snap toggle.
- [ ] All value edits are clamped to the underlying parameter's range (EQ / filter / gain from EPIC-0007; BPM from EPIC-0003); no command can write an out-of-range value into the model (PRD-0087).
- [ ] Editing a lane mutates only the automation model (PRD-0087); the applier (PRD-0092) reflects the edit on its **next** evaluation. An edit made during live playback takes effect from the next evaluation tick without restarting playback.
- [ ] Editing during playback produces no audible click at the edited breakpoint: the applier's existing smoothing (EPIC-0007 parameter smoothing / snapshot delivery) absorbs the value change.
- [ ] The EPIC-0011 capstone loop passes: record a filter sweep and a master-tempo nudge → playback reproduces both → edit the curves (add/move/delete breakpoints, set one segment to step/hold, move a boolean toggle) → next playback reflects the edits, click-free, with the tempo edit driving `MasterClockManager` (never a forked tempo).
- [ ] No automation editing code executes on the audio thread; all edits occur on the message thread against the ValueTree. The audio thread continues to read only the published snapshot and performs no allocation, no locks, and no I/O as a result of editing.
- [ ] `Tests/` coverage includes automation editing command tests (e.g. `AutomationEditingTests.cpp`): each command (`AddBreakpoint`, `MoveBreakpoint`, `DeleteBreakpoint`, `SetInterpolation`, `MoveBooleanStep`) applies, undoes, and redoes correctly against the model, with grid-snap and value-clamp assertions. The capstone round trip is exercised by combining `AutomationCaptureTests.cpp` and `AutomationPlaybackTests.cpp` with the new editing tests (capture → playback → edit → playback).

## 1.5. Grey Areas

### 1.5.1. Breakpoint Add Gesture: Single-Click vs Double-Click

Adding a breakpoint on an empty lane region could be a single-click or a double-click. Single-click is fast and discoverable but risks accidental breakpoints when the DJ meant to select the lane, scrub, or click through to something beneath. Double-click is the safer, more deliberate gesture and is the convention in most DAWs (Ableton, Logic) for adding automation points.

**Resolution:** Double-click adds a breakpoint on a continuous lane; single-click selects the nearest breakpoint (or clears selection if the click is on empty space). This matches established DAW muscle memory, prevents accidental point creation during ordinary lane interaction, and keeps single-click free for selection and future scrub affordances. On boolean lanes, where the only continuous-style ambiguity is absent, double-click likewise adds a toggle for consistency. A single-click-to-add preference can be a future per-user setting without changing the command contract.

### 1.5.2. Snap Granularity and Per-Edit Toggle

Grid snap (PRD-0065) defines where breakpoints land in time, but the master grid can be coarse (bars/beats) while a DJ may want finer placement (e.g. a sweep that resolves on a 1/16). The question is the snap granularity for automation edits and whether snap can be momentarily defeated during a single drag.

**Resolution:** Automation edits snap to the same active grid resolution as the rest of the timeline (PRD-0065), respecting the global snap toggle, so the DJ sets snap once and it applies everywhere. Holding the platform's snap-override modifier (the same modifier EPIC-0010 already uses for clip edits) during a drag temporarily disables snap for that gesture, allowing fine placement without changing the global setting. This reuses one consistent snap model across clips and automation rather than inventing an automation-only granularity.

### 1.5.3. Interpolation UI: Per-Segment Menu vs Per-Breakpoint Handle

Changing interpolation could be exposed as a context-menu action on the segment between two breakpoints, or as an inline handle on each breakpoint that cycles its outgoing interpolation, or as a curve handle dragged to bend the segment.

**Resolution:** A per-segment context-menu choice (`Linear`, `Step / Hold`) is the v1 surface, with interpolation stored per outgoing segment on the left breakpoint (consistent with PRD-0087's per-segment interpolation field). This is the smallest, most explicit UI that satisfies the EPIC-0011 requirement of "at minimum linear and step/hold," keeps the `SetInterpolation` command trivial (target segment + new mode), and avoids the complexity and DESIGN.md monochrome-rendering challenge of draggable curve-tension handles. Curved/exponential interpolation and draggable tension handles are an explicit future enhancement; the command and model are designed so a new interpolation mode can be added without breaking this PRD's contract.

### 1.5.4. Multi-Select of Breakpoints

The DJ may want to move or delete several breakpoints at once (e.g. shift an entire sweep later by a beat). This PRD's command set is single-breakpoint-oriented; multi-select introduces marquee selection, group-move, and group-delete semantics.

**Resolution:** v1 ships single-breakpoint edits only; multi-select is deferred. The command layer is designed to make group operations a later additive change: a group move is a batch of `MoveBreakpoint` commands wrapped in a single EPIC-0010 compound/transaction command so the group undoes as one unit. Shipping single edits first keeps the editing surface and tests focused on the capstone loop; marquee multi-select and group transactions can land in a follow-up PRD without altering the per-breakpoint command contract defined here.

### 1.5.5. Boolean Toggle Drag Direction

A boolean step event has only a time and a state. Dragging it horizontally moves it in time (the intended `MoveBooleanStep`), but a DJ might drag vertically expecting to flip the state, which is meaningless for a step whose state is determined by alternation with its neighbours.

**Resolution:** Boolean toggle drags are horizontal-only: `MoveBooleanStep` changes the step's time, snapped to the grid; vertical drag is ignored. Flipping a boolean lane's state at a point is achieved by adding or deleting a step (which alternates the held state across the lane), not by dragging a step vertically. This keeps the boolean model unambiguous (state is derived from ordered alternation, per PRD-0087) and avoids a confusing vertical affordance on a two-state lane.

### 1.5.6. Value Clamping vs Free Drag Beyond Range

When dragging a breakpoint's value past the top or bottom of the lane, the edit could clamp the value to the parameter's range (the cursor goes past the edge but the value stops) or reject the drag at the boundary (the breakpoint cannot follow the cursor past the edge).

**Resolution:** Clamp the value to the parameter's valid range (EPIC-0007 ranges for mixer params, EPIC-0003 BPM range for tempo) while letting the cursor move freely; the breakpoint pins to the lane edge and the committed `MoveBreakpoint`/`AddBreakpoint` value is the clamped value. This guarantees the model (PRD-0087) never holds an out-of-range value — a hard invariant the applier (PRD-0092) relies on — while keeping the drag feel forgiving rather than fighting the cursor at the boundary.

### 1.5.7. Editing During Playback: Live Reflect on Next Evaluation

If the DJ edits a lane while playback is running, the edit must take effect, but the timing and click-freeness need a defined contract. Re-evaluating the applier synchronously on every keystroke risks clicks; deferring to the next regular evaluation tick is simpler but introduces a small latency between the edit and its audible effect.

**Resolution:** Edits mutate the model immediately and are picked up by the applier on its **next** scheduled evaluation tick (PRD-0092), not synchronously at edit time. The applier's existing smoothing (EPIC-0007 parameter smoothing for continuous params; the SeqLock-published snapshot for sample-accurate values) absorbs the new value so no click occurs at the edited breakpoint. The sub-tick latency is imperceptible at the applier's cadence and is the correct trade for click-free, audio-thread-safe editing. Playback never restarts on edit; the playhead continues and simply reads the updated model from the next tick.

### 1.5.8. Validation Acceptance: Sweeps Reproduced and Edits Applied Click-Free

The EPIC-0011 capstone acceptance is qualitative ("the mix reproduces the gestures, then reflects the edits"). It needs a concrete, testable bar so the Epic can be declared done.

**Resolution:** Acceptance is met when an automated test sequence — captured filter sweep + master-tempo nudge → playback → programmatic edits (add, move, delete a breakpoint; set one segment to step/hold; move a boolean toggle) → second playback — produces a model after editing that matches the expected edited breakpoint set, and the applier's emitted values on the second playback match the edited curve within the smoothing tolerance, with the tempo edit observed on `MasterClockManager` (not a forked tempo). Click-freeness is asserted via the same smoothing/snapshot tolerance used by PRD-0092's playback tests. This combines `AutomationCaptureTests.cpp`, `AutomationPlaybackTests.cpp`, and the new `AutomationEditingTests.cpp` into the EPIC-0011 capstone regression. A manual test plan (below) provides the human-in-the-loop confirmation that the round trip sounds correct.
