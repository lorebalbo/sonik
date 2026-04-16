---
status: Implemented
epic: EPIC-0001
---

# 1. PRD-0007: Track Info Display

## 1.1. Problem

A DJ performing live makes split-second decisions based on what they see on screen. After loading a track (PRD-0003) and initiating playback (PRD-0004), the deck shell (PRD-0005) exists but displays no information about the loaded track beyond the waveform. The DJ cannot see the track title to confirm the right song is loaded, cannot see the artist to announce the track, cannot see the BPM to mentally plan a transition, cannot see the key to judge harmonic compatibility with the other deck, and cannot see elapsed or remaining time to know how urgently the next mix must begin. Without these, the DJ must rely entirely on memory and ear, which is error-prone under stage pressure and impossible for unfamiliar tracks. Every professional DJ tool (CDJ-3000, Traktor, Serato, rekordbox) prominently displays this information because it is the minimum viable read-out a DJ needs to operate confidently. The track info display is a pure read-only UI component: it consumes state published by PRD-0001, PRD-0003, and PRD-0004 and renders it visually. It modifies nothing.

## 1.2. Objective

The system provides a track info display component per deck that:
- Renders the loaded track's title, artist, BPM, musical key, album art, and elapsed/remaining time within the `DeckShellComponent` content area.
- Updates elapsed and remaining time at 60 Hz by reading the transport's atomic playhead position, with zero allocations, zero locks, and zero polling on the audio thread.
- Displays BPM to one decimal place (e.g., `128.0`) and musical key in Camelot notation (e.g., `8A`), showing a placeholder dash (`--`) when analysis data is not yet available.
- Truncates long title and artist strings with an ellipsis and scrolls the text on hover for full readability.
- Renders album art as a square thumbnail with a default placeholder when no art is embedded.
- Remains fully readable at the minimum deck shell size (420 x 280 pixels) and scales proportionally on Retina/HiDPI displays.
- Reacts to state changes via JUCE Listeners with zero polling, clearing all fields to defaults on track eject and populating from new metadata on track load.

## 1.3. User Flow

1. The user launches Sonik. Both decks are in the Empty state. The track info area within each deck shell is blank: no title, no artist, time reads `0:00 / 0:00`, BPM and key show `--`, and the album art area shows the default placeholder icon.
2. The user loads a track onto Deck A via drag-and-drop. As soon as metadata extraction completes (PRD-0003 publishes to the state tree before decoding finishes), the title and artist fields populate, album art appears (or a placeholder if none is embedded), BPM shows `--` (analysis has not yet run), and key shows `--`. Elapsed time reads `0:00`, remaining time reads the full track duration (e.g., `-6:32`).
3. The user presses Play. Elapsed time begins counting up and remaining time counts down, both updating at the display refresh rate (~60 Hz). The time is computed from the transport's atomic playhead position divided by the sample rate, representing the track's timeline (not wall clock time).
4. The user adjusts the pitch fader, changing `speedMultiplier` to 1.06. Elapsed and remaining time continue to reflect the playhead's position within the track. Time advances faster in real time because playback is faster, but the displayed values always represent where the playhead sits in the track's original timeline.
5. BPM analysis completes (future PRD). The BPM field updates from `--` to the detected value (e.g., `126.3`). When `speedMultiplier` is not 1.0, the display shows the effective BPM (original BPM multiplied by `speedMultiplier`, e.g., `133.9`).
6. Key detection completes (future PRD). The key field updates from `--` to the detected Camelot value (e.g., `8A`).
7. The track has a long title ("Aphex Twin - Selected Ambient Works Volume II - Extended Remaster"). The title truncates with an ellipsis at the component's width boundary. The user hovers over the title text; it begins scrolling horizontally to reveal the full string. The scroll stops when the mouse leaves.
8. The user pauses playback. Elapsed and remaining time freeze at the current playhead position. Resuming playback continues from where it left off.
9. The user stops playback. Elapsed time resets to `0:00` and remaining time shows the full track duration (the playhead resets to position 0 per PRD-0004).
10. The user ejects the track from Deck A. All fields clear: title and artist become blank, BPM and key return to `--`, elapsed and remaining time show `0:00 / 0:00`, and album art reverts to the default placeholder.
11. The user loads a different track. All fields repopulate from the new track's metadata. Previously displayed values are fully replaced.
12. The user resizes the window, shrinking Deck A to its minimum size. Font sizes remain readable (no smaller than 11pt for secondary text, 13pt for title). Album art scales down but remains visible. Time, BPM, and key fields never truncate — they use fixed-width formatting that always fits.
13. On a Retina display, all text and the album art thumbnail render at native resolution with no blurriness.

## 1.4. Acceptance Criteria

- [ ] A `TrackInfoComponent` renders within each `DeckShellComponent` content area, positioned above the waveform display area.
- [ ] The component displays: track title, artist name, album art, BPM, musical key, elapsed time, and remaining time.
- [ ] Title and artist text are read from the deck's ValueTree state (fields published by PRD-0003 metadata extraction).
- [ ] Album art is displayed as a square thumbnail (80 x 80 logical pixels at 1x scale). If no art is available, a default placeholder image renders in its place.
- [ ] Album art image data is read from the LRU cache populated by PRD-0003 (keyed by content hash). The component does not decode or extract art itself.
- [ ] Elapsed time is computed as `playheadPosition / sampleRate` and displayed in `M:SS` format (e.g., `0:00`, `3:45`, `12:07`).
- [ ] Remaining time is computed as `(totalSamples - playheadPosition) / sampleRate` and displayed in `-M:SS` format with a leading minus sign (e.g., `-6:32`).
- [ ] Time values update at the UI repaint rate (~60 Hz) by reading the transport's `std::atomic<int64_t>` playhead position. No polling threads; updates driven by a `juce::Timer` callback at 16 ms intervals.
- [ ] Time display reflects track position (where the playhead sits on the original timeline), not wall-clock duration. At `speedMultiplier` 2.0, a 6-minute track's remaining time counts down from `-6:00` to `0:00` in 3 real-time minutes.
- [ ] BPM is displayed to one decimal place with fixed-width formatting (e.g., `128.0`, `85.5`, `174.2`).
- [ ] When `speedMultiplier` is not 1.0, the displayed BPM reflects the effective tempo: `originalBPM * speedMultiplier`, rounded to one decimal place.
- [ ] When BPM analysis data is not available (field absent or zero in the state tree), BPM displays `--`.
- [ ] Musical key is displayed in Camelot notation (e.g., `1A` through `12B`). When key detection data is not available, key displays `--`.
- [ ] Key display does not change with `speedMultiplier` unless a key-lock or transposition feature is active (not in this PRD).
- [ ] Title text longer than the available width truncates with an ellipsis (`...`). On mouse hover, the text scrolls horizontally at a constant rate (50 pixels/sec) to reveal the full string, then pauses for 1 second at the end, resets, and repeats while hovered.
- [ ] Artist text follows the same truncation and hover-scroll behavior as the title.
- [ ] Title renders at 13pt minimum font size. Artist, BPM, key, and time render at 11pt minimum.
- [ ] BPM and key fields use monospaced or tabular-figure font rendering so that digit width changes do not cause layout shifts.
- [ ] Elapsed and remaining time fields use monospaced or tabular-figure font rendering with fixed character width.
- [ ] All fields are readable at the minimum `DeckShellComponent` size of 420 x 280 pixels.
- [ ] On track load, metadata fields populate as soon as metadata is published to the state tree (before decoding completes). Elapsed time shows `0:00`, remaining time shows the full duration.
- [ ] On track eject, all fields reset: title and artist to empty, BPM and key to `--`, times to `0:00 / 0:00`, album art to placeholder.
- [ ] On loading a new track into a deck that already has a track, all fields clear and repopulate from the new track's metadata.
- [ ] The component observes state changes via `juce::ValueTree::Listener`. No polling for metadata, BPM, or key values.
- [ ] The `juce::Timer` used for time updates stops when the deck is in Empty state (no track loaded) and restarts on track load to avoid unnecessary repaints.
- [ ] Correct rendering on Retina/HiDPI displays (2x and 3x scale factors). Album art is pre-scaled to the appropriate resolution.
- [ ] All rendering occurs on the UI thread. The component performs zero audio-thread work.
- [ ] All code resides under `Source/Features/Deck/UI/`.
- [ ] Dependencies (state tree reference, album art cache reference) are passed via constructor injection. No singletons.

## 1.5. Grey Areas

1. **Elapsed time precision (M:SS vs M:SS.ms).** Full millisecond display is standard on CDJ hardware but consumes horizontal space and increases visual noise. Resolution: display `M:SS` format only. Sub-second precision (tenths or hundredths) is deferred to a future PRD if beat-phase or micro-cue workflows require it. The underlying computation retains full sample-level precision — only the display rounds down.

2. **Elapsed vs remaining time layout (toggle or simultaneous).** Pioneer CDJs show both simultaneously, Traktor shows one with a click-to-toggle. Resolution: show both simultaneously. Elapsed time renders left-aligned (counting up), remaining time renders right-aligned with a leading minus sign (counting down). This matches the dominant professional DJ convention (CDJ-3000 / XDJ-XZ) and avoids hiding information behind a toggle that the DJ may forget to flip during a performance.

3. **BPM display when speedMultiplier is not 1.0.** The display could show original BPM, effective BPM, or both. Resolution: display effective BPM only (`originalBPM * speedMultiplier`). The DJ needs to know the tempo they hear, not the tempo of the file at rest. A small "original" indicator is unnecessary complexity for this PRD; the pitch fader's percentage readout (future PRD) already communicates the delta. If the original BPM is needed, it is accessible via the track's persistent metadata.

4. **Key notation format (Camelot vs Open Key vs musical notation).** DJs overwhelmingly use the Camelot wheel for harmonic mixing because it reduces music theory to a simple number grid. Resolution: Camelot notation only for this PRD. A future Preferences PRD may add a user-configurable notation format (Open Key, standard musical key). The state tree stores key data in a canonical format that supports conversion to any notation.

5. **Album art loading performance for rapid track browsing.** If the user loads and ejects tracks rapidly, decoding album art from the file on each load could cause UI stutters. Resolution: the component never decodes art itself. It reads pre-decoded, pre-scaled thumbnails from the LRU memory cache populated by PRD-0003. Cache misses result in the placeholder appearing until the cache is populated — no blocking, no stutter.

6. **Text scroll behavior when deck is at minimum size.** At 420px width, nearly all titles will truncate. Continuous scrolling without hover would be visually distracting across 4 decks. Resolution: static truncation with ellipsis by default; scroll activates only on mouse hover over the specific text field. This keeps the UI calm during performance and gives the DJ on-demand access to full text.

7. **BPM and key field empty state before analysis.** Showing a blank field could imply a bug. Showing `0.0` for BPM is misleading. Resolution: display `--` (double dash) for both BPM and key when the value is absent or zero in the state tree. This is the universal "not yet determined" signifier on professional DJ hardware.

8. **Time display relationship to speedMultiplier.** Two valid interpretations exist: time could represent wall clock elapsed since pressing Play, or the playhead's position on the track's original timeline. Resolution: track-position time. The display answers "where am I in the song?" not "how long has playback been running." At 2x speed, reaching the 3:00 mark in the track takes 1.5 real minutes, but the display reads `3:00`. This matches CDJ and Traktor behavior and aligns with PRD-0004's definition: `elapsed = playheadPosition / sampleRate`.