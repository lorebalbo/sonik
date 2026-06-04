---
status: Implemented
epic: EPIC-0011
depends-on:
  - PRD-0065
  - PRD-0067
  - PRD-0087
  - PRD-0092
---

# 1. PRD-0093: Automation Lane UI & Rendering

## 1.1. Problem

EPIC-0011 captures, stores, and replays performance gestures as automation: the
filter sweeps, EQ moves, gain rides, tempo changes, and mode toggles that make a
mix feel alive. By the time this PRD is reached, the automation data model exists
(PRD-0087): the `daw` ValueTree holds continuous lanes (filter, high, mid, low,
gain per channel; tempo on the master) as ordered breakpoints, and boolean lanes
(key-lock, pitch-stretch, key-stepper per channel) as ordered step events. The
playback applier exists too (PRD-0092): during timeline playback it evaluates each
enabled lane at the playhead and writes the resulting value back through the
single-source-of-truth paths the live controls use.

But none of this is visible. A DJ who recorded a slow filter sweep on Deck A and a
master-tempo ride has automation data sitting in the tree with no on-screen
representation. They cannot see the curve they drew, cannot tell which lanes hold
data, cannot tell whether a lane is currently driving the mix or has been bypassed,
and have nowhere to later edit a breakpoint (PRD-0094, which paints onto the
surface this PRD provides). The DAW panel (EPIC-0008) renders per-deck channel
groups with three source lanes (PRD-0067) and clip blocks (PRD-0068), but it has no
concept of an automation lane beneath a group, no concept of a master-tempo lane,
and no controls to show, hide, or bypass automation.

What is missing is the **view layer for automation**: a set of organisms under
`Source/Features/Daw/Automation/Ui/` that observe the PRD-0087 model via JUCE
Listeners, render continuous breakpoint curves and boolean step events horizontally
positioned by the PRD-0065 `TimelineTransform`, and expose per-lane show/hide and
per-lane bypass controls — all strictly `DESIGN.md`-compliant: monochrome curves
expressed through tonal layering and dithering rather than colour, `2px` borders,
zero corner radius, `Space Mono` labels, and pixel-perfect alignment to the same
time axis the ruler and clip lanes use. This PRD builds rendering and the
show/hide/bypass controls only. Editing breakpoints — drag, add, delete, redraw,
interpolation change — is explicitly PRD-0094.

## 1.2. Objective

The system renders automation lanes inside the DAW panel and exposes show/hide and
per-lane bypass such that:

- For each per-channel continuous parameter (`filter`, `high`, `mid`, `low`,
  `gain`) that has a lane node in the PRD-0087 model, an **automation lane organism**
  can be rendered beneath the corresponding channel group (PRD-0067), aligned to the
  same horizontal time axis via the PRD-0065 `TimelineTransform`.
- For the master tempo, a single **master automation lane** can be rendered beneath
  the master region, sharing the identical time axis.
- For each per-channel boolean parameter (`key-lock`, `pitch-stretch`,
  `key-stepper`) that has a lane node, a **boolean automation lane organism** renders
  its step events as monochrome step blocks.
- Continuous lanes render as a polyline connecting ordered breakpoints, with a
  breakpoint marker (a small filled square dot) at each breakpoint, in `#2d2d2d` on
  the `#fdfdfd` lane canvas, with a dithered fill beneath the curve to express
  "level" without colour or smooth gradient (§1.5.2). Segment interpolation (linear
  vs step/hold, owned by PRD-0087) is honoured by the polyline shape.
- Boolean lanes render as solid `#2d2d2d` step blocks spanning each "on" interval
  between step events, against the `#fdfdfd` lane canvas (§1.5.3).
- Each lane carries a left-hand **value axis** with `Space Mono` min/max labels and a
  `Space Mono` parameter label, sized per the `DESIGN.md` type scale (§1.5.6).
- Each lane exposes a **show/hide** affordance and a **per-lane bypass (enable)**
  toggle, both rendered as `DESIGN.md` buttons (`2px solid #2d2d2d`, zero radius,
  explicit active/inactive fill inversion); bypass reflects and drives the lane's
  enable flag in the PRD-0087 model, which PRD-0092 already honours.
- Lanes are **hidden by default** to reduce clutter; a per-group automation
  disclosure reveals them (§1.5.4). The set of visible lanes scrolls vertically when
  more lanes are shown than fit (§1.5.5).
- The view observes the PRD-0087 ValueTree (lanes, breakpoints, step events, enable
  flags) via JUCE Listeners (Observer pattern) and repaints reactively; it never
  polls the audio thread. It reflects the live applier value (PRD-0092) as a
  read-only playhead-position indicator on the lane (§1.5.7).
- No audio-thread code is added; all rendering is message/UI-thread and all
  cross-thread reads use the existing published snapshots.

## 1.3. User Flow

1. The DJ has recorded automation (filter sweep on Deck A, master-tempo ride). The
   PRD-0087 model now holds a `filter` continuous lane under Deck A's track node and
   a `tempo` lane under the master node, each with ordered breakpoints. By default no
   automation lane is visible — the DAW panel looks exactly as PRD-0067/PRD-0068 left
   it.
2. The DJ clicks the **automation disclosure** control on Deck A's channel group
   header. The group expands to reveal its automation lanes beneath its three source
   lanes (PRD-0067). The `filter` lane shows the recorded curve: a `#2d2d2d` polyline
   with square breakpoint dots, a dithered fill beneath it, and a left-hand value axis
   labelled in `Space Mono` (`FILTER`, min/max). Lanes for parameters with no recorded
   data render empty (flat baseline) but are still listed.
3. The DJ scrubs the timeline playhead. As the applier (PRD-0092) evaluates each lane,
   a thin vertical playhead indicator crosses every visible lane at the same x as the
   ruler and clip lanes (shared PRD-0065 transform), and a small read-only marker on
   each continuous lane tracks the current applied value at the playhead.
4. The DJ wants to A/B the mix with and without the recorded filter motion. They click
   the `filter` lane's **bypass** button. The button inverts fill (active → bypassed),
   the lane dims via tonal layering to signal it is inactive, and PRD-0092 stops
   applying that lane (the live filter control resumes manual authority). The curve
   stays visible for reference.
5. The DJ reveals the master region's automation and sees the **tempo** lane with its
   recorded BPM ride as a continuous curve. Its value axis shows the BPM min/max in
   `Space Mono`.
6. The DJ shows several lanes across multiple decks until they exceed the panel body
   height. The automation region scrolls vertically (§1.5.5); group/lane order remains
   stable. The DJ hides Deck A's automation again via the disclosure; the lanes
   collapse and the reclaimed vertical space returns to clip lanes.

## 1.4. Acceptance Criteria

- [ ] A new organism set under `Source/Features/Daw/Automation/Ui/` renders
  continuous automation lanes (one per `filter`/`high`/`mid`/`low`/`gain` lane node
  present in the PRD-0087 model under a channel group) and the single master `tempo`
  lane, each horizontally aligned to the PRD-0065 `TimelineTransform` such that a
  breakpoint at timeline sample `S` lands at the same x as the PRD-0066 ruler tick and
  any PRD-0068 clip at `S`.
- [ ] A boolean automation lane organism renders `key-lock`, `pitch-stretch`, and
  `key-stepper` lane nodes (per channel) as solid `#2d2d2d` step blocks spanning each
  "on" interval, against the `#fdfdfd` canvas.
- [ ] Continuous lanes render as a `#2d2d2d` polyline through ordered breakpoints with
  a filled square breakpoint marker at each breakpoint and a dithered fill beneath the
  curve; no colour, no smooth gradient, no anti-aliased blur is used to express level.
- [ ] Per-segment interpolation (linear vs step/hold) from the PRD-0087 model is
  honoured by the rendered polyline shape (step segments draw as horizontal-then-
  vertical, linear segments draw as a straight diagonal).
- [ ] Each lane renders a left-hand value axis with `Space Mono` min/max labels and a
  `Space Mono` all-caps parameter label, per the `DESIGN.md` type scale; labels are
  `#2d2d2d` on the lane's tonal-layer background.
- [ ] Each lane exposes a per-lane **bypass (enable)** toggle rendered as a
  `DESIGN.md` button (`2px solid #2d2d2d`, zero radius, active/inactive fill
  inversion). Toggling it reads/writes the lane's enable flag in the PRD-0087 model;
  PRD-0092 honours that flag (verified by the applier's existing contract, not
  re-implemented here).
- [ ] A bypassed lane dims via tonal layering (not colour) while its curve/steps
  remain visible for reference.
- [ ] Automation lanes are **hidden by default**. A per-group (and master) automation
  disclosure control reveals/collapses the group's automation lanes; revealing does
  not disturb the source-lane (PRD-0067) or clip (PRD-0068) layout above it.
- [ ] When the total height of all shown automation lanes exceeds the available
  panel body height, the automation region scrolls vertically; lane and group order
  is stable across show/hide and across deck load/eject churn.
- [ ] The view observes the PRD-0087 ValueTree (lane add/remove, breakpoint/step
  add/move/remove, enable-flag change) via JUCE Listeners and repaints reactively;
  no polling of deck or audio-thread state occurs.
- [ ] A read-only playhead-position indicator crosses every visible lane at the same
  x as the ruler/clip playhead (shared PRD-0065 transform), and each continuous lane
  shows a read-only marker tracking the PRD-0092 applied value at the playhead.
- [ ] No editing interaction is implemented: clicking, dragging, or double-clicking a
  breakpoint, a curve segment, or a step event performs no model mutation in this PRD
  (those are PRD-0094). The only writes this PRD performs are the per-lane enable flag
  (bypass) and per-group view-state (show/hide, scroll), the latter held in view state
  or a UI-only ValueTree branch, never on a captured-automation node.
- [ ] No audio-thread code is added or modified; all rendering and observation run on
  the message/UI thread, perform no allocation in any audio path, take no locks shared
  with the audio thread, and read cross-thread values only via existing published
  snapshots (PRD-0092 applied value, PRD-0069/master playhead).
- [ ] All visuals conform to `DESIGN.md`: strict `#2d2d2d` / `#fdfdfd` monochrome,
  `Space Mono` labels, `2px` borders, zero corner radius, dithered fills/step shading,
  and pixel-grid-aligned curves (a blurry curve is a defect).

## 1.5. Grey Areas

### 1.5.1. Lane Vertical Placement and Space Competition with Clip Lanes

Automation lanes must live *somewhere* vertically, and PRD-0067's three source lanes
plus PRD-0068's clips already consume the channel group's height. Options: (a) place
automation lanes beneath each channel group's three source lanes, expanding the
group's total height when revealed; (b) overlay automation translucently on top of
the clip lanes; (c) a separate global automation pane below all groups.

**Resolution:** Option (a) — automation lanes are collapsible rows rendered *beneath*
their owning channel group's three source lanes, and the master tempo lane is rendered
beneath the master region. Revealing them increases the group's vertical extent and
pushes lower groups down within the scrollable body; collapsing reclaims the space for
clip lanes. Overlay (b) violates `DESIGN.md` (no translucency / simulated depth, and it
would collide with clip dithering, destroying legibility). A separate global pane (c)
breaks the spatial "this automation belongs to this deck" relationship EPIC-0011 §1.1
relies on. Placement beneath the owning group keeps automation spatially adjacent to
the parameter it drives and reuses PRD-0067's existing scroll/collapse machinery.

### 1.5.2. Continuous Curve Rendering Style

A continuous curve could be drawn as just a line, a line with breakpoint markers, a
filled region, or some combination. `DESIGN.md` forbids smooth gradients and colour.

**Resolution:** Render the curve as a `1px`/`2px` `#2d2d2d` polyline through the
ordered breakpoints, a small filled `#2d2d2d` square dot at each breakpoint (so the
discrete breakpoints PRD-0094 will edit are visible), and a **dithered fill** of
`#2d2d2d` beneath the line whose checkerboard density is constant (a single tonal
layer), not value-proportional — the line height already encodes value, so a
value-proportional dither would double-encode and read as noise. The dithered fill
exists only to give the lane visual body and separate "below the curve" from "above
the curve" monochromatically, consistent with the waveform 1-bit treatment in
`DESIGN.md`. Linear segments draw diagonally; step/hold segments draw as a horizontal
run to the next breakpoint's x then a vertical jump.

### 1.5.3. Boolean Lane Visual

A boolean (on/off) lane could render as a square wave line, as filled step blocks, or
as discrete event markers. It must remain unambiguous in strict monochrome.

**Resolution:** Filled step blocks. Each "on" interval (from a true step event to the
next false event, or to the lane's right edge) renders as a solid `#2d2d2d` block
spanning that horizontal interval, leaving "off" intervals as bare `#fdfdfd` canvas.
This is the most legible monochrome encoding: a solid presence/absence reads instantly
where a thin square-wave line would be lost against the lane border, and discrete
markers would fail to show *duration*. The block's top/bottom are inset from the lane
border so the lane's `2px` frame stays visible. The key-stepper lane records its
engaged/stepped transitions as on/off per PRD-0087, so the same block treatment
applies.

### 1.5.4. Show/Hide Default

Should automation lanes be visible by default, or hidden until disclosed?

**Resolution:** Hidden by default. The DAW's primary surface is structure (clips) and
the live mix; surfacing every automatable parameter's lane for every deck at all times
would flood the panel and bury the clips, violating the `DESIGN.md` "editorial,
restrained" intent. A per-group automation disclosure (an all-caps `Space Mono`
`AUTO` toggle on the group header, rendered as a `DESIGN.md` button) reveals the
group's automation lanes on demand. Lanes that contain recorded data are listed first
when revealed so the DJ sees populated lanes without scrolling; empty lanes follow.
The disclosure state is view-only (UI ValueTree branch or component state), never
written to a captured-automation node, so toggling visibility never dirties the
arrangement.

### 1.5.5. How Many Lanes Visible at Once

Revealing every parameter across four decks is up to `4 decks × 8 lanes + 1 master`
lanes — far more than fit. The view needs a bounded, scrollable presentation.

**Resolution:** Reuse PRD-0067's vertical scroll for the whole channel-group stack;
revealed automation lanes participate in that same scroll rather than introducing a
second nested scrollbar (nested scroll is a `DESIGN.md` and usability anti-pattern).
Each automation lane has a fixed, modest height (a tonal-layer band shorter than a
source lane) so several can be shown without dominating. There is no hard cap on how
many lanes may be revealed; the single outer scroll handles overflow. If profiling
later shows repaint cost from many simultaneously visible curves, an off-screen-lane
paint cull (skip painting lanes outside the viewport) is the optimisation, not an
artificial visible-lane limit.

### 1.5.6. Value Axis Labels

Each lane needs to communicate its value range. Options: no axis (just the curve),
min/max end labels, or full tick-marked axis.

**Resolution:** A minimal left-hand value axis with two `Space Mono` labels — the
lane's min at the bottom and max at the top — plus the all-caps `Space Mono` parameter
label (`FILTER`, `HIGH`, `GAIN`, `TEMPO`, etc.). Units follow the underlying
parameter's existing definition (normalised `0`–`1` for filter/EQ/gain unless the
parameter already exposes a unit; BPM for tempo). A full tick-marked axis is overkill
for the at-a-glance reading automation lanes need and would crowd the narrow lane
height; two end labels plus the parameter name give enough orientation, and PRD-0094's
editing affordances can surface a precise value readout on hover/drag when editing is
introduced.

### 1.5.7. Per-Lane Bypass Visual and Live-Value Reflection

Bypass must be unmistakable in monochrome (no "greyed out" colour), and the lane
should reflect the live applied value without implying it is editable yet.

**Resolution:** The bypass control is a `DESIGN.md` button on the lane's value-axis
gutter with explicit fill inversion: enabled = filled `#2d2d2d` glyph on `#fdfdfd`;
bypassed = inverted. When bypassed, the entire lane body shifts one tonal layer (a
denser dither wash over the canvas) so it reads as inactive at a glance while keeping
the curve/steps legible for reference. The live applied value (PRD-0092) is shown only
as a small read-only filled marker riding the curve at the playhead x — visually
distinct from the editable breakpoint dots (e.g. an open/hollow marker vs the filled
breakpoint squares) so the DJ never mistakes the live cursor for an editable point.
When a lane is bypassed, the live marker is suppressed (the applier is not driving it),
making the bypass state doubly clear.
