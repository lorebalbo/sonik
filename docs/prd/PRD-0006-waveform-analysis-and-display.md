---
status: Not Implemented
epic: EPIC-0001
---

# 1. PRD-0006: Waveform Analysis and Display

## 1.1. Problem

A DJ navigating a live set relies on visual feedback as much as auditory feedback. Waveforms are the primary visual instrument: they show the energy contour of a track at a glance, reveal breakdowns, drops, and vocal entries before they arrive, and give the DJ a spatial sense of where the playhead sits relative to the full track. Without waveforms, mixing becomes reactive instead of anticipatory — the DJ cannot see an upcoming break to time a transition, cannot visually confirm that two tracks are phrase-aligned, and cannot distinguish a low-energy intro from a high-energy chorus at scanning speed. The waveform system also serves as the mounting surface for future visual overlays — beat grids, cue markers, loop regions — that transform a simple amplitude plot into a navigational instrument.

## 1.2. Objective

The system provides a background waveform analysis pipeline and dual waveform display per deck that:
- Analyzes decoded PCM audio on a background thread, computing peak and RMS amplitude data at a base resolution of 256 samples per waveform point (~172 points/sec at 44.1 kHz), completing analysis of an 8-minute track in under 1 second.
- Generates a multi-resolution mipmap (1x, 2x, 4x, 8x, 16x, 32x reduction), enabling O(1) lookup for any zoom level.
- Caches analyzed waveform data in SQLite (keyed by file content hash), eliminating redundant re-analysis.
- Renders an overview waveform displaying the full track with a playhead marker and viewport indicator.
- Renders a scrolling detail waveform centered on the playhead, supporting 6 discrete zoom levels (4s to 64s of visible audio).
- Achieves 60 fps for the scrolling detail waveform using GPU-accelerated rendering.
- Applies three-band frequency coloring (low=red, mid=green/yellow, high=blue/cyan) computed during analysis.
- Exposes a `samplePositionToPixelX` coordinate mapping for future overlay features (beat grid, cue markers, loops).

## 1.3. User Flow

1. The user loads a track onto Deck A. The decoding pipeline completes. Waveform analysis is triggered automatically.
2. If cached data exists for this file's content hash, the waveforms render immediately from cache (< 100 ms). No progress indicator.
3. If no cache exists, the background analysis thread processes the buffer. The overview waveform renders progressively from left to right. The detail waveform shows analyzed regions and flat lines for unanalyzed regions.
4. Analysis completes. Full waveform data is written to the SQLite cache. Both waveforms display the complete track.
5. The user presses Play. The detail waveform scrolls horizontally, centered on the playhead (fixed vertical line at center). The overview shows a moving playhead marker and a viewport rectangle.
6. The user scrolls the mouse wheel over the detail waveform. Zoom changes one step per tick through 6 levels: 4s, 8s, 16s, 32s, 48s, 64s. The waveform transitions smoothly within 100 ms.
7. The user clicks on the overview waveform. The transport receives a seek command to the clicked position. The detail waveform jumps to the new location.
8. The user observes three-band coloring: bass-heavy sections render red, vocal passages show green/yellow, hi-hat patterns appear as blue/cyan.
9. The user resizes the window. Both waveform components resize and re-render.
10. Loading a new track clears and replaces the waveform data. Ejecting clears to empty state.

## 1.4. Acceptance Criteria

- [ ] Waveform analysis executes on a dedicated background thread, never on the UI or audio thread.
- [ ] Analysis computes peak and RMS values at 256 samples per point for both left and right channels.
- [ ] Per-point frequency band energy is computed (low: 20-250 Hz, mid: 250-4000 Hz, high: 4000-20000 Hz) using 3-band crossover filters.
- [ ] A multi-resolution mipmap is generated at reduction factors of 2x, 4x, 8x, 16x, and 32x (6 levels total).
- [ ] Analysis of an 8-minute 44.1 kHz stereo track completes in under 1 second.
- [ ] Analyzed data is cached in SQLite keyed by content hash. Loading cached data completes in under 100 ms.
- [ ] If cached data exists, no re-analysis is performed.
- [ ] Analysis progress is published to the deck's ValueTree for progressive rendering.
- [ ] Analysis can be cancelled on track eject or new track load without resource leaks.
- [ ] The overview waveform renders the full track horizontally in a fixed-width component.
- [ ] The overview waveform displays a playhead marker updated at ~60 Hz from the transport's atomic position.
- [ ] The overview waveform displays a viewport rectangle indicating the detail view's visible region.
- [ ] Clicking the overview waveform issues a seek command to the transport at the corresponding sample position.
- [ ] The detail waveform scrolls centered on the playhead with a fixed vertical playhead marker at horizontal center.
- [ ] 6 discrete zoom levels: 4s, 8s, 16s, 32s, 48s, 64s. Default is 16s on track load.
- [ ] Zoom changes via mouse scroll wheel or trackpad pinch, one level per event.
- [ ] Zoom transitions animate within 100 ms.
- [ ] The detail waveform selects the appropriate mipmap tier, avoiding over-drawing (max 4 points per pixel).
- [ ] Both waveform components use `juce::OpenGLContext` for GPU-accelerated rendering.
- [ ] Scrolling detail waveform achieves 60 fps minimum on hardware with OpenGL 3.2+.
- [ ] Software rendering fallback targets 30 fps if OpenGL is unavailable.
- [ ] Three-band frequency coloring: low=red, mid=green/yellow, high=blue/cyan, composited additively per column.
- [ ] Waveform renders as a filled peak envelope with a brighter inner RMS region.
- [ ] Both components mount into the DeckShellComponent content area and resize correctly.
- [ ] Correct rendering on Retina/HiDPI displays (2x and 3x scale factors).
- [ ] Loading a new track clears previous waveform data and renders the new track's waveform.
- [ ] Ejecting clears both components to empty state.
- [ ] Fully per-deck: multiple decks analyze and display independently.
- [ ] Beat grid overlay is NOT implemented; `samplePositionToPixelX` coordinate mapping is exposed for future use.
- [ ] Cue/loop markers are NOT implemented; coordinate mapping API is available for future use.
- [ ] Mono-summed display only (max of L/R for peak, quadratic mean for RMS). Per-channel data is stored for future stereo display.
- [ ] All code resides under `Source/Features/Waveform/`.
- [ ] Dependencies passed via constructor injection. No singletons.