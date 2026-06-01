---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0063
  - PRD-0064
---

# 1. PRD-0065: Timeline Coordinate Transform

## 1.1. Problem

EPIC-0008 builds a docked DAW timeline whose ruler, grid lines, clip blocks, and
live playhead must all agree on a single answer to one deceptively simple
question: "given a sample position in the source/timeline, where on screen does
it land — and given a horizontal pixel, what sample does the user mean?" Every
visual element in the timeline depends on this mapping. The ruler (PRD-0066)
needs to know which bars and beats are visible and where to draw their ticks.
The grid overlay needs to place vertical lines at beat boundaries. Clip blocks
(PRD-0067/PRD-0068) need to convert their `[timelineStartSample, …]` extent into
an on-screen rectangle. The live playhead (PRD-0069) needs to convert the
deck's published source position into an x-coordinate every UI frame. Without a
single, shared, well-tested transform, each of these consumers would re-derive
the math independently, drift out of agreement (the playhead would land one
pixel off the grid line it sits on), and make zoom/scroll feel inconsistent.

PRD-0064's `MasterGridService` already answers the *musical* half of the
problem: it converts samples ↔ beats/bars against the authoritative master
tempo and phase origin (read-only from the `MasterClockSnapshot`). What is still
missing is the *view* half: a horizontal zoom level (how much musical time fits
on screen) and a horizontal scroll offset (what timeline position sits at the
left edge), composed with the musical conversion to produce final pixel
coordinates. That composition — samples ↔ beats ↔ pixels, plus the inverse —
must be a single pure value type so that all timeline rendering is provably
consistent, and so that zoom and scroll behave correctly (in particular, zoom
must keep the sample under the cursor fixed, which requires the transform to
expose a focal-point zoom operation rather than leaving each caller to guess).

The transform must also be pure UI-thread value math with no JUCE `Component`
dependency, so it can be unit-tested exhaustively in isolation (round-trip
stability, clamping, focal-point invariants) without instantiating any UI.

## 1.2. Objective

Introduce `Source/Features/Daw/Transform/TimelineTransform.{h,cpp}`: a pure,
message/UI-thread coordinate-transform value type that composes PRD-0064's
sample↔beat conversion with a horizontal view transform (zoom + scroll) to map
between three coordinate spaces — **samples** (timeline sample position),
**musical time** (beats/bars), and **pixels** (x within the timeline viewport).
Specifically:

- The transform holds a **view state**: a horizontal zoom expressed as a single
  canonical musical unit (`pixelsPerBeat`, see §1.5.1) and a horizontal scroll
  offset expressed as the **timeline sample at the left edge** of the viewport.
- It exposes **forward mappings**: `sampleToX(int64 sample) -> double`,
  `beatToX(double beat) -> double`, and a convenience `barToX`.
- It exposes **inverse mappings**: `xToSample(double px) -> int64` and
  `xToBeat(double px) -> double`, the exact inverses of the forward mappings
  within rounding tolerance.
- All mappings **clamp** their inputs/outputs to documented bounds (§1.5.4) so a
  caller can never produce a NaN, an out-of-range sample, or an x outside the
  representable range.
- It exposes a **focal-point zoom** operation `zoomAroundX(double focusPx,
  double zoomFactor)` that adjusts `pixelsPerBeat` by `zoomFactor` while keeping
  the sample currently under `focusPx` fixed at `focusPx` (§1.5.5).
- It exposes a **snapping helper** `snapSampleToGrid(int64 sample) ->
  int64` returning the nearest grid-line (beat) sample, for later use by
  editing/recording PRDs (it performs no UI work itself).
- It exposes a **scroll setter** `setLeftEdgeSample(int64)` and a `scrollByX`
  helper, both clamped to the scroll bounds of §1.5.4.
- Zoom is clamped to documented **min/max limits** (§1.5.2) so the user can
  neither zoom out past a useful overview nor zoom in past sub-sample pixels.
- The type is a **pure value object**: it takes a `MasterGridService` snapshot
  (or the minimal `samplesPerBeat` + grid origin it needs) plus a viewport
  width, owns no JUCE `Component`, allocates nothing on a hot path, and is fully
  unit-testable in isolation.
- A new test file `Tests/TimelineTransformTests.cpp` verifies round-trip
  stability, clamping, the focal-point zoom invariant, snapping correctness, and
  the tempo-independent sample→pixel path (§1.5.7).

This PRD delivers only the math. It does **not** render anything, own any
`Component`, or handle any mouse/keyboard event — those are PRD-0066 (panel +
ruler), PRD-0067 (lane layout), and PRD-0070 (interaction/polish), all of which
consume this transform.

## 1.3. Developer / Integration Flow

1. `TimelineTransform` is constructed from three inputs: (a) the musical
   conversion source — a `MasterGridService` reference or a lightweight
   immutable snapshot exposing `samplesPerBeat()` and the grid-origin sample
   (PRD-0064); (b) the current view state (`pixelsPerBeat`, `leftEdgeSample`);
   (c) the viewport width in pixels. It stores these by value and exposes the
   mappings below. It re-reads the grid snapshot whenever the master tempo
   changes (the owning component refreshes the transform; the transform itself
   does not subscribe to anything).
2. Forward sample→pixel is computed as a pure sample-space affine map:
   `x = (sample - leftEdgeSample) * pixelsPerBeat / samplesPerBeat`. This path is
   **independent of beats** as an intermediate (it uses `samplesPerBeat` only as
   a scale factor), which guarantees clips that are not beat-aligned still map
   correctly (§1.5.7). `beatToX` is `sampleToX(gridService.beatToSample(beat))`,
   composing PRD-0064's musical conversion with the affine view map.
3. Inverse pixel→sample is the algebraic inverse:
   `sample = leftEdgeSample + round(px * samplesPerBeat / pixelsPerBeat)`,
   rounded to the nearest integer sample (§1.5.6) and clamped to the scroll/
   content bounds (§1.5.4). `xToBeat` is `gridService.sampleToBeat(xToSample(px))`.
4. `zoomAroundX(focusPx, zoomFactor)` records the sample currently under
   `focusPx` (`anchor = xToSample(focusPx)`), multiplies `pixelsPerBeat` by
   `zoomFactor`, clamps the result to `[minPixelsPerBeat, maxPixelsPerBeat]`
   (§1.5.2), then solves for the new `leftEdgeSample` such that
   `sampleToX(anchor) == focusPx` again, and clamps that to the scroll bounds.
   When the clamp on `pixelsPerBeat` is hit, the anchor invariant is preserved
   for the clamped zoom level (the sample under the cursor stays put even though
   the requested zoom was limited).
5. `snapSampleToGrid(sample)` computes the nearest beat boundary by rounding
   `gridService.sampleToBeat(sample)` to the nearest whole beat and converting
   back to a sample via `gridService.beatToSample`. It returns an integer sample
   and is used later by recording (EPIC-0009) and editing (EPIC-0010); this PRD
   only provides and tests the helper.
6. Consumers use the transform read-only: PRD-0066's ruler calls `beatToX`/
   `barToX` for every visible bar/beat tick and `xToBeat` for hit-testing;
   PRD-0067/PRD-0068's clip blocks call `sampleToX` on their start/end samples
   to compute their on-screen rectangle; PRD-0069's playhead calls `sampleToX`
   on the deck's published source position once per UI frame. None of them
   re-derive the math.
7. `Tests/TimelineTransformTests.cpp` constructs a transform from a synthetic
   grid snapshot (fixed `samplesPerBeat`, fixed origin, fixed viewport width)
   and asserts the invariants in §1.4 with no JUCE `Component` instantiated.

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Daw/Transform/TimelineTransform.{h,cpp}` exists, lives in
  the `Daw` feature slice, and `#include`s only public contracts (PRD-0064's
  `MasterGridService` public header, standard library, `juce_core` for
  `int64`/`jlimit` only) — no JUCE `Component`, no GUI module, no reach into any
  slice's `internal/`.
- [ ] The transform exposes forward mappings `sampleToX(int64) -> double`,
  `beatToX(double) -> double`, and `barToX(double) -> double`, and inverse
  mappings `xToSample(double) -> int64` and `xToBeat(double) -> double`.
- [ ] Round-trip stability: for any in-range pixel `px`,
  `sampleToX(xToSample(px))` equals `px` within `±0.5` px, and for any in-range
  sample `s`, `xToSample(sampleToX(s))` equals `s` exactly (integer sample
  round-trip is lossless within the representable zoom range).
- [ ] The canonical zoom unit is `pixelsPerBeat` (a positive `double`); any
  bars-per-screen value supplied by the UI is converted to `pixelsPerBeat` at
  the boundary (§1.5.1), and the transform stores and reasons in
  `pixelsPerBeat` exclusively.
- [ ] Zoom is clamped to `[minPixelsPerBeat, maxPixelsPerBeat]` (§1.5.2); any
  setter or `zoomAroundX` call that would exceed the bounds clamps to the limit
  and never produces a non-positive or non-finite `pixelsPerBeat`.
- [ ] Horizontal scroll is stored as `leftEdgeSample` (an `int64`) and clamped
  to `[minLeftEdgeSample, maxLeftEdgeSample]` (§1.5.4); `setLeftEdgeSample` and
  `scrollByX` both apply the clamp.
- [ ] Focal-point zoom invariant: after `zoomAroundX(focusPx, factor)`, the
  sample that was under `focusPx` before the call is under `focusPx` after the
  call, within `±1` sample (or within `±0.5` px when the zoom clamp is hit),
  verified by a unit test sweeping several focus points and zoom factors.
- [ ] All mappings are total and safe: no input produces a NaN, an infinity, a
  non-finite x, or an out-of-range sample; inverse mappings clamp their result
  to the content/scroll bounds (§1.5.4).
- [ ] `snapSampleToGrid(int64) -> int64` returns the nearest beat-boundary
  sample (round-half-to-even or round-half-up, fixed by §1.5.6) computed via
  PRD-0064's `MasterGridService`, and is verified for samples on a beat, just
  before a beat, just after a beat, and exactly halfway between two beats.
- [ ] The sample→pixel forward path is tempo-independent in the sense of §1.5.7:
  given a fixed `samplesPerBeat` and view state, `sampleToX` is a pure affine
  function of the sample that does not route through a rounded beat value, so a
  clip whose start sample is not on a beat boundary maps to a fractional,
  correct x (verified by a test using a deliberately off-beat sample).
- [ ] Pixel alignment to the `DESIGN.md` strict pixel grid is handled by an
  explicit, documented rounding policy (§1.5.3): the transform returns
  sub-pixel `double` x-values, and a single documented helper
  `alignToPixelGrid(double x) -> double` (or `int`) rounds to the device pixel
  for renderers that require crisp 1-px lines; renderers that anti-alias may use
  the raw `double`. The transform never silently rounds inside the forward
  mapping.
- [ ] `Tests/TimelineTransformTests.cpp` exists, is registered in the test
  runner / CMake test target, and covers: round-trip stability, zoom clamping,
  scroll clamping, the focal-point zoom invariant, snapping correctness, the
  off-beat sample→pixel path, and the degenerate cases (zero viewport width,
  `pixelsPerBeat` at each limit).
- [ ] No audio-thread code is added by this PRD. The transform runs only on the
  message/UI thread, reads PRD-0064's already-published grid snapshot, performs
  no allocation on any mapping call, takes no locks, and performs no I/O. It does
  not touch any `processBlock` path.
- [ ] No JUCE `Component`, no rendering, no mouse/keyboard handling, and no
  `ValueTree` mutation is added by this PRD; the transform is a pure value type
  consumed by later PRDs (PRD-0066, PRD-0067, PRD-0069, PRD-0070).

## 1.5. Grey Areas

### 1.5.1. Zoom Unit: Bars-Per-Screen vs Pixels-Per-Beat

The Epic (§1.2.1) describes horizontal zoom as "musical: bars-per-screen," but
the transform internally needs a linear scale factor between sample-space and
pixel-space. Storing bars-per-screen forces a division by the (variable)
viewport width on every mapping and makes the scale depend on two quantities
(bars-per-screen and width) rather than one.

**Resolution:** The canonical stored unit is `pixelsPerBeat` (a single positive
`double`). Bars-per-screen is a *presentation* unit used at the UI boundary: a
zoom control that thinks in "N bars on screen" converts once to
`pixelsPerBeat = viewportWidth / (N * beatsPerBar)` before handing the value to
the transform, and the transform converts back for display via the inverse.
Keeping `pixelsPerBeat` canonical makes the affine map a single multiply, makes
the math independent of viewport width except where the width genuinely matters
(scroll clamping, focal-point zoom), and avoids re-deriving the scale on every
mapping call. This is the one canonical unit; no code path stores
bars-per-screen as state.

### 1.5.2. Minimum and Maximum Zoom Limits

Without limits the user could zoom out until the whole set is one pixel wide
(unreadable) or zoom in until one beat spans the screen at sub-sample precision
(meaningless and numerically fragile).

**Resolution:** Clamp `pixelsPerBeat` to a documented `[minPixelsPerBeat,
maxPixelsPerBeat]`. The minimum corresponds to a sensible overview (on the order
of a few pixels per beat — enough to see the whole arrangement structure); the
maximum corresponds to roughly one beat filling a large fraction of the viewport
(enough to place a clip edge precisely, but never finer than ~1 sample per pixel
at the maximum supported sample rate, to keep `samplesPerBeat / pixelsPerBeat`
≥ 1 and inverse rounding stable). Exact constants are tuned during
implementation against `DESIGN.md` readability and the 48 kHz/96 kHz sample
rates, exposed as named constants in the header, and asserted by the clamp
tests. The limits are deliberately conservative; later interaction polish
(PRD-0070) may expose user-facing zoom presets within these bounds.

### 1.5.3. Sub-Pixel Rounding and Alignment to the Strict Pixel Grid

`DESIGN.md` mandates crisp 2-px borders, zero border-radius, and a strict pixel
aesthetic, which argues for integer pixel positions; but the transform's natural
output is a fractional `double`, and rounding inside the forward mapping would
break the round-trip invariant and the focal-point zoom invariant.

**Resolution:** The forward mappings return sub-pixel `double` x-values and never
round internally. A single documented helper, `alignToPixelGrid(double x)`,
performs device-pixel rounding (respecting the display scale factor) and is
called *by the renderer* at draw time for elements that need crisp 1-/2-px lines
(grid lines, ruler ticks, clip borders, playhead). Renderers that anti-alias
(e.g. waveform fills) may use the raw `double`. This keeps the math pure and
invertible while satisfying `DESIGN.md` at the only layer that can correctly
account for the display scale — the renderer. The transform owns the *policy*
(the helper) but applies it only where a caller explicitly asks.

### 1.5.4. Scroll Clamping Bounds: Before Bar 0 and Past Content End

The DJ might try to scroll left of the timeline origin (negative time) or right
past the end of all content. Allowing unbounded scroll lets the viewport show
only empty space and disorients the user; clamping too tightly prevents a small
margin that makes the first/last clip edge comfortable to grab.

**Resolution:** `leftEdgeSample` is clamped to `[minLeftEdgeSample,
maxLeftEdgeSample]`. `minLeftEdgeSample` is a small **negative** margin (a
configurable number of beats before bar 0, default a fraction of a bar) so the
origin/bar-0 line is not jammed against the left edge but the user cannot scroll
into a large void. `maxLeftEdgeSample` is `contentEndSample` minus a margin (a
fraction of the viewport) so the last content remains visible with a little room
past it, but the user cannot scroll into an arbitrarily large empty region. The
content-end sample is supplied by the owning component (it knows the arrangement
extent); the transform treats it as an input and does not compute it. When
content is shorter than the viewport, the bounds collapse so the content stays
anchored at the left with bar 0 visible. The exact margins are named constants,
tuned during implementation, and covered by the scroll-clamp tests.

### 1.5.5. Focal-Point Zoom Anchor

Zooming with a scroll-wheel or pinch should keep the musical position under the
cursor fixed (the standard DAW behaviour); zooming via a button or keyboard
shortcut has no cursor and needs a different anchor.

**Resolution:** `zoomAroundX(focusPx, factor)` is the primitive: it anchors on
an explicit pixel, used directly for cursor/pinch zoom. Button/keyboard zoom is
implemented by the caller (PRD-0070) by passing a chosen `focusPx` — typically
the viewport centre (`viewportWidth / 2`) or the current playhead x — to the
same primitive. The transform exposes only the explicit-focus operation; it does
not embed a policy about which anchor a keyboard shortcut should use, keeping the
math pure and pushing the UX choice to the interaction layer. The anchor
invariant (the sample under `focusPx` stays under `focusPx`) holds for every
caller regardless of how `focusPx` was chosen.

### 1.5.6. Integer Sample vs Double Precision for Transforms

Sample positions are integers (`int64`), but the affine map and its inverse
naturally produce fractional values, and accumulating rounding across many
zoom/scroll operations could drift.

**Resolution:** The **stored** view state uses exact types: `leftEdgeSample` is
an `int64` (no drift across scroll operations) and `pixelsPerBeat` is a `double`
(zoom is inherently continuous). Forward `sampleToX` returns a `double` (sub-pixel
position; the renderer aligns per §1.5.3). Inverse `xToSample` returns an `int64`
rounded **once** from the `double` computation using a single fixed rule
(round-half-up), so there is no per-operation accumulation — the inverse is
computed fresh from the stored exact state each time, never by integrating
deltas. `snapSampleToGrid` likewise rounds the beat value once. This confines all
rounding to the boundary between continuous (pixels/zoom) and discrete (samples)
spaces and guarantees the integer-sample round-trip in §1.4 is lossless within
the supported zoom range.

### 1.5.7. Mapping Clips That Are Not Beat-Aligned (Future Non-Matching-Tempo Clips)

A later Epic may place clips whose source tempo does not match the master grid,
or clips deliberately positioned off the beat grid (free placement). If the
sample→pixel path routed through a rounded beat value, such a clip's edges would
visibly snap to the nearest beat line, which is wrong.

**Resolution:** The forward sample→pixel path is a **pure affine function of the
sample** (§1.3 step 2): `x = (sample - leftEdgeSample) * pixelsPerBeat /
samplesPerBeat`, using `samplesPerBeat` only as a scalar, never routing through a
quantised beat index. This makes `sampleToX` correct for any sample, beat-aligned
or not, including future off-grid or non-matching-tempo clips, which simply use
the sample→pixel path directly and ignore `beatToX` entirely. The beat-based
mappings (`beatToX`, `xToBeat`, `snapSampleToGrid`) exist solely for the
*musical* consumers (ruler, grid, snap) and never gate the *positional* mapping
that clips use. A unit test places a clip start at a deliberately off-beat sample
and asserts its x is the exact fractional affine result, not a snapped beat
position.
