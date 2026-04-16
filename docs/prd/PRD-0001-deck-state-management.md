---
status: Implemented
epic: EPIC-0001
---

# 1. PRD-0001: Deck State Management

## 1.1. Problem

Every interaction a DJ has with Sonik — loading a track, pressing play, setting a cue point, adjusting pitch — reads from or writes to a deck's state. Without a centralized, well-defined state system, these operations become fragile: the UI could display stale metadata while the audio engine plays a different track, cue points could silently vanish between sessions, or adding a third deck mid-set could corrupt the other two. Professional DJs cannot tolerate state inconsistencies during a live performance — a glitch in state management is indistinguishable from a software crash. No other feature (mixer, effects, library, MIDI mapping) can be built reliably until every deck's state is a single, trustworthy source of truth that the entire application can observe without risk of audio-thread stalls or data races.

## 1.2. Objective

The system provides a centralized, observable state container for 1 to 4 independently controllable decks, ensuring that:
- Every deck property (playback status, playhead position, loaded track metadata, pitch, gain, cue points, loops, beatgrid, key, stem state, quantize, slip) is readable by any component without polling or locking.
- State mutations originate from a single codepath and propagate to all observers (UI, audio engine, future MIDI layer) via the JUCE Listener pattern within one message-loop cycle.
- Track-specific data (cue points, beatgrid, key) persists across sessions in a SQLite database so that a DJ's preparation work is never lost.
- Deck lifecycle operations (add, remove, load track, eject track) enforce safety invariants at the state level, preventing illegal transitions (e.g., removing a playing deck) before they reach the audio engine.
- All cross-thread state access between the UI thread and the audio thread occurs exclusively through `std::atomic` and lock-free structures, with zero allocations or blocking on the audio thread.

## 1.3. User Flow

1. The user launches Sonik for the first time. The application initializes with 2 decks (Deck A and Deck B) in side-by-side layout, both in the Empty state with no track loaded. Deck A is the active (focused) deck.
2. The user drags an audio file from the Library (or from the OS file system) onto Deck A. The deck transitions from Empty to Stopped, the playhead sits at position 0, and the track's metadata (title, artist, duration, album art, sample rate, bit depth) populates the deck's state. Previously saved cue points, beatgrid, and key data for this file are loaded from the database.
3. The user presses Play on Deck A. The playback status transitions from Stopped to Playing and the playhead advances in real time.
4. The user presses Pause. The playback status transitions from Playing to Paused; the playhead holds its current position.
5. The user presses Play again. Playback resumes from the paused position.
6. The user adjusts the pitch fader on Deck A. The pitch value updates in state; the audio engine reads the new value on its next processing cycle.
7. The user clicks the "Add Deck" button. Deck C appears in the layout (now a 2+1 grid). Deck C initializes in the Empty state. Deck C becomes the active deck.
8. The user loads a track onto Deck C and begins playback. Decks A and C now play independently.
9. The user clicks the "Remove Deck" button on Deck A while it is playing. The remove control is disabled (grayed out) with a tooltip: "Stop playback to remove this deck." No state change occurs.
10. The user stops Deck A and clicks "Remove Deck" again. A confirmation dialog appears: "Remove Deck A? The loaded track will be unloaded." The user confirms. Deck A is removed, the layout adapts to a side-by-side view of Deck B and Deck C, and the active deck switches to Deck B.
11. The user loads a new track onto Deck C (replacing the current one). Track-specific state (metadata, cue points, beatgrid, key, waveform, stem separation, loop state) resets and repopulates from the new track's data. Deck-level state (gain/trim, quantize mode, slip mode) persists unchanged.
12. The user clicks "Eject" on Deck B (which is stopped with a track loaded). The deck returns to the Empty state — all track-specific state clears to defaults.
13. The user quits Sonik. The application persists the current session layout (deck count, which tracks are loaded on which decks, deck-level preferences) and all track-specific data (cue points, beatgrid, key) to the database.
14. The user relaunches Sonik. The previous session's deck layout and loaded tracks are restored. All decks initialize in the Stopped state (playback never auto-resumes on launch).

## 1.4. Acceptance Criteria

- [ ] The system initializes with 2 decks (A, B) on first launch and restores the previous session's deck count on subsequent launches.
- [ ] Decks can be added up to a hard maximum of 4 and removed down to a minimum of 1.
- [ ] Each deck's state is fully independent; mutating one deck's state never affects another deck.
- [ ] A deck that is in the Playing state cannot be removed; the remove control is visually disabled with a tooltip explanation.
- [ ] A deck in Paused or Stopped state with a loaded track shows a confirmation dialog before removal.
- [ ] Loading a track into a deck resets all track-specific state (metadata, cue points, beatgrid, key, loop, stem separation) and loads persisted data for the new track from the database.
- [ ] Loading a track into a deck preserves deck-level state (gain/trim, quantize mode, slip mode).
- [ ] Pitch resets to 0% (original speed) on track load.
- [ ] Ejecting a track returns the deck to the Empty state; eject is blocked while the deck is Playing.
- [ ] Every deck state property is observable via the JUCE Listener pattern; no component polls for state.
- [ ] All state is stored in a single `juce::ValueTree` hierarchy that serves as the sole source of truth.
- [ ] Cross-thread state access between the UI and audio threads uses only `std::atomic` and lock-free structures — zero allocations, locks, or blocking on the audio thread.
- [ ] Track-specific data (cue points, beatgrid, key) persists in SQLite keyed by file path and content hash, surviving application restarts.
- [ ] Session layout (deck count, loaded tracks, deck-level preferences) persists across application restarts.
- [ ] On relaunch after a crash, the last persisted session state is restored with all decks in Stopped state.
- [ ] Playback status transitions follow the state machine: Empty -> Stopped (on track load), Stopped -> Playing, Playing -> Paused, Paused -> Playing, Paused -> Stopped, Playing -> Stopped, any state -> Empty (on eject/track removal).
- [ ] An `activeDeckId` property exists at the root of the state tree. Newly added decks automatically become active. Clicking a deck's UI surface switches focus.
- [ ] Decks keep their assigned letter (A, B, C, D) permanently. Removing a deck preserves other decks' identities. Adding a new deck fills the first available letter.
- [ ] An `isMasterTempo` boolean exists per deck, with at most one deck set to true at any time. Removing the master deck clears the flag globally.
- [ ] Eject is a first-class operation separate from deck removal, returning the deck to Empty without removing it from the layout.
- [ ] On track load, cue points from the database that fall beyond the new track's duration are flagged as invalid and hidden but not deleted.
- [ ] Stem separation state resets to "not separated" on track load; cached stems (keyed by content hash) are offered for fast restore but not auto-activated.
- [ ] State fields are classified as track-specific (reset on load: metadata, playhead, cue points, beatgrid, key, loop, stems, waveform) or deck-level (persist across loads: gain/trim, quantize, slip).