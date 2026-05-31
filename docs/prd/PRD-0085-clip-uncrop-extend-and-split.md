---
status: Not Implemented
epic: EPIC-0010
depends-on:
  - PRD-0063
  - PRD-0068
  - PRD-0083
  - PRD-0084
---

# 1. PRD-0085: Clip Uncrop / Extend & Split

## 1.1. Problem

A recorded clip on the `daw` timeline (EPIC-0008 §1.3.1) is a reference into a
source file: a crop window `[sourceStartSample, sourceEndSample]` inside the
source bounds `[0, sourceLengthSamples]`, placed at `timelineStartSample`. When
the DJ performed live, that crop captured only the portion of the song they
actually played — perhaps the track was dropped at 1:00 and only the back half
was ever heard. But the *entire* source file is still on disk, fully intact and
referenced by the clip. None of the un-played audio was lost; it was simply
never inside the crop window.

PRD-0084 delivered the trim interactions: dragging a clip edge *inward* to
**shorten** the crop. That is half of the edge-drag model. The other half — the
flagship operation of this Epic (EPIC-0010 §1.1) — is the inverse: dragging a
clip edge *outward* to **uncrop / extend**, lengthening the crop window to
reveal more of the source song that was never played live. This is the "start
the track at 0:30 instead of 1:00" / "extend the intro" workflow that
distinguishes a non-destructive DAW from a destructive recorder: the audio to
reveal is *already there*, behind the crop boundary, waiting to be exposed.

Separately, DAW users expect to **split** a clip into two contiguous halves at
the playhead — to apply different edits, gain, or (in a later Epic) automation
to each part. Split must partition one clip into two that together occupy
exactly the same timeline span and reference exactly the same source span as the
original, with no gap, overlap, or sample loss.

Both operations are pure crop-window arithmetic over the clip value object. They
must never stretch, resample, fabricate, or discard audio, and — running through
the command layer (PRD-0083) — must be fully undoable. Without them, the Epic's
central promise (reconstruct *and reshape* the performed mix losslessly) is
unfulfilled: the DJ can shorten what they played but never recover what they
didn't.

## 1.2. Objective

The system delivers uncrop/extend and split as non-destructive clip edits over
the `daw` model, expressed through the command layer (PRD-0083), such that:

- **Uncrop start**: dragging a clip's left edge outward (earlier on the
  timeline) lowers `sourceStartSample` toward `0` and shifts
  `timelineStartSample` left by the same sample delta, revealing earlier source
  audio while keeping the clip's right edge and source-end fixed. The crop window
  grows; the audio under the newly exposed region is read from the source at full
  quality.
- **Uncrop end**: dragging a clip's right edge outward (later on the timeline)
  raises `sourceEndSample` toward `sourceLengthSamples`, revealing later source
  audio while keeping the left edge and source-start fixed.
- Both uncrop directions are clamped at the source bounds: `sourceStartSample`
  cannot go below `0`, `sourceEndSample` cannot exceed `sourceLengthSamples`. At
  the clamp, the edge stops moving — the DJ has reached the start or end of the
  source file and there is no more audio to reveal.
- **Split**: invoking split on a clip at a cut sample `C` (where
  `timelineStartSample < C < timelineStartSample + cropLength`) replaces the one
  clip with two contiguous clips sharing the same `sourceFileId`: clip A spans
  source `[sourceStartSample, sourceStartSample + (C - timelineStartSample)]` at
  the original `timelineStartSample`; clip B spans the remaining source
  `[that boundary, sourceEndSample]` at timeline position `C`. The two clips
  abut exactly: no gap, no overlap, no lost sample.
- Every uncrop and split is emitted as a command (`UncropClip`, `SplitClip`) via
  PRD-0083, pushed onto the undo stack, and triggers a schedule recompile and
  republish (EPIC-0010 §1.3.2) so playback reflects the edit.
- Uncrop and split honour the grid-snap toggle (PRD-0084 / EPIC-0008 master
  grid): when snap is on, the dragged edge and the split point land on grid
  lines via `TimelineTransform` (PRD-0065) pixel↔sample conversion.
- Edges newly revealed by uncrop display their waveform by reusing the cached
  waveform data for the whole source (PRD-0068), not by re-analysing; the
  revealed region was always part of the source's waveform cache.
- No uncrop or split operation performs allocation, locking, or I/O on the audio
  thread: all mutation happens on the message thread; the audio thread only ever
  reads the recompiled, lock-free-published schedule.

This PRD is the *outward* counterpart to PRD-0084's *inward* trim. Together they
form one **unified edge-drag model**: the same physical drag handle on each clip
edge produces trim when dragged toward the clip body and uncrop when dragged away
from it. §1.5.1 specifies this unification precisely.

## 1.3. User Flow

1. The DJ has a recorded clip on a lane. It was captured from 1:00 into the
   source track; the source file is 6:00 long. The clip's crop window is
   `[sourceStartSample = 1:00, sourceEndSample = 3:30]`, placed at
   `timelineStartSample = bar 16`.

2. The DJ hovers the clip's **left edge**. The cursor changes to the edge-drag
   handle (shared with PRD-0084's trim). Because there is source audio *before*
   the current `sourceStartSample` (1:00 > 0), dragging outward (left) is
   permitted.

3. The DJ drags the left edge **outward** (to the left). As they drag:
   - `sourceStartSample` lowers toward `0` (revealing audio from before 1:00).
   - `timelineStartSample` shifts left by the identical sample delta, so the
     clip's right edge and the audio already under it do not move on the
     timeline — only earlier material is prepended.
   - The clip's waveform redraws, the newly exposed left region painting from
     the source's cached waveform (PRD-0068).
   - With grid snap on, the left edge snaps to the nearest grid line.

4. The DJ drags far enough that `sourceStartSample` would go below `0`. The edge
   **stops at the source start**: the clip now begins at exactly 0:00 of the
   source; no further outward drag is possible on this edge. The DJ has
   "extended the intro" to the very beginning of the track.

5. The DJ releases. An `UncropClip` command (PRD-0083) captures the before/after
   crop window, pushes onto the undo stack, and recompiles the schedule. On the
   next DAW play, the clip enters earlier and plays the previously un-played
   intro at full quality.

6. The DJ later positions the **transport playhead** (PRD's DAW transport) over
   a clip and invokes **Split** (menu / keyboard). The clip is replaced by two
   contiguous clips at the playhead sample; both reference the same source file,
   partitioned at the cut. With grid snap on, the cut lands on the nearest grid
   line. A `SplitClip` command records the operation for undo.

7. The DJ presses **Undo**. The `SplitClip` command inverts: the two clips
   collapse back into the original single clip. A second Undo inverts the
   `UncropClip`: the left edge returns to its pre-uncrop crop window. Nothing was
   ever destroyed; every edit was a reversible crop-window mutation.

## 1.4. Acceptance Criteria

- [ ] Dragging a clip's left edge outward (earlier) lowers `sourceStartSample`
      and shifts `timelineStartSample` left by the same sample count, leaving the
      clip's right edge fixed on the timeline and revealing earlier source audio.
- [ ] Dragging a clip's right edge outward (later) raises `sourceEndSample`,
      leaving the clip's left edge fixed and revealing later source audio.
- [ ] Uncrop of the left edge is clamped so `sourceStartSample` never goes below
      `0`; the edge stops at the source start and refuses further outward drag.
- [ ] Uncrop of the right edge is clamped so `sourceEndSample` never exceeds
      `sourceLengthSamples`; the edge stops at the source end.
- [ ] No uncrop in either direction ever changes the audio already inside the
      crop window; uncrop only prepends/appends previously-hidden source samples,
      and is fully reversible (it is a pure crop-window adjustment, never a
      stretch or resample).
- [ ] `SplitClip` at cut sample `C` (strictly inside the clip's timeline span)
      replaces the clip with two clips sharing `sourceFileId`: clip A =
      `[sourceStartSample, boundary]` at the original `timelineStartSample`, clip
      B = `[boundary, sourceEndSample]` at timeline position `C`, where
      `boundary = sourceStartSample + (C - timelineStartSample)`.
- [ ] After split, the two clips are exactly contiguous on the timeline (clip B's
      `timelineStartSample` equals clip A's `timelineStartSample + cropLengthA`)
      with no gap, no overlap, and no lost or duplicated source sample.
- [ ] A split invoked at a cut sample at or outside the clip's span (`C <=
      timelineStartSample` or `C >= timelineStartSample + cropLength`) is a no-op
      and produces no command on the undo stack.
- [ ] Both halves of a split inherit the original clip's per-clip gain and lane
      assignment (see §1.5.4).
- [ ] Uncrop and split are each emitted as a command (`UncropClip`, `SplitClip`)
      via PRD-0083, pushed onto the undo stack, and are individually undoable and
      redoable; undoing a split recombines the two clips into the original, and
      undoing an uncrop restores the prior crop window exactly.
- [ ] When grid snap is enabled, the uncropped edge and the split point land on
      the nearest master-grid line, using `TimelineTransform` (PRD-0065) for
      pixel↔sample conversion; when snap is disabled, both are sample-accurate to
      the cursor / playhead (see §1.5.6).
- [ ] Newly revealed regions from uncrop render their waveform from the source's
      existing cached waveform data (PRD-0068) without triggering re-analysis.
- [ ] The same physical edge-drag handle produces trim (PRD-0084) when dragged
      inward and uncrop (this PRD) when dragged outward; the two PRDs share one
      handle and one drag gesture, distinguished only by direction relative to
      the clip body (see §1.5.1).
- [ ] All uncrop and split mutation occurs on the message thread through the
      command layer; the audio thread performs no allocation, takes no lock, and
      does no I/O — it reads only the recompiled, lock-free-published schedule
      (EPIC-0010 §1.3.1, §1.3.2).
- [ ] Tests under `Tests/` cover: uncrop-start sample arithmetic and clamp at
      `0`; uncrop-end clamp at `sourceLengthSamples`; uncrop preserving the
      interior crop audio; split partition arithmetic and contiguity; split
      gain/lane inheritance; split no-op outside span; undo/redo of both
      commands; and grid-snap rounding of the uncropped edge and split point.

## 1.5. Grey Areas

### 1.5.1. Unified Edge-Drag Model: Trim (Inward) vs Uncrop (Outward)

Trim (PRD-0084) shortens the crop by dragging a clip edge toward the clip body;
uncrop (this PRD) lengthens it by dragging the same edge away from the body. They
are the same drag handle on the same edge, in opposite directions. The risk is a
split-brain implementation: two handles, two gestures, two code paths that
disagree at the boundary (the moment the drag crosses the clip's original edge).

**Resolution:** One handle, one gesture, one continuous drag interaction per
edge. The edge-drag handler computes the candidate new `sourceStartSample` (left
edge) or `sourceEndSample` (right edge) from the cursor position via
`TimelineTransform`, then clamps it to the full source bounds
`[0, sourceLengthSamples]`. Whether the result is *smaller* (trim) or *larger*
(uncrop) than the current crop is irrelevant to the geometry: the same clamp and
the same `timelineStartSample` coupling (for the left edge) apply throughout. The
DJ can drag inward past the original edge into trim, then back outward through
the original edge into uncrop, in a single fluid gesture, with no discontinuity
at the crossover. The *command* emitted on release is chosen by comparing final
vs initial crop: a net-shorter crop emits PRD-0084's `TrimClip`, a net-longer one
emits this PRD's `UncropClip` (or a single unified `SetClipCrop` command may back
both — see §1.5.7). The hard rule: the live drag math is one shared function
spanning both PRDs; only the clamp at `0` / `sourceLengthSamples` ever stops the
edge, and that clamp is the sole behavioural difference between trim and uncrop
(uncrop can hit the source bound; trim hits the opposite edge / minimum-length
floor instead).

### 1.5.2. Clamping at Source Bounds

Uncrop cannot reveal audio that does not exist: the source file is finite. The
left edge cannot pass `sourceStartSample = 0`; the right edge cannot pass
`sourceEndSample = sourceLengthSamples`. The question is the *feel* at the clamp:
does the edge stop dead, rubber-band, or give a visual "end of source" cue?

**Resolution:** The edge stops dead at the bound — the drag continues to track
the cursor in pixels, but the clip edge pins at the source boundary and does not
move further. This matches every DAW's clip-edge behaviour at the media end
(Ableton, Logic, Pro Tools all pin the edge at the clip's source extent). No
rubber-band, no elastic overshoot. A subtle visual cue (the edge handle changing
to an "at source end" state — e.g. a different border treatment per `DESIGN.md`)
may indicate the bound has been reached, but is optional polish, not required by
this PRD's contract. The hard guarantee is the clamp: `sourceStartSample =
max(0, candidate)` and `sourceEndSample = min(sourceLengthSamples, candidate)`,
enforced in the shared edge-drag math (§1.5.1) so neither trim nor uncrop can
ever exceed the source bounds.

### 1.5.3. Split Point Source: Playhead vs Cursor Click vs Both

A split needs a cut sample. It could come from the **transport playhead** (split
where playback is parked), a **cursor click / right-click context position** on
the clip (split where the mouse is), or **both** (whichever the invocation
implies).

**Resolution:** Support both, with the invocation determining the source. A
keyboard-shortcut / menu split uses the **transport playhead** sample (the DAW's
current play position), matching the DAW convention "split at playhead." A
context-menu split invoked by right-clicking a clip uses the **click position**
on that clip (converted to a sample via `TimelineTransform`). Both feed the
identical `SplitClip` command with a cut sample `C`; the command does not care
where `C` originated. This gives the DJ the fast keyboard workflow (park
playhead, hit split) and the precise mouse workflow (right-click exactly where to
cut) without two divergent code paths — only the *source of `C`* differs at the
UI layer; the command and its validation (`timelineStart < C < timelineEnd`) are
shared. When grid snap is on, `C` is snapped before the command is built (§1.5.6),
regardless of which source produced it.

### 1.5.4. Split Inheritance of Gain and Lane

When one clip becomes two, both halves must inherit the original's per-clip gain
(EPIC-0008) and lane (Original / Instrumental / Vocal) assignment. The question
is whether anything *should* differ between the halves.

**Resolution:** Both halves inherit identical per-clip gain and the identical
lane assignment from the parent clip; split changes only the crop partition and
timeline placement, nothing else. A split is a *spatial* cut, not a content or
routing change — making the halves differ in gain or lane would surprise the DJ
(who expects the audio to play identically immediately after a split, just as two
clips instead of one). The whole point of splitting is to *then* edit the halves
independently (different gain, later different automation in EPIC-0011); at the
moment of the split itself, the two clips must be audibly indistinguishable from
the original single clip. Therefore: copy `gain`, `lane`, `sourceFileId`, and any
other per-clip non-crop properties verbatim to both halves; partition only
`sourceStartSample` / `sourceEndSample` / `timelineStartSample`. The undo of a
split relies on this: recombining requires the two halves to be confirmed
contiguous, same-source, and same-properties, else the undo is rejected as
inconsistent (which cannot happen for a split this PRD produced).

### 1.5.5. Uncrop Revealing an Un-Analysed Waveform Region

Uncrop exposes source samples that were outside the original crop and therefore
may never have been drawn as a clip waveform before. The concern is whether the
newly revealed region needs fresh waveform analysis (potentially a visible delay
or a blank patch) when it scrolls into view.

**Resolution:** No fresh analysis is needed; reuse the source's existing cached
waveform (PRD-0068). The waveform cache is keyed to the **whole source file**,
not to the clip's crop window — PRD-0068 analyses and caches the entire source's
waveform overview when the source is first imported / decoded. A clip merely
*displays a window into* that whole-source cache. Uncropping widens the displayed
window, but the cache for the revealed region already exists; the renderer simply
reads a wider slice of the same cached overview. There is no re-analysis, no
blank patch, and no delay: the moment the edge is dragged outward, the revealed
waveform paints from cache. The only edge case is a source whose waveform cache
has been evicted (memory pressure); in that case PRD-0068's normal
cache-miss-and-regenerate path applies uniformly to the whole source (not to the
uncropped region specially), and is out of scope for this PRD beyond requiring
that uncrop trigger no *crop-specific* analysis.

### 1.5.6. Snap Behaviour on Uncrop and Split

Grid snap (the master-grid toggle) governs all edits. For uncrop, the dragged
edge should snap; for split, the cut point should snap. The subtlety is *what*
snaps to the grid and whether snapping can violate the source-bound clamp.

**Resolution:** When snap is on, the **timeline position** of the uncropped edge
snaps to the nearest master-grid line (via `TimelineTransform`, PRD-0065), and
the corresponding `sourceStartSample` / `sourceEndSample` is derived from that
snapped timeline position. The split cut sample `C` likewise snaps to the nearest
grid line before the `SplitClip` command is built. Snapping is applied *before*
the source-bound clamp (§1.5.2): if a snap would push the edge past `0` or
`sourceLengthSamples`, the clamp wins and the edge pins at the source bound even
though that bound is not on a grid line — revealing all available audio takes
precedence over grid alignment at the very ends of the source. When snap is off,
the edge and the cut are sample-accurate to the cursor / playhead. The snap
toggle is the single shared toggle used by move/trim (PRD-0084); this PRD adds no
new toggle, only honours the existing one for the uncrop edge and the split cut.

### 1.5.7. Overlap After Uncrop and the Anti-Click Ramp

Uncropping a clip's edge outward can grow it into a neighbouring clip on the same
lane, producing an overlap. EPIC-0010 §1.2.2 defers full user-drawn crossfades;
only the short anti-click ramp (§1.3.1) is in scope. The question is what uncrop
does when it would overlap a neighbour.

**Resolution:** Permit the overlap; the render engine's existing short anti-click
ramp (EPIC-0010 §1.3.1, ~64-sample boundary ramp) handles the seam, exactly as it
does for any same-lane clip boundary. Uncrop does **not** push, trim, or delete
the neighbouring clip, and does **not** draw a full crossfade — both are out of
scope (full crossfades are a future enhancement per §1.2.2). When two clips
overlap on a lane after an uncrop, the later-starting clip's audio takes
precedence in the overlap region (or the engine's documented same-lane overlap
policy applies), with the anti-click ramp smoothing the transition into and out
of the overlap. This keeps uncrop a *pure single-clip* crop-window mutation — it
never reaches across to mutate a neighbour, preserving the command's simple
inverse for undo. If the DJ wants a clean abutment instead of an overlap, they
trim the neighbour (PRD-0084) as a separate, independently-undoable edit. A future
crossfade Epic may layer real crossfades over these overlaps without changing this
PRD's contract.
