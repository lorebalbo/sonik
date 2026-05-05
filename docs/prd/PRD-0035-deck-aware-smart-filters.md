---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0032
  - PRD-0033
  - PRD-0001
---

# 1. PRD-0035: Deck-Aware Smart Filters

## 1.1. Problem

During a live performance, a DJ must continuously select the next track to mix. Choosing a track that is harmonically and rhythmically compatible with the one currently playing requires two simultaneous mental calculations: first, computing which Camelot Wheel keys are harmonically adjacent to the key of the playing deck; second, computing the effective BPM range that accounts for the current pitch-fader position, which can deviate significantly from the BPM stored in the database. These calculations must happen while the DJ is simultaneously monitoring the crowd, managing the crossfader, and listening for cues.

Without automated deck-aware filtering, the DJ must either memorize the Camelot Wheel, rely on error-prone mental arithmetic, or pause to manually type search operators (`bpm:125-135 key:Am`) into the search bar. Any of these paths increases cognitive load and reaction time, raising the probability of selecting an incompatible track — a key clash or significant tempo mismatch that disrupts the energy of the room.

The problem is compounded when the DJ has shifted the pitch fader: the stored BPM in the library no longer reflects the deck's actual tempo. A track stored at 128 BPM but playing at +4% pitch is effectively running at 133.12 BPM, a value no manual search will naturally surface.

## 1.2. Objective

The system provides two reactive smart-filter controls — KEY MATCH and BPM MATCH — embedded in the Library UI Shell (PRD-0033). When activated, these filters automatically narrow the visible track collection in real time based on the effective state of all currently loaded decks, eliminating manual harmonic and tempo calculations from the live mixing workflow.

Specifically, the system ensures that:
- Activating KEY MATCH instantly narrows the library to tracks whose key is harmonically compatible with the keys of all loaded decks, as defined by Camelot Wheel adjacency rules.
- Activating BPM MATCH instantly narrows the library to tracks whose stored BPM falls within a configurable ±BPM VISION window around the effective tempo of each loaded deck.
- Both filters update automatically whenever deck state changes (new track loaded, pitch fader adjusted, track ejected), with a debounce strategy that prevents query storms from rapid pitch-fader movements.
- The Library module observes deck state exclusively through the shared `juce::ValueTree` and the identifier constants in `DeckIdentifiers.h`, maintaining a strict module boundary with no direct dependency on any Deck feature header.

## 1.3. User Flow

1. The DJ loads a track onto Deck A. Deck A is playing at an effective tempo of 128.0 BPM in key 8A (A minor on the Camelot Wheel). No smart filters are yet active; the library shows the full collection.
2. The DJ clicks the KEY MATCH button in the Library toolbar. The button enters its activated state. The library table immediately narrows to tracks whose key index falls within the harmonically compatible set for 8A: keys 8A, 8B, 7A, and 9A. Tracks with any other key index are excluded from the result set.
3. The DJ clicks the BPM MATCH button. The button activates. BPM VISION is at its default value of 6. The library table now applies both filters simultaneously as an AND condition: only tracks whose key is harmonically compatible AND whose BPM falls within [122.0, 134.0] are shown.
4. The DJ moves the pitch fader on Deck A to +4%. The effective tempo of Deck A becomes 133.12 BPM. After a 150 ms debounce, the BPM MATCH filter recomputes the window to [127.12, 139.12] and the table refreshes with the updated result set.
5. The DJ adjusts the BPM VISION input from 6 to 12. The BPM window widens to [121.12, 145.12] and the table refreshes immediately.
6. The DJ loads a second track onto Deck B, which has key 10B (B major on the Camelot Wheel) and effective tempo 140.0 BPM. The KEY MATCH filter now computes the union of compatible keys from both decks: 8A, 8B, 7A, 9A (from Deck A) plus 10B, 10A, 9B, 11B (from Deck B). The BPM MATCH filter shows tracks within the union of both BPM windows: [121.12, 145.12] from Deck A and [128.0, 152.0] from Deck B, effectively [121.12, 152.0].
7. The DJ ejects the track from Deck A. Only Deck B has a track loaded. Both filters recompute based on Deck B's state alone.
8. The DJ ejects the track from Deck B as well. No deck has a track loaded. Both KEY MATCH and BPM MATCH filters are automatically suspended: the full library is shown regardless of the toggle state. The toggle buttons retain their activated visual appearance but display a distinct suspended indicator, communicating that the filter intent is stored but currently inactive.
9. The DJ loads a new track onto Deck A. Both filters resume immediately using the new deck's key and effective tempo.
10. The DJ enables the half-time BPM compatibility option from the filter settings panel. With Deck A at 128.0 BPM effective tempo and BPM VISION = 6, BPM MATCH additionally includes tracks in the half-time range [58.0, 70.0] and the double-time range [250.0, 262.0].
11. The DJ deactivates KEY MATCH by clicking its toggle button again. The button returns to the inactive state and the library re-dispatches the current query with only BPM MATCH applied.
12. The active filter state (which toggles are on, the BPM VISION value, the half-time option) persists across application restarts. On relaunch with no decks loaded, the toggles remember their last active state but remain suspended until a deck loads a track with valid analysis data.

**Alternative flow — deck has no analyzed track:**

If Deck A has a track loaded but that track has no completed BPM or key analysis (analysis status is pending or failed), Deck A contributes nothing to the filter computation. If no other deck has valid analysis data, both filters behave identically to the no-deck-loaded case: the full library is shown and a non-blocking notice appears below the filter controls explaining that the active deck has no usable analysis data.

**Alternative flow — empty result set:**

If the combined KEY MATCH and BPM MATCH filters produce zero matching tracks, the table area displays an empty-state panel with the message: "No tracks match the current deck filters. Try widening BPM VISION or loading a different track." The panel includes a single action button labelled "Clear Filters", which deactivates both KEY MATCH and BPM MATCH and re-dispatches the query showing the full unfiltered collection.

## 1.4. Acceptance Criteria

### 1.4.1. KEY MATCH Toggle and Camelot Wheel Compatibility

- [ ] Activating the KEY MATCH toggle narrows the library table to only those tracks whose stored key index is a member of the harmonically compatible set computed from the union of all loaded decks' keys.
- [ ] The harmonically compatible set for a given Camelot key index K includes: the exact key K, the key one position clockwise on the Camelot Wheel (K + 1, wrapping from 12 to 1), the key one position counterclockwise (K − 1, wrapping from 1 to 12), and the relative key (same number, opposite letter: A ↔ B).
- [ ] For Camelot index 12A, the compatible set is exactly: 12A, 1A, 11A, 12B — confirming correct wrap-around at the upper boundary.
- [ ] For Camelot index 1B, the compatible set is exactly: 1B, 2B, 12B, 1A — confirming correct wrap-around at the lower boundary.
- [ ] When multiple decks have tracks loaded with different keys, the KEY MATCH filter shows tracks whose key index belongs to the union of all individual compatible sets; a track is included if it is compatible with at least one loaded deck.
- [ ] Deactivating the KEY MATCH toggle removes the key constraint and the library re-dispatches the current query without any key filter applied.

### 1.4.2. BPM MATCH Toggle and BPM VISION Window

- [ ] Activating the BPM MATCH toggle narrows the library table to tracks whose stored BPM satisfies `effectiveTempo − bpmVision ≤ trackBPM ≤ effectiveTempo + bpmVision` for at least one loaded deck.
- [ ] `effectiveTempo` for each deck is computed as `IDs::bpm × IDs::speedMultiplier` read from that deck's ValueTree subtree; the raw stored BPM tag is never used as the effective tempo.
- [ ] BPM VISION defaults to 6.0 on first launch. Its value is persisted in application state and is restored to the same value on the next launch.
- [ ] The BPM VISION control accepts numeric input in the range [0.0, 30.0]. Values entered outside this range are silently clamped to the nearest boundary.
- [ ] When BPM VISION is set to 0.0, only tracks whose stored BPM equals the effective tempo within a ±0.05 BPM floating-point tolerance are included.
- [ ] When multiple decks are active with different effective tempos, the BPM MATCH filter includes tracks that fall within the union of all individual BPM windows; a track satisfying any single deck's window is included.
- [ ] Deactivating the BPM MATCH toggle removes the BPM constraint and the library re-dispatches the current query without any BPM filter applied.

### 1.4.3. DeckAwareFilterState Computation

- [ ] A `DeckAwareFilterState` struct is computed on the Message Thread whenever a relevant ValueTree property changes. It holds a `juce::Array<int>` of all compatible key indices (union across all decks) and a `juce::Array<std::pair<double, double>>` of [min, max] BPM intervals (one entry per active deck with valid analysis).
- [ ] Only decks whose BeatGrid and KeyInfo analysis is in a completed state contribute to the `DeckAwareFilterState`. Decks with pending, in-progress, or failed analysis are silently excluded from the computation.
- [ ] When the resulting `DeckAwareFilterState` contains an empty key set and an empty BPM interval list — because no deck has valid analysis data or no deck has a track loaded — both KEY MATCH and BPM MATCH filters are suspended and the full unfiltered library is returned regardless of toggle state.

### 1.4.4. Reactive Re-dispatch on ValueTree Property Changes

- [ ] `LibraryComponent` registers as a `juce::ValueTree::Listener` on every Deck subtree in the shared ValueTree at construction time and deregisters in its destructor, with no listener reference outliving the component.
- [ ] When `IDs::keyIndex` changes on any Deck subtree, `DeckAwareFilterState` is recomputed and the current query is re-dispatched to `LibraryQueryThread` immediately, without waiting for any debounce timer.
- [ ] When `IDs::speedMultiplier` or `IDs::bpm` changes on any Deck subtree, a 150 ms debounce timer is started (or reset if already running). Only after the timer fires is the `DeckAwareFilterState` recomputed and the query re-dispatched. Intermediate property changes during the 150 ms window do not trigger intermediate queries.
- [ ] When a deck loads a new track (a change to `IDs::filePath` under `TrackMetadata` is detected), the `DeckAwareFilterState` is recomputed and the query is re-dispatched immediately without debounce.
- [ ] The Library module does not `#include` any header from `Source/Features/Deck/` other than `DeckIdentifiers.h`. All ValueTree property access uses exclusively the identifier constants defined in that file.

### 1.4.5. AND Combination When Both Filters Are Active

- [ ] When both KEY MATCH and BPM MATCH are active simultaneously, only tracks satisfying both the key compatibility condition and the BPM window condition are included in the result set (logical AND, not OR).
- [ ] The SQL query dispatched to `LibraryQueryThread` expresses both constraints within a single `WHERE` clause; the result set is not computed by running two separate queries and intersecting them in memory.

### 1.4.6. Filter Suspended State When No Deck Has a Track Loaded

- [ ] When no deck has a track loaded, both KEY MATCH and BPM MATCH filters are suspended and the full library (subject to any active text search or scope operators) is returned.
- [ ] The KEY MATCH and BPM MATCH toggle buttons retain their activated visual appearance while suspended, and display a distinct suspended indicator — such as a dimmed overlay or a dash suffix — clearly communicating that the filter intent is stored but not currently reducing the result set.
- [ ] When any deck subsequently loads a track with valid BPM and key analysis data, both suspended filters resume automatically and the query is re-dispatched immediately, without requiring the DJ to re-activate the toggles.

### 1.4.7. Half-Time BPM Compatibility Option

- [ ] A half-time BPM compatibility option is available in the filter settings panel. When enabled, the BPM MATCH filter additionally includes tracks in the half-time range `[0.5 × effectiveTempo − bpmVision, 0.5 × effectiveTempo + bpmVision]` and the double-time range `[2 × effectiveTempo − bpmVision, 2 × effectiveTempo + bpmVision]` for each active deck.
- [ ] The half-time option is disabled by default on first launch and its enabled/disabled state persists across application restarts.
- [ ] Enabling the half-time option while BPM MATCH is already active triggers an immediate query re-dispatch with the extended BPM windows; no manual toggle interaction is required.

### 1.4.8. Empty Result State

- [ ] When the combined active filters produce zero matching tracks, the table area is replaced by an empty-state panel displaying a descriptive message and a "Clear Filters" action button.
- [ ] Activating "Clear Filters" from the empty-state panel deactivates both the KEY MATCH and BPM MATCH toggles and immediately re-dispatches the query, restoring the full unfiltered collection view.