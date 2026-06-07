---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0064
  - PRD-0065
  - PRD-0082
  - PRD-0083
  - PRD-0084
  - PRD-0086
---

# 1. PRD-0102: Timeline Direct-Manipulation Polish — Playhead Scrubbing, Grid Snap & Granularity, and Clip Context Menu

## 1.1. Problem

EPIC-0010 delivered an audible, editable arrangement: the DAW transport plays
the recorded set (PRD-0082), and clips can be moved, trimmed, uncropped, split,
deleted, and gain-adjusted through the command layer (PRD-0083 to PRD-0086). The
mouse layer for clip editing exists — a clip body drag moves the clip, edge
drags trim and uncrop, double-click splits, `Delete`/`Backspace` removes the
selected clip. What the DJ still cannot do are three direct-manipulation gestures
that every DAW user reaches for the moment they open a timeline, and whose
absence makes the panel feel like a viewer rather than an editor:

- **There is no way to position where playback starts.** The transport plays
  only from the very beginning (or wherever it was left). The time ruler — the
  header band with bar numbers and the tick band below it — is inert: PRD-0070
  §1.5.6 deliberately resolved the ruler click as a no-op because, at that point
  in the project, no DAW transport existed to seek. The transport now exists
  (`DawTransport::seek`), but nothing drives it from the ruler. The DJ cannot
  click the ruler to park the playhead, and cannot drag along it to scrub.

- **Clip and edge drags ignore the musical grid.** Every move/trim/uncrop drag
  lands on a raw sample position: `ClipBlock::mouseDrag` converts pixels to
  samples and the command applies that value verbatim. PRD-0084 §1.4 called for a
  snap-to-grid toggle, but the shipped code never wired one, so a DJ cannot
  reliably drop a clip exactly on bar 17 or trim exactly to a beat. PRD-0084
  §1.5.2 further deferred any *granularity* control, so even a future snap would
  be locked to a single subdivision — there is no way to choose coarser (bar) or
  finer (1/4-beat) steps for different editing tasks.

- **A clip can only be deleted by keystroke.** `Delete`/`Backspace` works on the
  focused clip, but there is no right-click affordance. New users — and the
  primary user of this project has no DAW muscle memory to fall back on — expect
  to right-click an object to act on it, and currently get nothing. There is also
  no clear, persistent indication of *which* clip is selected, so it is ambiguous
  what a `Delete` keypress will act on.

Without these three gestures the editing surface is incomplete in exactly the
ways a first-time user notices first: they cannot choose where the song plays
from, their edits drift off the grid, and they cannot right-click to delete.

## 1.2. Objective

The system completes the timeline's direct-manipulation surface such that:

- **Ruler scrubbing.** Clicking anywhere on the time ruler (the bar-number header
  band and the tick band, PRD-0066) positions the DAW transport playhead at the
  clicked musical position; pressing and dragging along the ruler moves the
  playhead in real time, the vertical playhead line following the cursor for the
  duration of the drag. Releasing leaves the playhead at the final position, and
  a subsequent **Play** starts from there. All seeking routes through the
  existing `DawTransport::seek` (PRD-0082), which re-primes the clip streamers off
  the audio thread; the ruler interaction adds no audio-thread code.

- **Grid snap toggle with selectable granularity.** A single snap-to-grid toggle
  governs every timeline edit gesture (clip move, head/tail trim, uncrop,
  split-at-cursor) and the ruler scrub. A granularity selector chooses the snap
  subdivision (bar, beat, 1/2-beat, 1/4-beat) used when snap is on. When snap is
  on, the moved clip start / dragged edge / split point / playhead lands on the
  nearest grid line of the chosen subdivision, resolved through the master grid
  (PRD-0064) and `TimelineTransform` (PRD-0065); when snap is off, edits are
  sample-continuous. The toggle and selector are `DESIGN.md`-compliant controls
  in the DAW panel.

- **Clip context menu.** Right-clicking a clip selects it and opens a popup menu
  whose only item, for now, is **Delete**, which removes exactly that one clip
  via the existing `DeleteClip` command (PRD-0086) — never the sibling clips that
  a loop or beat-jump produced. The menu is built so further per-clip actions can
  be added later without re-architecting it.

- **Visible single-clip selection.** Selecting a clip (by clicking it, dragging
  it, or right-clicking it) gives it a clear, `DESIGN.md`-compliant selected
  state so the DJ always knows which clip a `Delete` keystroke or a context-menu
  action will affect. Selection remains single-clip (PRD-0086 §1.5.4).

- All interaction runs on the message thread; the only audio-thread contact is
  the existing `seek` re-prime path. No new audio-thread allocation, lock, or I/O
  is introduced, and every model mutation continues to route through the
  PRD-0083 command layer with undo/redo.

## 1.3. User Flow

1. The DJ has a recorded arrangement open and the DAW transport stopped. They
   want to audition from bar 33, not the top.
2. The DJ clicks the time ruler near bar 33. The vertical playhead line jumps to
   that position; with snap on at "bar" granularity it lands exactly on bar 33.
   Nothing plays yet — the playhead is parked.
3. The DJ presses **Play**. Playback begins from bar 33; the streamers were
   re-primed by the seek so audio starts cleanly at the new position.
4. The DJ wants to find an exact spot. They press on the ruler and drag slowly to
   the right. The playhead line follows the cursor live; while playing, audio
   tracks the scrubbed position per the scrub-during-playback policy (§1.5.1). On
   release, the playhead stays where they let go.
5. The DJ switches the granularity selector from "bar" to "1/4-beat" to make a
   finer edit, leaving the snap toggle on.
6. The DJ drags a clip body. As it moves, its start snaps to the nearest
   1/4-beat line, which is highlighted during the drag. On release, one
   `MoveClip` command is committed (one undo step).
7. The DJ grabs the clip's right edge and drags it outward to uncrop; the edge
   snaps to the 1/4-beat grid and stops at the source end. Releasing commits one
   command.
8. The DJ toggles snap **off** to nudge a clip a few samples for a manual
   crossfade overlap with its neighbour; the drag is now sample-continuous and
   the overlap is permitted (the engine's anti-click ramp handles the seam, per
   PRD-0084 §1.5.5).
9. The DJ right-clicks a clip created by a loop pass. It becomes selected (clear
   selected outline) and a menu appears with a single **Delete** item. Choosing
   it removes only that clip; the other loop-generated clips remain. The DJ
   presses **Cmd/Ctrl-Z** and the clip returns.
10. The DJ clicks a different clip to select it (selected outline moves to it),
    then presses **Delete** on the keyboard; that clip — and only that clip — is
    removed.

## 1.4. Acceptance Criteria

- [ ] A press on the time ruler (header band or tick band) seeks the DAW
      transport to the musical position under the cursor, via
      `DawTransport::seek` (PRD-0082), converting pixel → sample through
      `TimelineTransform` (PRD-0065); the playhead line redraws at the new
      position.
- [ ] A press-drag along the ruler moves the playhead continuously, the playhead
      line following the cursor for the duration of the drag; on release the
      playhead remains at the final position and a subsequent Play starts from
      there.
- [ ] Scrubbing while the transport is playing behaves per the resolved policy in
      §1.5.1 (visual playhead follows live; audio seeks are coalesced; final
      position applied on release) with no audio-thread allocation, lock, or I/O.
- [ ] A single snap-to-grid toggle exists in the DAW panel, is
      `DESIGN.md`-compliant (`2px` solid `#2d2d2d` border, explicit
      active/inactive fill inversion, `Space Mono` label, zero `border-radius`),
      and its state is read at the start of every edit/scrub gesture.
- [ ] A granularity selector exists offering at minimum bar, beat, 1/2-beat, and
      1/4-beat (final set per §1.5.3); the chosen subdivision is the snap target
      for clip move, head trim, tail trim, uncrop, split-at-cursor, and ruler
      scrub when snap is on.
- [ ] With snap on, a clip move lands the clip start on the nearest grid line of
      the selected granularity; a trim/uncrop lands the dragged edge on the
      nearest grid line; a double-click split lands the cut on the nearest grid
      line; a ruler click/scrub lands the playhead on the nearest grid line. The
      relevant grid line is highlighted during a drag.
- [ ] With snap off, every one of the above gestures is sample-continuous (lands
      exactly where the cursor maps), matching today's behaviour.
- [ ] Snapping is computed from the master grid (PRD-0064) via `TimelineTransform`
      (PRD-0065) and is correct at multiple horizontal zoom levels (verified at at
      least two zoom factors in tests).
- [ ] Each completed snapped drag still issues exactly one command on mouse-up via
      PRD-0083 coalescing (one move/trim/uncrop = one undo step); the live
      snapped preview pushes no undo entries.
- [ ] `Cmd+Z` undoes and `Cmd+Shift+Z` redoes the most recent clip edit (move,
      trim, extend, split, delete, gain), with multi-level history — repeated
      `Cmd+Z` walks back through the full action stack and redo walks forward —
      via the shared `juce::UndoManager` behind the PRD-0083 command layer. Bound
      at the top-level content component so it works regardless of which DAW
      sub-component holds focus.
- [ ] Right-clicking a clip selects that clip and opens a popup menu containing a
      single **Delete** item; choosing it issues exactly one `DeleteClip` command
      (PRD-0086) for that clip id only, leaving all sibling clips (including those
      generated by loops/beat-jumps) intact.
- [ ] The context menu is implemented so additional per-clip items can be added
      later without restructuring the menu (an extensible item list, not a
      hard-coded single entry).
- [ ] A clip shows a clear, `DESIGN.md`-compliant selected state when selected by
      click, drag, or right-click; at most one clip is selected at a time
      (PRD-0086 §1.5.4); a `Delete`/`Backspace` keystroke and the context-menu
      Delete both act on exactly the selected clip.
- [ ] Clicking empty timeline space or the ruler does not change the clip
      selection in a way that breaks the Delete target (selection behaviour at
      §1.5.5).
- [ ] No new audio-thread code path is added: ruler scrub, snap, selection, and
      the context menu all run on the message thread; seeking reuses the existing
      `seek` re-prime; clip mutation reuses the PRD-0083 commands.
- [ ] All new controls (snap toggle, granularity selector, context menu, selected
      state) comply with `DESIGN.md`: monochrome `#2d2d2d`/`#fdfdfd`, `2px`
      borders, `Space Mono`, zero `border-radius`, no gradients, dithered depth.
- [ ] Tests under `Tests/` cover: pixel→sample seek mapping at two zoom levels;
      snap rounding to each granularity for move/trim/uncrop/split/scrub; snap-off
      continuity; the single-undo-entry guarantee under snapping; context-menu
      Delete acting on one clip id only; and the single-selection invariant.

## 1.5. Grey Areas

### 1.5.1. Scrub Behaviour While the Transport Is Playing

A ruler drag while stopped is unambiguous (park the playhead). While *playing*,
the gesture could (a) tape-scrub the audio (continuously re-seek so the DJ hears
the audio rush under the cursor), (b) keep the visual playhead following the
cursor while audio keeps playing normally and only jumps on release, or (c) pause
audio for the duration of the drag and resume from the release point. True
tape-scrub re-primes the streamers on every mouse-move, which is expensive and
risks glitching.

**Proposed resolution:** The vertical playhead line follows the cursor in real
time during the drag (satisfying the user's "move the bar in real time"
requirement visually). Audio is **not** tape-scrubbed: actual `seek` calls are
coalesced/throttled during the drag and the authoritative seek is applied on
mouse-up, so playback resumes cleanly from the release point. If the transport
was stopped, it stays stopped and simply parks at the release position. A true
audible scrub mode is deferred as a future enhancement. This keeps the expensive
re-prime off the per-mouse-move path while still giving live visual feedback.

### 1.5.2. Snap Toggle Default and Momentary Bypass

The snap toggle needs a default state, and DAWs commonly offer a modifier to
temporarily suspend (or force) snapping mid-drag without flipping the toggle.
PRD-0084 §1.5.2 deferred the momentary override.

**Proposed resolution:** Snap defaults **on** at **beat** granularity — the
DAW-conventional default and the most useful for a beatgridded set. A held
modifier (proposed: `Cmd`/`Ctrl`) temporarily *suspends* snapping for the
duration of a single drag, letting the DJ make a fine sample-accurate nudge
without toggling the global control; releasing the modifier restores the toggle
state. The modifier is read continuously during the drag. (Note: `Cmd`+wheel is
already taken for clip gain on the clip body; the bypass modifier applies to
drag gestures, not the wheel, so there is no conflict — but this is worth
confirming.)

### 1.5.3. Granularity Option Set and Triplets

The granularity selector needs a concrete list. Options range from a minimal
{bar, beat} to a full musical set including 1/2, 1/4, 1/8 and triplet
subdivisions. More options mean more UI and more grid math; too few defeats the
"smaller or larger steps" request.

**Proposed resolution:** Ship **bar, beat, 1/2-beat, 1/4-beat** as the initial
set — four steps spanning coarse arrangement moves to fine beat edits, all
expressible as integer subdivisions the master grid already provides. 1/8-beat
and triplet subdivisions are deferred; the selector is built as an ordered list
so adding them later is a data change, not a redesign. The selector is a compact
cycling button or dropdown in the DAW toolbar next to the snap toggle.

### 1.5.4. Does the Ruler Scrub Obey the Snap Toggle?

The snap toggle clearly governs clip edits, but it is ambiguous whether the
*playhead* should also snap when scrubbing, since a DJ auditioning a spot may
want sample-precise parking, while another wants the playhead locked to the
grid.

**Proposed resolution:** The ruler scrub obeys the **same** snap toggle and
granularity as clip edits — one global snap state, applied uniformly. When snap
is on, clicking/scrubbing parks the playhead on the nearest grid line of the
chosen subdivision; the momentary bypass modifier (§1.5.2) gives sample-precise
parking on demand. This keeps a single mental model ("snap is on or off") rather
than a separate per-surface snap setting.

### 1.5.5. Selection Model: What Selects, What Deselects, and the Delete Target

Today a clip grabs keyboard focus on mouse-down but there is no explicit,
visible selection, and it is unspecified what clears selection or what `Delete`
acts on when nothing is selected.

**Proposed resolution:** Exactly one clip is selected at a time. Clicking a clip
body, starting a drag/trim on it, or right-clicking it makes it the selected
clip and shows the selected outline. Clicking empty timeline space or the ruler
**does not** clear the selection (so a ruler click to position the playhead does
not silently disarm a pending Delete); selection only moves when another clip is
selected. A `Delete`/`Backspace` keystroke with no selection is a no-op.
Multi-select (marquee, shift-click) stays deferred to a future editing-polish
effort, consistent with PRD-0086 §1.5.4.

### 1.5.6. Context Menu Contents Now vs Later

The user asked for a context menu that "currently only includes the Delete
button." The question is whether to include anything else now (Split, gain
reset, etc.) given those operations already exist via other gestures.

**Proposed resolution:** **Delete only** for this PRD, to match the request
exactly and avoid scope creep, but the menu is implemented as an extensible item
list (the same `juce::PopupMenu` idiom DawPanel already uses for "Import Audio
File", PRD-0098) so Split, per-clip gain entry, rename, or duplicate can be
appended later with a one-line addition and a command call. No confirmation
dialog on Delete — recovery is via undo (PRD-0086 §1.5.3).

### 1.5.7. Scrubbing Past the Viewport Edge

If the DJ drags the playhead to the right edge of the visible timeline, the
arrangement may extend further than what is on screen. The gesture could stop at
the edge or auto-scroll the view to keep following the cursor.

**Proposed resolution:** For this PRD the scrub tracks the cursor within the
visible ruler; dragging to the edge does **not** auto-scroll (auto-scroll-while-
scrubbing is deferred, consistent with the Follow-mode policy in PRD-0070
§1.5.2, which keeps manual navigation in the DJ's control). The DJ scrolls the
view (existing wheel/drag pan) and continues scrubbing. Edge auto-scroll during
a scrub is a clean future enhancement.
