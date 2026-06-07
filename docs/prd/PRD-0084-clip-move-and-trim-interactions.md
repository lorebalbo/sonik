---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0065
  - PRD-0068
  - PRD-0083
---

# 1. PRD-0084: Clip Move & Trim Interactions

## 1.1. Problem

EPIC-0010 has, by the time this PRD is reached, a fully audible and editable
substrate but no direct-manipulation editing. The arrangement snapshot compiler
(PRD-0065 group), the streaming reader, the render engine, and the DAW transport
all play clips back sample-accurately; PRD-0068 draws each clip as a `ClipBlock`
organism on its lane with a waveform thumbnail; and PRD-0083 provides the edit
command layer (`MoveClip`, `TrimClip`, …) with an undo stack and schedule
recompilation. What is missing is the **mouse layer that turns a drag into a
command**: the DJ can see clips and hear them, but cannot yet grab a clip body
to slide it earlier on the timeline, nor grab a clip edge to shorten the crop.

Without this PRD, every edit modelled in EPIC-0008 and exposed through PRD-0083
is reachable only programmatically (or via tests). The DAW is not usable as an
editor. The core "drag the track so it enters at 0:30 instead of 1:00" and "pull
the tail in to clean up the outro" workflows — the first things any DAW user
reaches for — have no UI.

This PRD owns exactly two of those interactions: **non-destructive MOVE** (slide
a clip along time) and **non-destructive TRIM that only SHORTENS** the crop from
either edge. It is the direct-manipulation front-end for the corresponding
`MoveClip` / `TrimClip` commands. It deliberately stops at the crop the clip
already has: dragging an edge *outward* to reveal more source (UNCROP / EXTEND)
is owned by PRD-0085, and so is SPLIT; this PRD's trim handles travel inward
only. Keeping move/trim-shorten in their own PRD lets the hit-zone, snapping,
clamping, and live-feedback behaviour be specified and tested precisely before
the more delicate uncrop/extend edge-case math (source-bound clamping, reveal
direction) lands on top of the same handles in PRD-0085.

## 1.2. Objective

The system lets the DJ move and trim-shorten clips by dragging, such that:

- A drag on a `ClipBlock`'s **body** moves the clip along its lane, changing
  only `timelineStartSample`; the source crop (`sourceStartSample`,
  `sourceEndSample`) is untouched, so the clip's audio content and length in
  samples are preserved and only its position in the arrangement changes.
- A drag on a `ClipBlock`'s **left edge handle** trims the head: it raises
  `sourceStartSample` (revealing less of the source at the front) and shifts
  `timelineStartSample` by the same delta, so the clip's right edge stays
  pinned in timeline space while its left edge moves inward.
- A drag on a `ClipBlock`'s **right edge handle** trims the tail: it lowers
  `sourceEndSample`, pulling the clip's right edge inward while its left edge
  and `timelineStartSample` stay fixed.
- Both edge handles **only shorten** within this PRD. An attempt to drag an edge
  outward past the clip's current crop is clamped to the current crop boundary
  (no extension); the outward gesture is reserved for PRD-0085's uncrop, which
  reuses the same handles.
- A **snap-to-grid toggle** governs all three drags. When enabled, the dragged
  edge / clip start lands on the nearest master-grid line (PRD-0064 grid via
  PRD-0065's `TimelineTransform`); when disabled, edits are sample-continuous.
- Every drag uses `TimelineTransform` (PRD-0065) for pixel↔sample conversion and
  for resolving the nearest snap target, so the interaction stays correct at any
  horizontal zoom level.
- All edits are **clamped** so they never invert the clip (a clip never reaches
  zero or negative length; a minimum length is enforced) and never exceed the
  available source bounds.
- The drag gives **live, zero-latency visual feedback** (per `DESIGN.md`): the
  clip's drawn rectangle and waveform crop update continuously during the drag,
  and the relevant grid line / snap target is indicated while snapping.
- **One completed drag = exactly one undoable command.** The interaction issues
  a single `MoveClip` or `TrimClip` to PRD-0083 on mouse-up (using PRD-0083's
  coalescing so the live in-drag updates collapse into one undo step), not one
  command per mouse-move event.

## 1.3. User Flow

1. The DJ hovers a `ClipBlock` on a lane. The cursor over the clip body shows a
   move cursor; the cursor within the left or right **edge hit-zone** (a fixed
   pixel-width strip at each end of the clip, see §1.5.3) shows a horizontal
   resize cursor. The hit-zones are visually indicated on hover per `DESIGN.md`
   (a `2px` solid `#2d2d2d` edge emphasis, no new colour).
2. **Move:** the DJ presses on the clip body and drags horizontally. The clip
   rectangle follows the cursor in real time. `TimelineTransform` converts the
   pixel delta to a sample delta; the previewed `timelineStartSample` is
   computed live. If snap-to-grid is on, the previewed start is snapped to the
   nearest grid line and that grid line is highlighted. The clip cannot be
   dragged before sample 0 (its start clamps at the timeline origin).
3. On mouse-up, the interaction issues a single `MoveClip(clipId, newStart)` to
   PRD-0083. The undo stack gains one entry; the schedule recompiles; playback
   reflects the new position. Releasing without having moved past a small dead
   zone issues no command (a click, not a drag).
4. **Trim start:** the DJ presses within the left edge hit-zone and drags right
   (inward). The clip's left edge follows the cursor; the waveform thumbnail
   re-crops live so the DJ sees which part of the source is being hidden. The
   previewed edit raises `sourceStartSample` and shifts `timelineStartSample` by
   the same delta (right edge stays pinned). Dragging left (outward) past the
   current crop is clamped — the edge will not extend (that is PRD-0085).
5. **Trim end:** the DJ presses within the right edge hit-zone and drags left
   (inward). The clip's right edge follows the cursor; the previewed edit lowers
   `sourceEndSample` (left edge and `timelineStartSample` stay fixed). Dragging
   right (outward) past the current crop is clamped.
6. During any trim, if snap-to-grid is on, the moving edge snaps to the nearest
   grid line and the line is highlighted; the clip cannot be trimmed below the
   minimum clip length (§1.5.6), at which point the edge stops following the
   cursor.
7. On mouse-up, the trim issues a single `TrimClip(clipId, edge, newSourceStart
   / newSourceEnd, newTimelineStart)` to PRD-0083 — one undoable command. The
   schedule recompiles and playback reflects the new crop.
8. The DJ toggles **snap-to-grid** (a `DESIGN.md`-compliant button with explicit
   active/inactive fill inversion) in the timeline toolbar; the toggle state is
   read at drag time, so toggling mid-session changes the next drag's behaviour.

## 1.4. Acceptance Criteria

- [ ] A horizontal drag started on a `ClipBlock` body issues exactly one
      `MoveClip` command (PRD-0083) on mouse-up that changes only the clip's
      `timelineStartSample`; `sourceStartSample` and `sourceEndSample` are
      unchanged, so the clip's sample length is preserved.
- [ ] A drag started within the left edge hit-zone, moving inward, issues exactly
      one `TrimClip` command that raises `sourceStartSample` by N samples and
      raises `timelineStartSample` by the same N samples; the clip's right edge
      (its `timelineStartSample + cropLength` in timeline space) is unchanged.
- [ ] A drag started within the right edge hit-zone, moving inward, issues exactly
      one `TrimClip` command that lowers `sourceEndSample`; `sourceStartSample`
      and `timelineStartSample` are unchanged.
- [ ] Dragging either edge outward past the clip's current crop produces no
      extension: the previewed edge clamps at the current crop boundary and the
      issued command (if any) does not lower `sourceStartSample` below its
      pre-drag value nor raise `sourceEndSample` above its pre-drag value.
- [ ] With snap-to-grid enabled, the moved clip start (move) or the moved edge
      (trim) lands on the nearest master-grid line as resolved by
      `TimelineTransform` (PRD-0065); with snap-to-grid disabled, the edit is
      sample-continuous (lands exactly where the cursor maps).
- [ ] All pixel↔sample conversions during the drag go through
      `TimelineTransform` (PRD-0065) and are correct at multiple horizontal zoom
      levels (verified at at least two zoom factors in tests).
- [ ] A clip can never be moved so its `timelineStartSample` is negative; the
      clip start clamps at 0.
- [ ] A trim can never produce a clip shorter than the minimum clip length
      (§1.5.6); when the dragged edge reaches that limit it stops following the
      cursor and the issued command respects the limit.
- [ ] A trim can never invert the crop (`sourceStartSample < sourceEndSample`
      always holds) and never reads outside `[0, sourceLengthSamples]`.
- [ ] During a drag, the `ClipBlock` rectangle and its waveform crop update
      continuously (live feedback per `DESIGN.md`), and no command is issued
      until mouse-up; the per-move-event previews do not push undo entries.
- [ ] The completed drag results in exactly one undo entry; pressing undo once
      restores the pre-drag clip state exactly (position and crop), and redo
      re-applies it. PRD-0083's coalescing collapses the in-drag updates into the
      single step.
- [ ] A press-and-release without exceeding the drag dead-zone threshold issues
      no command and leaves the clip unchanged (treated as a click/selection,
      not an edit).
- [ ] The edge hit-zones are reported by the `ClipBlock` hit-testing such that a
      press within `hitZoneWidthPx` of either edge initiates a trim and a press
      elsewhere on the body initiates a move; on a clip too narrow to host two
      hit-zones plus a body, the resolution defined in §1.5.3 applies.
- [ ] The snap-to-grid toggle is a `DESIGN.md`-compliant button (`2px` solid
      `#2d2d2d` border, explicit active/inactive fill inversion, no
      `border-radius`); its state is read at drag-start so a mid-session toggle
      affects the next drag.
- [ ] No audio-thread code is added or modified by this PRD. Move/trim mutate the
      `daw` `ValueTree` via PRD-0083 commands on the message thread; the audio
      thread continues to read only PRD-0065's published schedule and performs no
      allocation, no locks, and no I/O.
- [ ] No uncrop/extend, split, delete, or gain behaviour is introduced by this
      PRD; those remain owned by PRD-0085 / PRD-0086.
- [ ] Tests under `Tests/` cover: a move on the body, a head trim, a tail trim,
      outward-clamp on each edge, snap-on vs snap-off landing, the minimum-length
      clamp, the negative-start clamp, the single-undo-entry guarantee, and the
      dead-zone click-vs-drag distinction.

## 1.5. Grey Areas

### 1.5.1. Same-Lane Move Only vs Cross-Lane Drag

A move drag could be constrained to the clip's current lane (horizontal only) or
allowed to carry the clip to another lane (Original / Instrumental / Vocal), as
many DAWs permit via vertical drag.

**Resolution:** Same-lane (horizontal-only) move for this Epic. A clip's lane is
its stem assignment (Original / Instrumental / Vocal); moving a clip to a
different lane would mean re-pointing it at a different source stream, which is a
semantically heavier operation than a position change and risks surprising the
DJ ("why did my vocal clip start playing the instrumental?"). EPIC-0010's editing
scope (§1.2.1) lists move as "drag a clip along the timeline (changes
`timelineStartSample`)" — purely temporal. The move interaction therefore ignores
vertical cursor motion and constrains the clip to its origin lane. Cross-lane /
stem-reassignment drag, if ever wanted, is a clean future enhancement that adds a
new command without altering this PRD's `MoveClip` contract; vertical drag is
left free for that future use rather than co-opted now.

### 1.5.2. Snap Granularity and the Snap Toggle UX

Snap-to-grid could snap to every beat, every bar, or an adjustable subdivision,
and the toggle could be a single on/off or a granularity selector.

**Resolution:** For this PRD, snap targets the master grid's **beat** lines as
exposed by `TimelineTransform` (PRD-0065), with a single on/off toggle. Beat
snapping is the DAW-conventional default and is precise enough for the move/trim
workflows here without a granularity menu cluttering the toolbar. The toggle is a
single boolean button (per `DESIGN.md` active/inactive inversion). A finer/coarser
subdivision selector (1/2 beat, bar, triplet) is deferred: `TimelineTransform`
already resolves "nearest grid line of subdivision X," so adding a granularity
control later is a UI addition that does not change this PRD's snap-resolution
call site. A temporary snap override (e.g. hold a modifier to suspend snapping
during a drag) is also deferred to keep this PRD's input handling simple.

**Superseded by PRD-0102.** The snap-to-grid toggle this PRD specified was not
wired in the shipped code; PRD-0102 implements it, adds the deferred granularity
selector (bar / beat / 1/2 / 1/4) and the deferred momentary-bypass modifier
(Cmd/Ctrl), and applies snapping to move, trim, uncrop, split, and ruler scrub.

### 1.5.3. Edge Hit-Zone Width vs Body, and Narrow Clips

The clip needs three press regions — left trim, body move, right trim — but on a
very narrow clip (few pixels wide at low zoom) three full-width zones cannot
coexist.

**Resolution:** Each edge hit-zone is a fixed `hitZoneWidthPx` strip (a small
constant, e.g. 8 px, chosen to be comfortably grabbable without stealing the
body) measured inward from each edge; the remaining centre is the body/move zone.
When the clip is narrower than `2 * hitZoneWidthPx + minBodyPx`, the hit-zones
shrink proportionally and, below a threshold where trim zones would be
un-grabbable, the **whole clip becomes a move zone** and trimming such a clip
requires zooming in first. This avoids a state where a tiny clip is impossible to
move because its body has vanished under two trim zones. The exact `hitZoneWidthPx`
and the narrow-clip threshold are implementation constants documented in the
component; the test suite asserts the resolution rule (press near edge = trim,
press centre = move, sub-threshold clip = move-only) rather than exact pixel
values.

### 1.5.4. Trim Shortening Here vs Extending in PRD-0085 — Boundary Clarity

The trim handles in this PRD and the uncrop handles in PRD-0085 are the *same*
left/right edge handles; the only difference is drag direction (inward = trim,
outward = uncrop). This risks an ambiguous contract about which PRD owns which
half of the gesture.

**Resolution:** This PRD owns the **inward** half of each edge drag (shorten the
crop) and explicitly **clamps the outward half to a no-op** (the edge stops at the
current crop boundary). PRD-0085 will *relax* that outward clamp — extending the
clamp range to the source bounds `[0, sourceLengthSamples]` and emitting an
`UncropClip` command for outward motion — while leaving the inward `TrimClip`
behaviour from this PRD intact. The handles, hit-zones, snapping, and live
feedback specified here are the shared substrate; PRD-0085 adds outward semantics
on top without re-specifying them. This PRD's tests assert the outward clamp so
that PRD-0085's later relaxation is a visible, intentional contract change rather
than a silent regression.

### 1.5.5. Overlap on Move (and on Trim)

Moving a clip could push it to overlap another clip on the same lane, and the
question is whether to forbid the overlap, ripple/push the neighbour, or allow
clips to overlap.

**Resolution:** Allow overlap. EPIC-0010 §1.3.1 already specifies that the render
engine handles same-lane clip boundaries with a short anti-click ramp, and
§1.2.2 lists full user-drawn crossfades as a future enhancement — meaning the
engine is designed to tolerate overlapping/adjacent clips on a lane via the ramp.
Forbidding overlap (collision rejection) or ripple-moving neighbours are both
heavier behaviours that surprise the DJ mid-drag; allowing free overlap keeps the
move interaction predictable (the clip always goes where you drag it) and defers
crossfade authoring to its future Epic. The render engine's existing ramp owns
the audible result of an overlap; this PRD does not arbitrate it.

### 1.5.6. Minimum Clip Length on Trim

A trim must not be allowed to collapse a clip to zero or near-zero length.

**Resolution:** Enforce a minimum clip length, defined as the larger of (a) a
small absolute sample floor (enough to survive the render engine's anti-click
ramp, e.g. at least the ramp length so a clip is never shorter than its own
fade) and (b) one grid subdivision when snap is on. When a trim drag reaches the
minimum, the dragged edge stops following the cursor and the issued `TrimClip`
clamps to exactly the minimum. This prevents zero-length / inverted clips and
guarantees every surviving clip is long enough for the click-free boundary
handling (EPIC-0010 §1.3.1) to apply its ramp. The exact floor is an
implementation constant tied to the ramp length; tests assert that no trim can
produce a clip shorter than it.

### 1.5.7. Drag Feedback: Ghost vs Live

The in-drag preview could be a "ghost" outline (the original clip stays put and a
translucent placeholder follows the cursor, committing on release) or a "live"
update (the actual clip rectangle and waveform move/re-crop continuously).

**Resolution:** Live update, per `DESIGN.md`'s "design for instant hover/active
states — DJs need zero-latency visual feedback" directive. The `ClipBlock`
itself follows the cursor and re-crops its waveform thumbnail in real time during
the drag, giving the DJ an exact preview of the result. Because no command is
issued until mouse-up (and PRD-0083 coalesces), the live preview is purely a view
concern and does not pollute the undo stack. A translucent ghost is avoided both
because it reads as a softer, less "instant" interaction and because translucency
/ alpha layering sits uneasily with the strict monochrome, dithered-depth
language of `DESIGN.md`. The snap target (grid line) is highlighted during the
drag as the only additional feedback affordance.
