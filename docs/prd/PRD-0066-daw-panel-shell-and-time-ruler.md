---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0005
  - PRD-0064
  - PRD-0065
---

# 1. PRD-0066: DAW Panel Shell & Time Ruler

## 1.1. Problem

EPIC-0008 introduces an in-app, non-destructive arrangement DAW docked at the top of the application, above the decks and mixer. The foundational data layer for that DAW now exists in isolation: PRD-0064 publishes a read-only `MasterGridService` that derives bar/beat grid lines from the authoritative `MasterClockSnapshot` (EPIC-0003), and PRD-0065 provides the `TimelineTransform` that converts samples ↔ musical time ↔ screen pixels at a given zoom. But none of this is visible. There is no surface in the running application onto which the grid, the ruler, the lanes, the clips, and the playhead can eventually be drawn, and there is no place for the DJ to read musical time against the master tempo.

The existing main layout (PRD-0005) composes the deck UI shell and `MainContentComponent` from the decks and (eventually) the mixer, but it has no top-docked region reserved for the DAW. Before any lane content (PRD-0067), clip rendering (PRD-0068), live projection (PRD-0070), or interaction polish can land, the Epic needs the *container* itself: a `DawPanel` organism that docks at the top of the window, reflows the decks below it when it collapses or expands, and renders a readable time ruler showing bars and beats. Without this shell, every subsequent DAW PRD has nowhere to attach its UI, and the DJ has no way to see — or reserve vertical space for — the arrangement canvas.

Additionally, `DESIGN.md` contains no DAW- or timeline-specific component specification. It defines the global design language (strict monochrome `#2d2d2d` / `#fdfdfd`, `Space Mono Regular`, 2-px solid borders, zero border-radius, dithering instead of gradients, tonal layering for depth, the "no-line" rule of separating modules by surface shifts rather than dividers) but says nothing about a ruler, bar labels, or beat ticks. The ruler's visual language must therefore be *derived* from the generic `DESIGN.md` rules, and that derivation must be documented so later PRDs (and any DESIGN.md amendment) stay consistent.

## 1.2. Objective

The system provides a docked, collapsible DAW panel and a readable musical time ruler such that:

- A `DawPanel` organism (`Source/Features/Daw/Ui/Organisms/DawPanel.h/.cpp`) is docked at the **top** of the application window, above the decks and mixer, occupying the full window width.
- The panel is **collapsible and expandable** via a single, always-visible toggle control. Collapse and expand are **instant** (no animation, per `DESIGN.md`), and the decks/mixer below reflow to reclaim or yield the vertical space immediately.
- When expanded, the panel renders a **time ruler** (`Source/Features/Daw/Ui/Molecules/TimeRuler.*`) composed of `RulerTick` atoms (`Source/Features/Daw/Ui/Atoms/RulerTick.*`) showing bars and beats in musical time.
- The ruler's grid is driven by `MasterGridService` (PRD-0064) and positioned via `TimelineTransform` (PRD-0065): bar and beat tick x-positions are computed by the transform from the master grid, not invented by the panel.
- Bar numbers are rendered in `Space Mono Regular` at the **top of the ruler**; bar ticks and beat ticks are distinguished by **tonal layering** (surface/length contrast), never by colour.
- The ruler renders at a **fixed, readable default zoom** in this PRD and exposes the `TimelineTransform` it uses so later PRDs can layer lanes, clips, and the playhead onto the same coordinate space. Zoom and scroll interaction handlers are out of scope (PRD-0070).
- The panel integrates into the existing main layout (PRD-0005 `MainContentComponent`) as a top dock region; the decks/mixer are laid out in the remaining vertical space below it.
- The entire shell is fully `DESIGN.md`-compliant: 2-px solid `#2d2d2d` borders where borders are used, zero border-radius, tonal `surface-container-*` tones for depth and module separation (the "no-line" rule), an optional dithered shadow (never a blurred/gradient one), and strict pixel-grid alignment of every tick.

## 1.3. Developer / Integration Flow

1. A new `DawPanel` organism is created under `Source/Features/Daw/Ui/Organisms/`. It owns the panel chrome (background surface, header, collapse toggle) and, when expanded, a `TimeRuler` molecule. It holds a reference to a `TimelineTransform` (PRD-0065) and a `MasterGridService` (PRD-0064), both injected via the constructor (no singletons, per `CLAUDE.md`).

2. A `TimeRuler` molecule is created under `Source/Features/Daw/Ui/Molecules/`. Given the transform and the grid service, it asks the grid service for the bar/beat lines spanning the currently visible sample range, maps each to an x-pixel via the transform, and lays out one `RulerTick` atom per visible tick. Bar lines also carry a `Space Mono` numeric label.

3. A `RulerTick` atom is created under `Source/Features/Daw/Ui/Atoms/`. It is a pure paint component drawing a single vertical tick. It exposes a "bar vs beat" kind and renders the two kinds with different tick heights and tonal weight (longer/heavier tick for bars, shorter/lighter for beats), with the optional bar-number label drawn above. It performs no layout logic of its own.

4. The `DawPanel` exposes a `bool isExpanded()` / `void setExpanded(bool)` pair and a collapse-toggle button atom (reusing the existing DESIGN-compliant button styling: `2px solid #2d2d2d`, inverted fill for active/inactive). Toggling flips an internal expanded flag, calls `resized()` on itself, and notifies its parent (the main layout) that its preferred height changed so the decks below reflow.

5. The main layout (PRD-0005 `MainContentComponent`) is amended to add the `DawPanel` as a top-docked child. In `resized()`, the layout reserves the panel's current preferred height (expanded height when expanded, collapsed header height when collapsed) at the top, then lays out the decks/mixer in the remaining bounds below. No fixed window-height assumption is broken: the decks simply receive less vertical space when the panel is expanded.

6. The panel's preferred height is a simple two-state value: a small fixed `collapsedHeight` (just the header / toggle strip) and a fixed `expandedHeight` (header strip + ruler height). The ruler height is sized to comfortably fit one row of `Space Mono` bar labels plus the tallest bar tick at the default zoom.

7. The default zoom (bars-per-screen) is a fixed constant configured on the `TimelineTransform` at construction in this PRD. The visible sample range is derived from the master grid's phase origin and the project sample rate (PRD-0064), starting the ruler at bar 1. No scroll offset is applied (scroll is PRD-0070).

8. The panel observes the relevant `ValueTree`/grid state via JUCE Listeners (Observer pattern) so that a master-tempo change (which moves bar/beat positions) triggers a `repaint()` of the ruler without polling the audio thread. All reads of master state go through the existing `MasterGridService` snapshot accessor; no new audio-thread code is introduced.

9. A new test file under `Tests/` (`DawPanelShellTests.cpp` or similar) verifies: collapse/expand toggles the preferred height between the two fixed values; the ruler requests the expected number of bar/beat ticks for a known BPM + sample-rate + default-zoom combination; bar ticks and beat ticks are assigned distinct tonal/height kinds; and tick x-positions match `TimelineTransform` output for a known grid.

## 1.4. Acceptance Criteria

- [ ] A `DawPanel` organism exists at `Source/Features/Daw/Ui/Organisms/DawPanel.h/.cpp`, is constructed with an injected `MasterGridService` (PRD-0064) and `TimelineTransform` (PRD-0065), and uses no singletons or global mutable state.
- [ ] The `DawPanel` is docked at the **top** of the application window, full window width, above the decks/mixer, when added to the PRD-0005 `MainContentComponent` layout.
- [ ] The panel has an always-visible collapse/expand toggle. Toggling between expanded and collapsed is **instant** (no animation), and the decks/mixer below reflow immediately to use the reclaimed/yielded vertical space.
- [ ] The panel reports a `collapsedHeight` (header strip only) and an `expandedHeight` (header strip + ruler) as two fixed values; `MainContentComponent.resized()` reserves the current value at the top and lays out the decks/mixer in the remaining bounds below.
- [ ] When expanded, a `TimeRuler` molecule exists at `Source/Features/Daw/Ui/Molecules/TimeRuler.h/.cpp` and renders bars and beats across the full panel width.
- [ ] A `RulerTick` atom exists at `Source/Features/Daw/Ui/Atoms/RulerTick.h/.cpp`, draws a single vertical tick, and renders "bar" and "beat" kinds with distinct tick heights and tonal weight (no colour difference).
- [ ] Bar and beat tick positions are computed from `MasterGridService` grid lines mapped through `TimelineTransform`; the panel does not compute tempo or grid positions independently (single-source-of-truth preserved).
- [ ] Bar numbers are rendered in `Space Mono Regular` at the **top of the ruler**, aligned to their bar tick on the pixel grid.
- [ ] The ruler renders at a fixed default zoom (bars-per-screen constant) and starts at bar 1 (the master grid phase origin); no scroll offset is applied.
- [ ] The `DawPanel` exposes the `TimelineTransform` instance it uses so later PRDs (lanes PRD-0067, clips PRD-0068, playhead PRD-0070) can render onto the identical coordinate space.
- [ ] The entire shell complies with `DESIGN.md`: zero border-radius everywhere; any borders are `2px solid #2d2d2d`; module separation from the decks below uses a `surface-container-*` tonal shift, not a 1-px divider; the collapse toggle uses the standard active (`#2d2d2d` fill / `#fdfdfd` text) and inactive (`#fdfdfd` fill / `#2d2d2d` text) button states; any shadow is a dithered (checkerboard, zero-blur) drop, never a gradient.
- [ ] Every tick, label, and border aligns to the integer pixel grid (no sub-pixel tick positions that would blur the 1-bit aesthetic).
- [ ] No zoom or scroll interaction handlers are added by this PRD (deferred to PRD-0070); the panel renders a static ruler at the default zoom.
- [ ] No lane content, channel-group headers, clip rendering, or live playhead are added by this PRD (PRD-0067, PRD-0068, PRD-0070 respectively); the expanded panel below the ruler is an empty `surface-container-*` canvas reserved for those PRDs.
- [ ] No new audio-thread code is added; all panel work (layout, painting, grid reads) happens on the message/UI thread, and master state is read via the existing `MasterGridService` snapshot accessor with no allocations, locks, or I/O on the audio thread.
- [ ] A test file under `Tests/` (`DawPanelShellTests.cpp` or similar) verifies collapse/expand height toggling, expected bar/beat tick counts for a known BPM + sample-rate + default-zoom combination, distinct bar/beat tick kinds, and tick x-positions matching `TimelineTransform` output.

## 1.5. Grey Areas

### 1.5.1. Docked-Top vs Floating Panel

The DAW panel could be a fixed top-docked region of the main window or a floating/detachable window (as some DAWs and DJ tools allow for secondary displays).

**Resolution:** Docked at the top, non-floating. EPIC-0008 §1.2.1 explicitly specifies "a DAW panel docked at the top of the application, above the decks/mixer." A floating window introduces window-management, multi-monitor, and focus concerns that are out of scope for the foundational Epic and add no value to the core "see what the decks are doing" experience. A detachable/secondary-display mode is a plausible future-Epic enhancement; this PRD builds only the docked region. Keeping it docked also makes the collapse/expand reflow contract with the decks below unambiguous.

### 1.5.2. Collapse Behaviour: Animate vs Instant

Collapsing and expanding the panel could animate (height eased over a few hundred milliseconds) or snap instantly.

**Resolution:** Instant, no animation. `DESIGN.md` mandates a rigid, brutalist interface ("Music may be fluid, the interface must be rigid") and defines depth through static tonal layering and pixel offset, not motion. Animated reflow would also force the decks below to re-layout every frame during the transition, which is wasted work for zero design-language benefit. The toggle flips the expanded flag, the height changes in one step, and both the panel and the decks reflow once. This matches the rest of the Sonik UI, where state changes are immediate surface inversions rather than transitions.

### 1.5.3. Default Panel Height and Competition for Vertical Space

The expanded panel competes with the decks/mixer for a fixed window height. How tall should it be, and how is the trade-off resolved?

**Resolution:** The expanded panel uses a fixed `expandedHeight` sized to exactly the header strip plus one ruler (one row of `Space Mono` bar labels plus the tallest bar tick) in this PRD — it is deliberately *thin*, because no lanes exist yet (lanes arrive in PRD-0067, which will revisit the expanded height to accommodate channel groups). The decks/mixer receive the remaining vertical space below. This keeps the foundational shell from stealing significant deck real estate before there is any lane content to justify it. PRD-0067 owns the question of how lane stacks grow the panel and whether the panel becomes internally scrollable; this PRD only reserves a minimal ruler-height band. The collapsed state returns essentially all vertical space to the decks (only the header strip remains).

### 1.5.4. Ruler Tick Density at Default Zoom

At the fixed default zoom, the ruler must decide how densely to draw ticks: every beat, every bar, or an adaptive subdivision.

**Resolution:** At the default zoom, draw one tick per **beat** and one labelled tick per **bar** (4 beats per bar assumed via the master grid; the grid service is the authority on beats-per-bar). Bar ticks are tall/heavy, beat ticks are short/light (tonal layering, §1.5.6). If the default zoom is dense enough that per-beat ticks would render closer than a legibility threshold (a fixed minimum pixel spacing), the ruler drops beat ticks and draws bars only, keeping the grid readable. This adaptive-drop rule is purely visual (a minimum-spacing guard) and does not change the underlying grid; it is the only density logic this PRD needs, and it lives entirely in the message-thread paint path. Full adaptive subdivision tied to a *variable* zoom is PRD-0070's concern.

### 1.5.5. Placement of Bar Labels

Bar-number labels could sit at the top of the ruler, the bottom, or inline with the ticks.

**Resolution:** Bar numbers sit at the **top** of the ruler, above the ticks, left-aligned to each bar tick on the pixel grid. This keeps the labels in a consistent reading band, leaves the bottom edge of the ruler clean to abut the (future) lane area, and matches the editorial/brutalist convention of a header row of monospaced numerals. Labels render in `Space Mono Regular` at a small body scale (not the massive display scale reserved for BPM/time), since they are dense reference numerals rather than a focal readout.

### 1.5.6. Deriving Ruler Visuals from Generic DESIGN.md Rules

`DESIGN.md` contains no DAW/timeline/ruler component specification. The ruler's visual language must be derived from the generic rules, and that derivation is a judgement call that should be stated explicitly.

**Resolution:** The ruler visuals are derived from `DESIGN.md`'s global rules and documented here as the canonical interpretation until `DESIGN.md` is amended: ticks are solid `#2d2d2d` on a `surface-container-*` ruler background; bar vs beat distinction uses **tick height plus tonal weight** (a heavier/taller bar tick, a lighter/shorter beat tick) rather than any colour, satisfying the strict-monochrome and tonal-layering rules; the ruler is separated from the lane area below by a surface shift (the "no-line" rule), not a divider; all ticks snap to integer pixels to preserve the 1-bit/pixel-art aesthetic; bar labels use `Space Mono Regular`. This PRD does not amend `DESIGN.md`; a future documentation pass may lift this interpretation into a formal "Timeline / Ruler" section of `DESIGN.md`. Stating the derivation here prevents later DAW PRDs from each re-inventing the ruler's look.

### 1.5.7. Persistence of Collapsed/Expanded State

Whether the panel remembers its collapsed/expanded state across application restarts is undecided.

**Resolution:** Defer persistence. In this PRD the collapsed/expanded flag is simple runtime state held by the `DawPanel`, defaulting to a sensible initial value (expanded, so the new DAW surface is discoverable on first run) and reset on every launch. Persisting UI layout preferences (panel state, zoom, scroll, lane collapse) is a project/settings-persistence concern owned by EPIC-0012 (project save/load) and any future app-settings PRD. Wiring persistence now would couple the foundational shell to a settings store that does not yet exist; a runtime-only flag is sufficient and keeps the dependency surface minimal.
