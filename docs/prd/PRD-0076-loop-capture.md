---
status: Not Implemented
epic: EPIC-0009
depends-on:
  - PRD-0014
  - PRD-0072
  - PRD-0073
---

# 1. PRD-0076: Loop Capture

## 1.1. Problem

EPIC-0009's clip-placement engine (PRD-0073) opens, grows, and closes a single
`DawClip` from the live source position of a playing deck. That model captures
linear playback faithfully: a clip's `sourceEndSample` advances in lock-step
with the deck's source position. But a loop is not linear. While a loop is
engaged, the deck's source position does not advance monotonically — it sweeps
from `loopInSamples` to `loopOutSamples`, jumps back to `loopInSamples`, and
sweeps again, for as many passes as the DJ holds the loop. PRD-0014 expresses
this with three properties on the deck's `Loop` ValueTree node:
`loopInSamples` (`int64_t`), `loopOutSamples` (`int64_t`), and `loopActive`
(`bool`).

If the capture engine naively tracked the source position, an engaged loop
would corrupt the growing clip: the `sourceEndSample` would oscillate
backwards on every wrap, producing a single clip whose source range is
ambiguous and whose timeline duration does not reflect what the DJ actually
heard. The arrangement must instead represent that the looped section was
*heard multiple times in a row* — once per pass — laid out contiguously on the
timeline so that the section audibly repeats when the arrangement is later
played back (EPIC-0010).

Without dedicated loop capture, a four-bar loop held for sixteen bars would be
recorded as a four-bar clip (losing twelve bars of performance) or as a
malformed clip with a backwards-jumping source crop. Neither reconstructs the
set. This PRD defines how the loop boundary crossings surfaced by the event
bridge (PRD-0072) drive the placement engine (PRD-0073) to emit one finalized
clip per loop pass, each referencing the identical source crop
`[loopInSamples, loopOutSamples]`, placed back-to-back on the same lane(s).

## 1.2. Objective

The capture engine renders an engaged loop as a sequence of contiguous,
identical clips — one per completed pass — such that:

- While `loopActive` is `true` on a recording deck, each completed traversal of
  the loop (deck source position reaching `loopOutSamples` and wrapping back to
  `loopInSamples`) finalizes one `DawClip` whose source crop is exactly
  `[loopInSamples, loopOutSamples)` and whose `timelineStartSample` is the
  previous pass's `timelineEndSample` (back-to-back, no gap, no overlap).
- Every pass clip is written to the same lane(s) the deck is currently writing
  to (per the active source mode, EPIC-0002), matching the lane(s) of the clip
  that was open when the loop engaged.
- The first pass begins at the timeline position the loop was entered, and its
  source crop begins at `loopInSamples` regardless of where within the loop the
  DJ engaged it (see §1.5.4 for the partial-first-pass nuance).
- Exiting the loop (`loopActive` transitioning to `false`, or a loop-out / loop
  deactivate event from PRD-0014) closes the current pass at the live source
  position and resumes a single growing clip from that position, contiguous on
  the timeline — handing control back to PRD-0073's ordinary open/grow/close
  lifecycle.
- A mid-loop change to `loopInSamples` or `loopOutSamples` (loop halve, loop
  double, or a manual in/out adjustment per PRD-0014) takes effect on the
  *next* pass: the in-progress pass finishes against the bounds it started
  with, and subsequent pass clips reference the new crop.
- Loop capture introduces no audio-thread work beyond the POD events already
  enqueued by PRD-0072; all clip creation and finalization happens on the
  message thread inside the recording session controller.

## 1.3. User Flow

This flow is observed in the captured `daw` model and the record playhead; the
DJ's loop controls are unchanged from PRD-0014.

1. The DJ has armed recording and pressed Play on Deck A. PRD-0073 has an open,
   growing clip on Deck A's lane(s), its `sourceEndSample` advancing with the
   deck.
2. The DJ engages a 4-beat auto-loop. PRD-0014 sets `loopInSamples`,
   `loopOutSamples`, and `loopActive = true`. PRD-0072 surfaces a loop-enter
   event to the recorder.
3. The placement engine closes the currently growing clip at the deck's source
   position at the instant the loop engaged (this becomes the partial first
   segment, see §1.5.4) and begins loop-capture mode for Deck A's lane(s).
4. The deck plays from the engage point to `loopOutSamples`, then wraps to
   `loopInSamples`. PRD-0072 surfaces a loop-boundary-crossing event. The
   placement engine finalizes pass clip #1 with source crop
   `[loopInSamples, loopOutSamples)` and `timelineStartSample` equal to the
   end of the partial first segment.
5. The deck continues looping. Each subsequent wrap finalizes another identical
   pass clip, each placed immediately after the previous one on the timeline.
   The arrangement now shows N back-to-back identical clips on Deck A's lane(s).
6. The DJ presses loop double (4 → 8 beats). PRD-0014 updates `loopOutSamples`.
   The pass currently in flight completes against the old bounds; the next
   finalized pass clip references the new, longer crop. No gap appears on the
   timeline.
7. The DJ exits the loop mid-cycle. PRD-0072 surfaces a loop-exit event. The
   placement engine finalizes a final partial pass clip whose source crop is
   `[loopInSamples, currentSourcePosition)` (the deck did not reach
   `loopOutSamples` this pass) and resumes a single growing clip from
   `currentSourcePosition`, contiguous on the timeline.
8. The DJ stops the deck. PRD-0073's ordinary close logic finalizes the growing
   clip. The captured arrangement faithfully reconstructs: linear playback, then
   K identical loop repetitions, then linear playback again.

## 1.4. Acceptance Criteria

- [ ] While a recording deck has `loopActive = true`, the recording session
  controller is in loop-capture mode for that deck's lane(s), and the ordinary
  PRD-0073 grow path (advance `sourceEndSample` with the live source position)
  is suspended for those lane(s).
- [ ] Entering loop-capture mode closes any clip that was open on the deck's
  lane(s) at the deck's source position at the loop-engage instant, finalizing
  its `sourceEndSample` and `timelineEndSample` (the partial first segment of
  §1.5.4).
- [ ] Each completed loop pass (deck source position reaching `loopOutSamples`
  and wrapping to `loopInSamples`, surfaced as a loop-boundary-crossing event by
  PRD-0072) finalizes exactly one `DawClip` with `sourceStartSample = loopInSamples`
  and `sourceEndSample = loopOutSamples`, written to the same lane(s) as the
  deck's active source mode.
- [ ] Each pass clip's `timelineStartSample` equals the immediately preceding
  segment's `timelineEndSample` on that lane (back-to-back, zero gap, zero
  overlap), and its `timelineEndSample - timelineStartSample` equals
  `loopOutSamples - loopInSamples` (each pass occupies exactly the loop's source
  length in timeline samples, since a captured loop plays at the deck's own
  rate).
- [ ] All pass clips produced by a single engaged loop with unchanged bounds
  reference an identical source crop (`sourceStartSample`, `sourceEndSample`,
  `sourceFileId`) and lane id; they are separate `DawClip` records, not one clip
  with a repeat count (see §1.5.1).
- [ ] A mid-loop change to `loopInSamples` and/or `loopOutSamples` (loop halve,
  loop double, manual in/out per PRD-0014) is applied to the *next* finalized
  pass clip; the pass already in flight at the change instant finalizes against
  the bounds it began with (see §1.5.3).
- [ ] Exiting the loop (`loopActive` → `false`, surfaced as a loop-exit event by
  PRD-0072) when the deck is mid-cycle finalizes a partial final pass clip with
  `sourceStartSample = loopInSamples` and `sourceEndSample = currentSourcePosition`
  (the live source position at exit), then resumes an ordinary growing clip from
  `currentSourcePosition`, contiguous on the timeline (see §1.5.4).
- [ ] Exiting the loop exactly at the loop boundary (deck at `loopOutSamples`)
  does not emit a zero-length partial clip; the last full pass is the final loop
  clip and the growing clip resumes from `loopOutSamples`.
- [ ] The lane(s) targeted by every pass clip match the deck's active source
  mode at the loop-engage instant; a source-mode change during an engaged loop
  is governed by PRD-0077 and is out of scope here, but loop capture must not
  prevent PRD-0077 from closing the loop segment and switching lanes.
- [ ] Loop passes shorter than a configured minimum length (very short
  beat-roll loops, e.g. 1/8 or 1/16 beat) are handled per the coalescing policy
  of §1.5.5: either each pass is emitted as a clip, or consecutive identical
  passes are coalesced into one clip annotated with a repeat indicator — the
  resolution in §1.5.5 selects the policy and this criterion asserts the chosen
  behaviour is applied deterministically.
- [ ] Re-loop (re-engaging the most recently defined loop after an exit, per
  PRD-0014 §1.3.4) is treated as a fresh loop-enter event: it closes the current
  growing clip and begins a new loop-capture sequence; the new pass clips are
  contiguous with the growing clip that preceded the re-engage (see §1.5.6).
- [ ] Loop capture introduces no audio-thread allocation, locks, or I/O: the
  loop-enter, loop-boundary-crossing, and loop-exit signals reach the recorder
  only as pre-allocated POD events through PRD-0072's lock-free FIFO; all clip
  mutation occurs on the message thread.
- [ ] Loop capture writes only into the existing `daw` `ValueTree` (EPIC-0008);
  no parallel model and no new top-level state tree is introduced.
- [ ] The grid-alignment rule (PRD-0074) applies to the loop sequence as a whole
  exactly as it does to any other clip: the first pass clip is aligned (grid or
  first-beat) per PRD-0074, and because each pass occupies an identical source
  length, subsequent passes inherit contiguous placement without re-running the
  alignment resolver per pass (see §1.5.4 and §1.5.7).

## 1.5. Grey Areas

### 1.5.1. One Clip Per Pass vs One Clip With a Repeat-Count Attribute

A held loop could be modeled as either N separate identical `DawClip` records
placed back-to-back, or a single `DawClip` carrying a `repeatCount` (or
`loopPasses`) attribute that EPIC-0010 expands at playback. The repeat-count
model is more compact and makes the DJ's intent ("this section repeated K
times") explicit in one record.

**Resolution:** One clip per pass — back-to-back identical crops are emitted as
separate `DawClip` records. This keeps the clip model uniform: every clip in the
arrangement is described solely by `(laneId, sourceFileId, sourceStartSample,
sourceEndSample, timelineStartSample)` with no special loop semantics, so
EPIC-0010 playback and EPIC-0010 editing (move/trim/split/delete) treat a loop
pass exactly like any other clip. A repeat-count attribute would force every
downstream consumer (playback, editing, export, project save) to special-case
loop clips, and would make per-pass editing (e.g. the DJ later trims the third
repetition) impossible without first expanding the count. The modest cost in
record count is acceptable; §1.5.5 addresses the pathological very-short-loop
case where the count could explode.

### 1.5.2. Detecting Each Loop Boundary Crossing as an Event

The capture engine needs to know when a pass completes. This could be derived on
the message thread by polling the deck's source position and detecting the
backward wrap (`position` dropping from near `loopOutSamples` to near
`loopInSamples`), or signalled explicitly by the audio engine as a
loop-boundary-crossing event on the bridge.

**Resolution:** An explicit loop-boundary-crossing POD event surfaced through
PRD-0072. The audio engine already wraps the playhead at `loopOutSamples`
(PRD-0014); at that exact sample it enqueues a small POD event (event type,
deck id, the `loopInSamples`/`loopOutSamples` in effect for the just-completed
pass, timestamp) into the pre-allocated FIFO. Polling the projected source
position on the message thread is unreliable: the projection cadence (EPIC-0008)
can skip over the wrap, double-count a wrap if the loop is shorter than the
projection interval, or miss a boundary entirely for a beat-roll loop. The
explicit event is exact, carries the bounds that were actually in force for that
pass (resolving §1.5.3 cleanly), and adds no audio-thread cost beyond a single
lock-free enqueue.

### 1.5.3. Loop Length Change Mid-Loop

The DJ can halve, double, or manually adjust the loop while it is engaged
(PRD-0014). The pass currently in flight when the bounds change has already been
partly "heard" against the old bounds. Whether the in-flight pass uses old or new
bounds, and when the change takes visible effect, must be defined.

**Resolution:** The next pass uses the new bounds; the in-flight pass finalizes
against the bounds it began with. Because the loop-boundary-crossing event
(§1.5.2) carries the bounds in force for the just-completed pass, the placement
engine always finalizes each pass clip with the bounds that actually played for
that pass. When the DJ doubles a 4-beat loop to 8 beats mid-pass, the current
pass completes as a 4-beat clip (it had already started against the 4-beat
out-point — or, if the new longer out-point is now ahead of the playhead, the
pass continues to the new out-point and is captured as the new length; the
governing rule is simply "the pass clip's crop equals the bounds at the moment
the wrap fires"). Subsequent passes are 8-beat clips. This matches what the DJ
hears and requires no special reconciliation: the event payload is the single
source of truth for each pass's crop.

### 1.5.4. Partial Pass When the Loop Is Entered or Exited Mid-Cycle

A DJ rarely engages a loop exactly on `loopInSamples` or exits exactly on
`loopOutSamples`. The first "pass" typically begins partway through the loop
region (from the engage position to `loopOutSamples`), and the last pass
typically ends partway through (from `loopInSamples` to the exit position).

**Resolution:** The engage instant closes the previously growing clip at the
deck's live source position — that clip is the natural partial *lead-in* and is
not itself a loop pass; it is the ordinary growing clip finalized early. The
first true pass clip begins at the first wrap to `loopInSamples`. On exit, if
the deck is between `loopInSamples` and `loopOutSamples`, a partial final pass
clip is emitted with crop `[loopInSamples, currentSourcePosition)` so the
timeline reflects exactly how much of the final repetition was heard; the
growing clip then resumes from `currentSourcePosition`. If exit coincides with a
boundary crossing (deck at `loopOutSamples`), no zero-length clip is emitted and
the growing clip resumes from `loopOutSamples`. This produces an arrangement
whose total timeline length under the loop equals exactly the wall-clock time the
loop was audible, with no rounding or phantom clips.

### 1.5.5. Very Short Loops (Beat-Roll) Producing Many Clips

Beat-roll loops (1/8, 1/16, 1/32 beat) held for even a second can produce
dozens or hundreds of identical micro-passes, each one a separate `DawClip`
record under §1.5.1. This risks a clip explosion that bloats the `daw` model and
degrades EPIC-0010 editing usability.

**Resolution:** A minimum-pass-length coalescing policy. Passes whose source
length is at or above a threshold (default: one beat at the deck's tempo, with a
hard floor of PRD-0014's 128-sample minimum loop length) are emitted one clip
per pass per §1.5.1. Passes below the threshold are coalesced: consecutive
identical sub-threshold passes accumulate into a single growing clip whose
timeline length spans all the coalesced repetitions, finalized when the loop
length crosses back above the threshold, the bounds change, or the loop exits.
This preserves the one-clip-per-pass model for musically meaningful loops while
preventing pathological record counts for beat-rolls, where the DJ's intent is a
single sustained "roll" gesture rather than a series of distinct sections. The
threshold is a recorder-side constant, not a PRD-0014 concern, and does not
alter audible playback. (If a future Epic adds per-clip repeat metadata, the
coalesced beat-roll clip is the natural place to attach it; this PRD does not
require that metadata.)

### 1.5.6. Reloop and Loop-Move

PRD-0014 §1.3.4 provides re-loop (re-engage the most recently defined loop after
an exit). Some controllers also support loop-move (shifting an active loop's
in/out points together by a beat). Both raise the question of how the capture
engine treats a re-engaged or relocated loop.

**Resolution:** Re-loop is captured as a fresh loop-enter: it closes the current
growing clip at the live source position and starts a new loop-capture sequence,
identical in mechanism to the first engage. The new pass clips are contiguous on
the timeline with whatever preceded the re-engage; there is no special linkage
back to the earlier loop sequence (they are independent runs of identical clips,
possibly with the same crop if the bounds were unchanged). Loop-move is treated
as a bounds change (§1.5.3): the moved in/out points are new bounds carried by
the next loop-boundary-crossing event, so the next pass clip references the
shifted crop while the in-flight pass finalizes against the pre-move bounds. No
additional event type is needed for either case beyond the loop-enter,
boundary-crossing, and loop-exit events of §1.5.2.

### 1.5.7. Interaction With Grid Alignment

PRD-0074's alignment resolver decides whether a clip is grid-aligned (BPM match
and phase-locked) or first-beat-anchored. A loop region is, in the common case,
already an exact beat multiple (auto-loop), so its passes tile the grid
naturally. But manual loops or non-matching BPMs complicate whether each pass
should be re-aligned.

**Resolution:** Alignment is resolved once for the loop sequence, at the first
pass clip, exactly as PRD-0074 resolves any clip; subsequent passes are placed
purely contiguously (each starts at the prior pass's timeline end) without
re-invoking the resolver per pass. Because every pass shares an identical source
length, contiguous placement preserves whatever grid relationship the first pass
established: if the first pass is grid-aligned and the loop is a clean beat
multiple, every pass lands on a grid line; if the first pass is
first-beat-anchored (non-matching BPM), the passes tile contiguously at the
deck's own rate without per-beat snapping, consistent with the rest of the
clip's free placement. Re-aligning each pass independently could introduce gaps
or overlaps between identical repetitions and would contradict the "back-to-back
identical clips" requirement, so the resolver is deliberately run only once per
loop run.
