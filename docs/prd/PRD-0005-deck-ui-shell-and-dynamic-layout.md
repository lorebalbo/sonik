---
status: Not Implemented
epic: EPIC-0001
---

# 1. PRD-0005: Deck UI Shell and Dynamic Layout

## 1.1. Problem

Without a dedicated layout system, the application has no way to present deck components to the user. A DJ may begin a session with two decks for a standard mix, then add a third for a creative transition or a fourth for a multi-deck routine. If the layout is hardcoded, the DJ cannot adapt their workspace to the moment. If decks are added without a structured container, child components (waveform, transport, knobs) have no parent to size and position them, leading to overlapping controls, invisible elements, or wasted screen real estate. The deck shell also serves as the interaction surface for deck focus: the DJ needs an unambiguous visual indicator of which deck is active, because future features (MIDI mapping, keyboard shortcuts, global controls) route commands to the focused deck. Without this, the DJ may unknowingly adjust the wrong deck during a live performance.

## 1.2. Objective

The system provides a dynamic, responsive deck layout container that:
- Renders 1 to 4 deck shells in an adaptive grid layout (1 deck: full-width, 2 decks: side-by-side, 3 decks: 2 top + 1 full-width bottom, 4 decks: 2x2 grid) that recalculates on every deck addition, removal, or window resize.
- Provides an "Add Deck" control in the global toolbar and a per-deck "Remove Deck" control on each deck's header bar, enforcing safety invariants from PRD-0001.
- Displays a clear active-deck visual indicator (3-pixel colored left-border accent) on exactly one deck at all times, synchronized with `activeDeckId` in the state tree.
- Shows an empty-state drop zone on decks with no track loaded, communicating readiness to accept a drag-and-drop or library load.
- Animates layout transitions within 200 ms using ease-out easing for a fluid workspace experience.
- Enforces a minimum window size that guarantees every visible deck has enough space for usable controls.
- Scales correctly on standard-DPI and Retina/HiDPI displays.

## 1.3. User Flow

1. The user launches Sonik. Two `DeckShellComponent` instances render in a side-by-side layout below the global toolbar. Deck A shows the active-deck indicator (colored left border). Both decks display the empty-state drop zone.
2. The user clicks "Add Deck" in the global toolbar. The layout animates from 2-column to 2+1 (A and B on top, C full-width on bottom) within 200 ms. Deck C becomes the active deck.
3. The user clicks "Add Deck" again. The layout transitions to a 2x2 grid (A/B top, C/D bottom). The "Add Deck" button becomes disabled with tooltip: "Maximum 4 decks reached."
4. The user clicks anywhere on Deck B's shell surface. The active-deck indicator moves to Deck B. No layout change occurs.
5. The user presses `Cmd+2` (macOS) / `Ctrl+2` (Windows). The active deck switches to Deck B.
6. The user clicks "Remove" on Deck D (Empty state, no confirmation needed). The layout animates from 2x2 to 2+1 within 200 ms. "Add Deck" re-enables.
7. The user attempts to remove a playing deck. The remove button is grayed out with tooltip: "Stop playback to remove this deck."
8. The user resizes the window. The layout recalculates proportionally. Below 960x600 pixels, the OS clamps the resize.
9. With one deck remaining, the remove button is permanently disabled: "At least one deck is required."
10. The user loads a track onto Deck A. The empty-state drop zone is replaced by the deck's content area.

## 1.4. Acceptance Criteria

- [ ] The `DeckLayoutManager` renders 1 to 4 `DeckShellComponent` instances in the correct grid layout: 1 full-width, 2 side-by-side, 3 as 2+1, 4 as 2x2.
- [ ] Layout recalculates on deck addition/removal via state tree listener callbacks (no polling).
- [ ] Layout recalculates on window resize via the JUCE `resized()` callback chain.
- [ ] An "Add Deck" button in the global toolbar is enabled when deck count < 4, disabled at 4 with tooltip.
- [ ] Each `DeckShellComponent` has a "Remove Deck" button (X icon) in its header bar, disabled when playing or when only 1 deck remains, with appropriate tooltips.
- [ ] Removing an empty deck requires no confirmation. Removing a deck with a loaded track (Stopped/Paused) shows a confirmation dialog.
- [ ] Exactly one deck displays the active-deck indicator at all times, synchronized with `activeDeckId`.
- [ ] Clicking any non-interactive area of a deck sets it as active.
- [ ] Keyboard shortcuts `Cmd+1` to `Cmd+4` (macOS) / `Ctrl+1` to `Ctrl+4` (Windows) switch the active deck. Shortcuts for non-existent decks are no-ops.
- [ ] Layout transitions animate over 200 ms with ease-out easing. Rapid successive operations cancel the current animation and animate from intermediate positions.
- [ ] Empty decks display a drop zone with instructional text ("Drop a track here or browse your library") and a dashed-border visual target.
- [ ] Loading a track replaces the drop zone with the content area. Ejecting reverts to the drop zone.
- [ ] Minimum window size of 960 x 600 pixels enforced via `setResizeLimits()`.
- [ ] Each `DeckShellComponent` has a minimum internal size of 420 x 280 pixels.
- [ ] All UI elements render correctly on Retina/HiDPI displays (2x and 3x scale factors).
- [ ] A single dark color theme is used. No theme switching in this PRD.
- [ ] Active deck indicator uses a 3-pixel colored left-border accent (A = blue, B = orange, C = green, D = purple).
- [ ] The header bar displays the deck letter and hosts the Remove button.
- [ ] The `DeckShellComponent` provides a single content `Component` below the header for future child components.
- [ ] All deck layout and shell code resides under `Source/Features/Deck/UI/`.
- [ ] Dependencies (state tree reference) are passed via constructor injection. No singletons.
- [ ] Touch input is not explicitly supported in this PRD.