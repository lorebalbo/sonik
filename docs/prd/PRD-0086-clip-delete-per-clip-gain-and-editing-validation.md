---
status: Not Implemented
epic: EPIC-0010
depends-on:
  - PRD-0081
  - PRD-0083
  - PRD-0084
  - PRD-0085
---

# 1. PRD-0086: Clip Delete, Per-Clip Gain & Editing Validation

## 1.1. Problem

EPIC-0010 is, by the time this PRD is reached, almost entirely built. The
arrangement snapshot compiler (PRD-0079) turns the `daw` `ValueTree` into a
lock-free audio-thread schedule; the clip streaming layer (PRD-0080) pre-rolls
source audio into ring buffers off the audio thread; the timeline render engine
(PRD-0081) sums active clips into the master output click-free; the DAW
transport (PRD-0082) plays, pauses, stops, and loops a review region; the edit
command layer (PRD-0083) wraps every mutation for undo/redo; and the move/trim
(PRD-0084) and uncrop/split (PRD-0085) interactions give the DJ the bulk of the
non-destructive editing surface. What remains are the two final clip operations
named in EPIC-0010 §1.2.1 — **Delete** and **Per-Clip Gain** — plus the
Epic-level **integration validation** that proves the whole stack works as one
coherent system.

The two missing operations are small in isolation but real gaps. **Delete**
(`DeleteClip`, EPIC-0010 §1.3.5) removes a clip node from the arrangement; it is
the only structural edit that reduces the clip count, and it must be undoable
so the DJ can recover from an accidental removal. **Per-clip gain** is already
modelled in the clip value object (`gainDb`, introduced in EPIC-0008 and carried
by PRD-0063) and is already *read* by the render engine (PRD-0081) when summing
clips — but there is no user affordance to change it and no command to make the
change undoable. Without `SetClipGain`, the `gainDb` field is dead weight: every
clip plays at unity and the DJ cannot pull one track down in the reconstructed
mix.

Beyond the two operations, EPIC-0010 has never been validated *as a whole*. Each
PRD shipped with its own unit tests, but no test or manual procedure exercises
the full chain — record a set (EPIC-0009), press DAW Play, hear the lossless
reconstruction, then move / trim / uncrop / split / delete / gain clips and
confirm every edit is heard click-free with working undo/redo. The render engine
reading the streamer's ring buffers, the snapshot recompiling on every command,
and the transport following all of it must be shown to interoperate. This PRD
owns that end-to-end proof: it is both the final feature PRD and the integration
gate for the Epic.

## 1.2. Objective

The system completes EPIC-0010's editing surface and validates the Epic
end-to-end such that:

- A `DeleteClip` command (PRD-0083 command layer) removes a single selected clip
  node from the `daw` `ValueTree`, recompiles and republishes the arrangement
  snapshot (PRD-0079), and is fully undoable / redoable. Deleting the last clip
  on a lane, or the last clip in the arrangement, leaves a valid empty
  arrangement that plays silence without error.
- A `SetClipGain` command adjusts a clip's `gainDb` field over a closed range,
  recompiles the snapshot, and is undoable / redoable. The render engine
  (PRD-0081) applies the per-clip gain when summing the clip into the master
  output, with smoothing so a gain change never zippers.
- A UI affordance exists to change per-clip gain (the canonical form is chosen
  in §1.5.1) and a UI affordance / keystroke exists to delete the selected
  clip; both route exclusively through the PRD-0083 command layer on the message
  thread.
- Delete relies on undo for recovery rather than a confirmation dialog
  (§1.5.3); no clip data is destroyed irrecoverably while the undo stack holds
  the operation.
- The full EPIC-0010 stack is validated end-to-end by an integration test suite
  and a manual test plan: a recorded set reconstructs sample-accurately on DAW
  Play, and every edit operation (move, trim, uncrop, split, delete, set-gain)
  is audible, click-free, and reversible via undo/redo.
- No new audio-thread allocation, lock, or I/O is introduced; delete and
  set-gain mutate the message-thread `ValueTree` and republish the snapshot
  exactly as every other PRD-0083 command does, and the render engine continues
  to read only the lock-free published schedule and the lock-free streamer ring
  buffers.

## 1.3. User Flow

1. The DJ has recorded a set (EPIC-0009, the **E9** workflow) onto the
   arrangement and presses **Play** on the DAW transport (PRD-0082). The
   timeline render engine (PRD-0081) streams each clip's source crop and sums
   the active lanes to the master output; the DJ hears a lossless reconstruction
   of the performed mix, the playhead following along.
2. The DJ clicks a clip to select it. The clip shows a **gain affordance** (per
   §1.5.1, a vertical drag handle / horizontal value line across the clip body).
   The DJ drags the handle down; a `SetClipGain` command fires on drag-end (or
   throttled during drag, §1.5.6), lowering the clip's `gainDb`. The arrangement
   snapshot recompiles, and on the next playback pass that clip is quieter in
   the mix. The change is smoothed in the renderer, so no click or zipper is
   heard even if the gain is changed during playback.
3. The DJ decides one clip should not be in the mix at all. With the clip
   selected, the DJ presses **Delete** (keystroke) or chooses **Delete** from
   the clip context menu. A `DeleteClip` command removes the clip node; the
   snapshot recompiles; on the next playback pass that clip's audio is gone. No
   confirmation dialog appears.
4. The DJ realises the deletion was a mistake and presses **Cmd/Ctrl-Z**. The
   `DeleteClip` command is undone: the clip node is reinstated with all its
   fields (`timelineStartSample`, `sourceStartSample`, `sourceEndSample`,
   `gainDb`, lane), the snapshot recompiles, and the clip plays again exactly as
   before. **Cmd/Ctrl-Shift-Z** redoes the deletion.
5. The DJ deletes every remaining clip one by one. After the last deletion the
   arrangement is empty; pressing **Play** produces silence and the playhead
   still advances normally, with no error and no crash. Undo restores the clips
   in reverse order.
6. Across all of the above, the DJ freely interleaves move (PRD-0084), trim
   (PRD-0084), uncrop and split (PRD-0085), delete, and set-gain edits while
   playback is running, and hears each change applied click-free with the
   transport never stalling.

## 1.4. Acceptance Criteria

- [ ] A `DeleteClip` command exists in the PRD-0083 command layer
      (`Source/Features/Daw/Editing/EditCommands.*`). Executing it removes the
      target clip node from the `daw` `ValueTree`, captures enough state to
      reconstruct the node on undo, and triggers a snapshot recompile +
      republication (PRD-0079).
- [ ] `DeleteClip` is undoable and redoable: undo reinstates the clip node with
      identical `lane`, `timelineStartSample`, `sourceStartSample`,
      `sourceEndSample`, `gainDb`, and source identity; redo removes it again.
- [ ] Deleting the last clip on a lane leaves that lane empty and valid;
      deleting the last clip in the arrangement leaves a valid empty
      arrangement. Pressing Play on an empty arrangement renders silence, the
      playhead advances, and no error is logged and no assertion fires.
- [ ] A `SetClipGain` command exists in the PRD-0083 command layer. Executing it
      writes the clip's `gainDb` field, captures the previous value for undo,
      and triggers a snapshot recompile + republication.
- [ ] `SetClipGain` is undoable and redoable: undo restores the previous
      `gainDb`; redo reapplies the new value. The range is the closed interval
      defined in §1.5.2 (`-inf dB` silence floor up to `+12 dB`); writes outside
      the range are clamped.
- [ ] The timeline render engine (PRD-0081) applies each clip's `gainDb` when
      summing the clip into the master output, and smooths gain changes (§1.5.6)
      so that changing a clip's gain during playback produces no click or
      zipper. A renderer-level test asserts the smoothed ramp on a gain change.
- [ ] A per-clip gain UI affordance exists (the canonical form per §1.5.1) on
      the clip component, routes its change exclusively through the
      `SetClipGain` command on the message thread, and never writes `gainDb`
      directly.
- [ ] A delete UI affordance exists: a keystroke (Delete / Backspace on the
      selected clip) and/or a clip context-menu item, both routing exclusively
      through the `DeleteClip` command on the message thread.
- [ ] Delete uses no confirmation dialog (§1.5.3); recovery is via undo. No clip
      audio data is destroyed — the source files and stem cache are never
      touched by a delete; only the `ValueTree` node is removed.
- [ ] Selection-then-delete operates on the editing model chosen in §1.5.4
      (single-clip selection for this Epic): deleting acts on exactly the
      selected clip.
- [ ] An integration test suite under `Tests/` (e.g.
      `DawEditingTests.cpp` for the command-level edits and
      `ArrangementPlaybackTests.cpp` for the render-through-transport path)
      exercises: build an arrangement, render a block range through PRD-0081 +
      PRD-0082, then apply each of move / trim / uncrop / split / delete /
      set-gain via PRD-0083 commands and assert the recompiled snapshot and the
      rendered output reflect the edit. Undo of each command restores the prior
      rendered output.
- [ ] The integration suite asserts **sample-accurate reconstruction**: for an
      unedited arrangement, the rendered master output equals the expected sum
      of the clips' source crops (within the resampler's documented tolerance,
      §1.5.5), confirming PRD-0079 + PRD-0080 + PRD-0081 + PRD-0082 interoperate.
- [ ] The integration suite asserts **click-free edits**: after a delete or a
      gain change during playback, the rendered output contains no discontinuity
      exceeding the anti-click ramp's bound (the EPIC-0010 §1.3.1 64-sample ramp
      criterion, §1.5.5).
- [ ] No new audio-thread code path added by this PRD allocates, locks, or
      performs I/O. `DeleteClip` and `SetClipGain` mutate the message-thread
      `ValueTree` and republish the snapshot via the existing SeqLock /
      double-buffer (PRD-0079); the audio thread continues to read only the
      published schedule and the lock-free streamer ring buffers (PRD-0080).
- [ ] All new UI affordances comply with `DESIGN.md`: monochrome palette, `2px
      solid #2d2d2d` borders, `Space Mono` labels, no `border-radius`, no
      gradients; the gain handle and any numeric readout follow the existing
      clip component language.

## 1.5. Grey Areas

### 1.5.1. Per-Clip Gain UI Affordance

Per-clip gain can be exposed as (a) a vertical drag handle / horizontal value
line drawn across the clip body (Ableton-style "clip gain line"), (b) a numeric
dB field in a clip inspector / context value, or (c) a context-menu submenu of
preset gains. Each has trade-offs: the drag line is the most direct and
DAW-idiomatic but is the most UI work; the numeric field is precise but slow;
the context menu is the least discoverable.

**Resolution:** A drag handle / horizontal gain line across the clip body
(option a) is the canonical affordance, with a numeric dB readout shown while
dragging. This matches the mental model every DAW user has of "clip gain" as a
draggable line, keeps the interaction inside the clip the DJ is already looking
at (no separate panel), and reuses the same drag-handle machinery PRD-0084 built
for trim. A future inspector panel may add a precise numeric entry, but it is
not required by this PRD. The handle and readout must follow `DESIGN.md`
(monochrome, `Space Mono`, hard 2px borders, no rounded corners).

### 1.5.2. Gain Range and Units

The `gainDb` field is in decibels, but the usable range is a choice. Options:
symmetric (e.g. `-12 .. +12 dB`), asymmetric with a true silence floor
(`-inf .. +12 dB`), or unity-capped (`-inf .. 0 dB`, attenuation only).

**Resolution:** `gainDb` is in **decibels**, with a usable range of `-inf dB`
(true silence, represented as a sentinel / the linear-zero floor) up to
`+12 dB`. The silence floor lets the DJ fully mute a clip without deleting it
(useful for A/B comparisons during editing), and `+12 dB` of headroom allows
boosting a quietly recorded source — bounded so a fat-fingered drag cannot
produce extreme gains that clip the master. The drag line maps its full vertical
travel to this range with `0 dB` (unity) at a clearly marked detent, mirroring
the filter-knob detent pattern (PRD-0056). Writes outside the range are clamped
by the `SetClipGain` command, not by the renderer.

### 1.5.3. Delete Confirmation

Deleting a clip could (a) prompt a confirmation dialog, (b) delete immediately
and rely on undo, or (c) move the clip to a "trash" lane first.

**Resolution:** Delete immediately and rely on undo (option b). EPIC-0010
§1.2.1 mandates undo/redo for all editing operations, so the safety net already
exists; a confirmation dialog on every delete would be a constant friction in an
editing workflow where deletes are routine, and DAWs universally make delete
immediate-and-undoable. Crucially, delete only removes a `ValueTree` node — it
never touches the source file or stem cache — so the operation is cheap to undo
and loses no audio data. A confirmation dialog is reserved for genuinely
destructive, non-undoable actions, which a clip delete is not.

### 1.5.4. Multi-Select vs Single-Clip Delete

Delete (and by extension gain) could operate on a single selected clip or on a
multi-selection of clips. Multi-select is a natural DAW expectation but adds
selection-model, marquee, and batched-command complexity.

**Resolution:** **Single-clip selection** for this Epic. The selection model
established by PRD-0084 (click to select one clip) is the basis; delete and
set-gain act on exactly that one selected clip. Multi-select (marquee selection,
shift-click extension, batched delete / batched gain as a single undo step) is a
real and desirable feature but is deferred to a future editing-polish Epic,
where it can be built once across all edit operations rather than special-cased
into delete. The command layer (PRD-0083) does not preclude a future
`DeleteClips` / batched composite command; this PRD simply does not require it.

### 1.5.5. Validation Scope and Acceptance Threshold

"Editing validation" needs a concrete pass/fail bar. The two properties worth
proving are **reconstruction fidelity** (does Play reproduce the recorded mix?)
and **edit cleanliness** (are edits click-free?), but exact sample equality is
impossible once resampling (PRD-0080 §1.3.6) is in the path.

**Resolution:** Two thresholds. (1) **Reconstruction:** for sources already at
the project sample rate, the rendered output must be **sample-accurate** (bit-
exact within floating-point summation tolerance) against the expected clip sum;
for resampled sources, equality is asserted within the resampler's documented
error bound (a small per-sample epsilon and/or a spectral / RMS-difference
threshold), not bit-exact. (2) **Edit cleanliness:** after any edit applied
during playback, the maximum inter-sample step at the affected clip boundary
must not exceed the bound implied by the EPIC-0010 §1.3.1 64-sample anti-click
ramp — i.e. the test asserts the ramp is present and no raw discontinuity leaks
through. These thresholds are codified as constants in the integration tests so
the bar is explicit and regression-checked.

### 1.5.6. Renderer Gain Smoothing to Avoid Zipper

Applying `gainDb` as a raw per-block multiplier means a gain change between
blocks produces an abrupt step (a "zipper" click), especially if the UI throttles
`SetClipGain` commands during a drag. The renderer must smooth.

**Resolution:** The render engine (PRD-0081) applies clip gain through a
**per-clip smoothed gain** (a `juce::SmoothedValue` or equivalent linear/exp
ramp over a short window, e.g. the same 64-sample order as the anti-click ramp),
so any change in the published target gain is reached over a ramp rather than
instantly. To bound the number of undo steps, the gain-line drag throttles
`SetClipGain` commands (e.g. coalesces intermediate drag values, committing a
single command on drag-end while previewing live), so a drag yields one undo
entry, not hundreds. The smoothing lives entirely in the audio thread reading
the published target — no allocation, no lock — and is independent of how many
commands the UI emits.

### 1.5.7. Empty Arrangement After Deleting All Clips

Deleting the last clip yields an arrangement with zero clips. The snapshot
compiler, render engine, and transport must all behave correctly with an empty
schedule.

**Resolution:** An empty arrangement is a fully valid state. The snapshot
compiler (PRD-0079) publishes an empty (zero-event) schedule; the render engine
(PRD-0081) sums nothing and outputs silence; the transport (PRD-0082) still
runs, the playhead still advances, and loop-region review still functions over
the empty timeline. No special-case "needs at least one clip" guard is added
anywhere — the zero-clip path is exercised by an integration test that deletes
every clip, presses Play, and asserts silent output with a normally advancing
playhead and no error. Undo restores the clips in reverse deletion order.
