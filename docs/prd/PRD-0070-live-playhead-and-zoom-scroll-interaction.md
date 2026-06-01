---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0066
  - PRD-0068
  - PRD-0069
---

# 1. PRD-0070: Live Playhead / Now-Line & Zoom/Scroll Interaction Polish

## 1.1. Problem

By the time this PRD is reached, the DAW timeline canvas is structurally
complete: the `daw` ValueTree branch and non-destructive clip model exist
(PRD-0063), the master-grid service reconciles bars/beats against the
authoritative `MasterClockSnapshot` (PRD-0064), the `TimelineTransform` maps
samples ↔ beats ↔ pixels and already carries the zoom/scroll math (PRD-0065),
the docked panel shell and time ruler render (PRD-0066), channel groups and
three-lane layout are laid out (PRD-0067), clip blocks paint reusing the
PRD-0006 waveform cache (PRD-0068), and the `LiveProjectionTimer` extends live
clips from each deck's published source position (PRD-0069).

What is missing is the layer that makes this canvas legible and navigable as a
single coherent instrument. Three gaps remain:

- There is no **live playhead / now-line**. The DJ can see clips growing, but
  nothing draws the single authoritative vertical line marking "where the live
  projection currently is" on the timeline. Without it, the relationship
  between the advancing live clip's leading edge and the musical grid is
  ambiguous, and the panel reads as a passive picture rather than a live view.

- The **zoom and scroll math exists but is not driven by input**. PRD-0065
  delivered `TimelineTransform` with a focal-point-anchored zoom function and a
  clamped scroll offset, but no mouse-wheel, drag, pinch, or scrollbar gesture
  is wired to it. The DJ cannot change bars-per-screen or pan along the
  timeline; the view is stuck at whatever default the shell initialised.

- There is no **follow / auto-scroll** behaviour. As the live projection
  advances past the right edge of the viewport, the now-line marches off-screen
  and the DJ loses sight of the action unless they manually scroll. A standard
  DAW keeps the now-line in view via an opt-in follow mode.

This is the final, "polish" PRD of EPIC-0008. It introduces no new audio-thread
code and no transport: it does not add the DAW's own Record or Play (those are
EPIC-0009 and EPIC-0010). The playhead it draws is a **live now-line** that
mirrors the deck/master-clock-driven live projection — it is explicitly *not* a
transport cursor the user can scrub, seek, or press play against.

## 1.2. Objective

The system completes the read-only DAW timeline as a fully navigable, live view
such that:

- A **`Playhead` atom** (`Source/Features/Daw/Ui/Atoms/Playhead.h/.cpp`) draws a
  single vertical now-line at the current live timeline position, spanning the
  full height of all lanes/channel groups in the panel, fully `DESIGN.md`
  compliant (solid `#2d2d2d`, 1–2 px, pixel-aligned, zero glow, zero gradient).
- The now-line position is derived on the message/UI thread from the live
  projection's leading edge (PRD-0069) / master-grid position (PRD-0064),
  converted to a screen x-coordinate through `TimelineTransform` (PRD-0065). It
  is purely presentational and never writes back to any deck, mixer, or clock.
- **Horizontal zoom** (musical bars-per-screen) is driven by mouse wheel,
  modifier+wheel, and trackpad pinch, anchored to the cursor focal point so the
  timeline point under the cursor stays put while the surrounding view scales,
  using `TimelineTransform`'s existing focal-anchored zoom.
- **Horizontal scroll** is driven by wheel, click-drag pan, and a horizontal
  scrollbar, clamped to the valid timeline extent so the view cannot scroll into
  negative time or arbitrarily far past the live content.
- **Vertical scroll / lane collapse** handles the case where the number of decks
  × lanes overflows the panel height, coordinating with the channel-group layout
  (PRD-0067) without introducing a competing layout authority.
- An optional **follow / auto-scroll mode** keeps the now-line in view as the
  live projection advances; it is toggleable, and a manual horizontal scroll or
  pan gesture temporarily disengages it (see §1.5.2).
- All interaction runs on the message/UI thread. The only audio-thread contact
  is **reading** existing per-deck atomics and the master-clock SeqLock snapshot
  (per §1.3.8 of EPIC-0008); this PRD adds no new audio-thread code path, no
  allocation on the audio thread, no locks, and no I/O.

## 1.3. User Flow

1. The DJ loads a track on Deck A and presses play on the deck. The live
   projection (PRD-0069) begins growing a clip on Deck A's active lane(s).
2. The `Playhead` atom draws a vertical now-line at the leading edge of the
   advancing live projection, spanning every visible lane from the top of the
   first channel group to the bottom of the last. The line stays pixel-aligned
   and crisp at every zoom level.
3. The DJ rolls the mouse wheel (or the configured modifier+wheel, see §1.5.1)
   over the timeline. The view zooms in/out in musical bars-per-screen, anchored
   to the timeline position under the cursor, which remains stationary on screen
   while the grid, ruler ticks, clips, and now-line scale around it.
4. The DJ scrolls horizontally (wheel, click-drag pan, or the scrollbar). The
   view pans along the timeline, clamped so it cannot pan before bar 0 or beyond
   the clamped right extent. The now-line, grid, ruler, and clips all translate
   together by the same offset.
5. The DJ enables **Follow** (a `DESIGN.md`-compliant toggle in the panel). As
   the live projection advances and the now-line reaches the configured trigger
   zone near the right edge, the view auto-scrolls to keep the now-line in view.
6. While Follow is on, the DJ manually scrolls or pans. Follow disengages so the
   manual navigation is honoured; it re-engages per the policy in §1.5.2 (e.g.
   on the next explicit Follow toggle, or when the now-line re-enters view).
7. With more decks/lanes active than fit vertically, the DJ scrolls vertically
   (or collapses a channel group via PRD-0067's affordance). The now-line still
   spans the full stacked height of the visible lanes and remains aligned.
8. The DJ clicks on the time ruler. Nothing seeks — there is no transport yet
   (see §1.5.6). The click is inert (or, at most, surfaces an informational
   read-out of the musical time at that point); it never moves the now-line.

## 1.4. Acceptance Criteria

- [ ] A `Playhead` atom exists at `Source/Features/Daw/Ui/Atoms/Playhead.h` and
      `Source/Features/Daw/Ui/Atoms/Playhead.cpp`, owning only the rendering of
      the now-line; it holds no tempo, transport, or model state of its own and
      receives its x-position (or the timeline sample/beat to convert) from its
      parent.
- [ ] The now-line is drawn as a single vertical line, solid `#2d2d2d`, 1–2 px
      wide, with zero `border-radius`, zero glow, and zero gradient, pixel-aligned
      so it never renders blurry at any zoom level, per `DESIGN.md`.
- [ ] The now-line spans the full vertical extent of all visible lanes / channel
      groups in the panel (from the top of the first lane to the bottom of the
      last), and continues to span correctly when channel groups are collapsed,
      expanded, or vertically scrolled (PRD-0067).
- [ ] The now-line position is computed on the message/UI thread from the live
      projection leading edge (PRD-0069) and/or the master-grid position
      (PRD-0064), converted to screen x via `TimelineTransform` (PRD-0065). It
      is read-only with respect to deck, mixer, and master-clock state.
- [ ] Mouse-wheel (and the configured modifier+wheel, §1.5.1) over the timeline
      changes the horizontal zoom in musical bars-per-screen via
      `TimelineTransform`, anchored to the cursor focal point so the timeline
      position under the cursor stays fixed on screen across the zoom.
- [ ] Trackpad pinch on macOS changes the horizontal zoom equivalently, anchored
      to the pinch focal point, mapped through the same `TimelineTransform` zoom
      entry point as wheel zoom.
- [ ] Horizontal scroll is supported via wheel, click-drag pan, and a horizontal
      scrollbar; all three update the same clamped `TimelineTransform` scroll
      offset and keep grid, ruler, clips, and now-line in lockstep.
- [ ] Horizontal scroll/zoom are clamped so the view cannot pan before timeline
      sample 0 (bar 0) and cannot zoom beyond the documented min/max
      bars-per-screen limits (§1.5.4); attempting to exceed a limit saturates at
      the limit rather than overshooting or wrapping.
- [ ] Vertical scrolling and/or lane collapse handle the deck × lane overflow
      case in coordination with PRD-0067; this PRD does not introduce a second,
      competing vertical layout authority (§1.5.5).
- [ ] A Follow / auto-scroll toggle exists, is `DESIGN.md`-compliant (`2px solid
      #2d2d2d` border, explicit active/inactive fill inversion, `Space Mono`
      label, zero border-radius), and when enabled keeps the now-line in view as
      the live projection advances.
- [ ] When Follow is enabled and the DJ performs a manual horizontal scroll or
      pan, Follow disengages so the manual navigation is honoured, and re-engages
      per the documented policy (§1.5.2). The disengage/re-engage state is UI-only
      and never affects audio or model state.
- [ ] Clicking the time ruler does not seek, move the now-line, or alter any
      deck/clock/transport state; the ruler click is inert or at most surfaces an
      informational musical-time read-out (§1.5.6).
- [ ] No keyboard shortcut for zoom/scroll/follow is introduced by this PRD
      (deferred, §1.5.7); all interaction is pointer/scrollbar/toggle driven.
- [ ] No new audio-thread code is added; the only audio-thread contact is reading
      the existing per-deck atomics and the master-clock SeqLock snapshot. No
      allocation, lock, or I/O occurs on the audio thread as a result of this PRD.
- [ ] No DAW transport, Record, or Play control is added; the playhead is a live
      now-line mirroring the live projection, not a user-scrubbable transport
      cursor (§1.5.3).
- [ ] Unit/UI tests cover: now-line x-position derivation through
      `TimelineTransform`, focal-anchored zoom invariance (the point under the
      cursor maps to the same screen x before and after a zoom step), scroll
      clamping at both ends, and the Follow engage/disengage state machine.

## 1.5. Grey Areas

### 1.5.1. Zoom Input Gesture Mapping (Wheel vs Modifier+Wheel vs Pinch)

On macOS, raw two-finger trackpad scroll arrives as wheel events and is the
natural gesture for *panning*, while pinch is the natural gesture for *zoom*. On
Windows, most mice have only a wheel and no pinch, so a modifier (Ctrl/Cmd) is
the conventional zoom affordance. Mapping the same physical wheel to both pan and
zoom risks accidental zooming.

**Resolution:** Plain wheel = horizontal scroll/pan; modifier+wheel (Ctrl on
Windows, Cmd on macOS) = horizontal zoom; trackpad pinch = horizontal zoom
(macOS). All three zoom paths funnel through the single focal-anchored zoom entry
point on `TimelineTransform` (PRD-0065), so the anchoring and clamping logic is
shared and tested once. This matches the dominant DAW convention (modifier+wheel
to zoom, wheel to scroll) and keeps plain wheel — the most common gesture —
non-destructive to the zoom level. Shift+wheel as an alternate horizontal-scroll
accelerator is permitted but not required.

### 1.5.2. Follow-Mode Default and How Manual Scroll Interrupts It

Follow could default on (the timeline always chases the live projection, which is
friendly for "just watch it play") or default off (the DJ owns the viewport, which
respects manual inspection). And once a manual scroll interrupts Follow, it could
re-engage automatically (after a timeout, or when the now-line re-enters view) or
only re-engage on an explicit toggle.

**Resolution:** Follow defaults **off**. The DAW in this Epic is a read-only
inspection surface; defaulting to auto-scroll would yank the viewport away from a
DJ who is examining an earlier part of the arrangement. When Follow is **on**, any
manual horizontal scroll/pan **disengages** it immediately (the manual gesture
wins), and it re-engages only on the next explicit Follow toggle — not on a timer
and not silently — so the viewport never moves out from under the DJ unexpectedly.
This is the least-surprising policy and avoids a fight between the user's hand and
the auto-scroller. A future Epic that adds the DAW transport may revisit auto
re-engagement once there is a transport playhead to follow.

### 1.5.3. Now-Line vs Future Transport-Playhead Distinction

A reader familiar with DAWs may expect the vertical line to be a transport
cursor: scrubbable, seekable, the thing Play starts from. In this Epic it is not.

**Resolution:** The line this PRD draws is a **live now-line**: it mirrors the
live projection's leading edge / master-grid position and is strictly read-only.
It cannot be dragged, clicked-to-seek, or used as a play-start marker, because no
DAW transport exists yet (Record is EPIC-0009; Play/scrub is EPIC-0010). The
`Playhead` atom is deliberately named and built so a future transport playhead can
reuse or extend it, but this PRD ships only the live, non-interactive now-line and
states the distinction explicitly so neither code nor users conflate the two.

### 1.5.4. Scroll and Zoom Limits

Unbounded zoom (down to sub-sample or up to thousands of bars per screen) and
unbounded scroll (far past any content) produce a useless or visually broken view.

**Resolution:** `TimelineTransform` (PRD-0065) already defines a min/max
bars-per-screen range; this PRD reuses those limits and saturates at them rather
than introducing its own. Horizontal scroll is clamped to `[0, rightExtent]`
where `rightExtent` is derived from the furthest live-projection content plus a
small trailing margin (a configurable number of bars so the DJ can see a little
empty grid ahead of the now-line). Zoom and scroll never wrap; reaching a limit is
a hard, silent saturation. Concrete numeric limits live in `TimelineTransform`'s
constants so they are tuned in one place.

### 1.5.5. Vertical Overflow Handling (Scrollbar vs Collapse) and PRD-0067 Coordination

When decks × lanes exceed the panel height, the overflow can be handled by a
vertical scrollbar, by collapsing channel groups (PRD-0067's lane-collapse
affordance), or both — and it is ambiguous which slice owns the vertical layout.

**Resolution:** PRD-0067 remains the single authority for channel-group/lane
vertical layout and collapse. This PRD adds only a vertical **scrollbar/viewport**
over whatever stacked height PRD-0067 reports; it does not re-implement collapse
or override lane sizing. The two compose cleanly: collapse (PRD-0067) reduces the
content height, and the vertical scroll (this PRD) pans within whatever height
remains. The now-line rendering reads the composed visible-lane extent so it spans
correctly regardless of which mechanism changed the layout. If the content fits,
the vertical scrollbar is hidden.

### 1.5.6. Whether Clicking the Ruler Seeks Anything

In a full DAW, clicking the ruler moves the transport playhead. Here there is no
transport, so the behaviour of a ruler click is undefined.

**Resolution:** Clicking the ruler **seeks nothing** and moves nothing. There is
no transport cursor to position (§1.5.3) and the now-line is driven solely by the
live projection, so a click must not relocate it. The ruler click is **inert** by
default; an optional, non-blocking enhancement is to surface an informational
read-out of the musical time (bar/beat) at the clicked x, which is purely
presentational and writes no state. Making the click inert now prevents users from
forming a "click-to-seek" expectation that EPIC-0010's transport would then have
to honour or break.

### 1.5.7. Keyboard Shortcuts Deferred

DAWs commonly bind zoom/scroll/follow to keys (e.g. `+`/`-` to zoom, arrows to
scroll, a key to toggle follow). Wiring these now would prematurely commit a
global shortcut scheme.

**Resolution:** Keyboard shortcuts for zoom, scroll, and Follow are **deferred**.
This PRD ships pointer-, scrollbar-, and toggle-driven interaction only. A later
Epic that introduces the DAW transport and a fuller editing surface is the right
place to design a coherent, conflict-checked global keyboard map (including these
navigation shortcuts) rather than accreting one key at a time here.
