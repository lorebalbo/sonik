---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0032
---

# 1. PRD-0033: Library UI Shell

## 1.1. Problem

The DJ has no way to visually browse, search, or load tracks. Even if the database schema (PRD-0030) and the asynchronous query engine (PRD-0032) are fully operational, they are invisible and inaccessible without a rendered interface. The absence of a UI shell means the collection of 50,000+ tracks exists only as rows in SQLite: there is nothing to click, nothing to sort, and no surface on which to drag a track onto a deck.

Beyond the simple absence of a shell, the design quality of the interface has a direct impact on workflow speed. A cluttered, poorly scannable table forces the DJ to read individual row values under time pressure, degrading performance during a live set. Column headers that cannot be sorted, a search bar that re-queries on every keystroke and introduces query storms, and a flat sidebar with no hierarchy all compound into an unusable experience. Missing state indicators — the track that is currently playing, the file that no longer exists on disk — leave the DJ guessing.

Without this feature the application has no library navigation at all. With a poor implementation, the application has navigation that slows the DJ down rather than accelerating the workflow.

## 1.2. Objective

Deliver a complete Library UI Shell that:
- Renders a persistent, three-zone layout: a left sidebar for collection navigation, a filter bar spanning the full width above the track area, and a virtualized sortable track table filling the remaining space.
- Provides a global search bar that accepts both free-text and Traktor-style scope operators, debounces input at 150 ms before dispatching queries via `LibraryQueryThread` (PRD-0032), and never blocks the JUCE Message Thread.
- Exposes KEY MATCH and BPM MATCH toggle buttons that activate deck-aware smart filtering reactively, narrowing the visible collection to harmonically and rhythmically compatible tracks without any additional user action beyond pressing the button.
- Presents a BPM VISION numeric input that defines the symmetric tolerance window (in BPM) used by the BPM MATCH filter, defaulting to ±6 BPM.
- Renders the track table using virtual rendering: row heights are uniform, `getNumRows()` returns the query result count, and `paintCell()` reads exclusively from a pre-fetched result buffer. No SQL is executed inside `paintCell()`.
- Conforms strictly to the `DESIGN.md` monochrome design system: strict `#000000` / `#f9f9f9` palette, zero `border-radius`, no gradients, zebra-striped rows, binary-inversion active states, and dithered context-menu shadows.
- Follows Atomic Design: `SearchBarAtom`, `SidebarItemAtom`, and `RatingAtom` compose into `FilterBarMolecule`, `SidebarMolecule`, and `TrackTableMolecule`, which together form the `LibraryComponent` organism.
- Stays within its module boundary: `Source/Features/Library/` must not include any header from another Feature module. All deck state is read from the root `juce::ValueTree`.

## 1.3. User Flow

1. The application launches. `LibraryComponent` is constructed with the root `juce::ValueTree` and a `TrackDatabase` reference. The sidebar shows "COLLECTION" as the active item. The track table renders the full library, sorted by date added descending. If the library is empty, the table area shows the centered empty-state message "No tracks in library. Add a Music Folder to get started."

2. The DJ clicks the search bar. The placeholder text "Search library..." disappears. An inline secondary placeholder expands below or beside the field to show supported scope operators: `bpm:`, `key:`, `rating:`, `title:`, `artist:`, `album:`. The sidebar and table remain visible and unchanged.

3. The DJ types "techno". After 150 ms of inactivity, `FilterBarMolecule` dispatches the query to `LibraryQueryThread`. While the query is in flight, the table retains its previous result set. When results arrive, the Message Thread swaps the result buffer and calls `repaint()`. The table now shows only matching tracks.

4. The DJ clears the search bar. The debounce timer resets and, after 150 ms, a query for the full current sidebar context (Collection) is dispatched. The table returns to showing all tracks.

5. The DJ types `bpm:128`. The scope operator is parsed client-side before SQL dispatch. The query is narrowed to tracks with a stored BPM between 127.5 and 128.5. Rows outside this range are not returned.

6. The DJ types `bpm:125-135`. The range operator is parsed. All tracks between 125 BPM and 135 BPM inclusive appear.

7. The DJ activates the BPM MATCH toggle. The button inverts its colors (white label on black background, zero `border-radius`). The effective tempo of the most recently active deck — computed as `beatgrid_bpm × speedMultiplier` — is read from the ValueTree. The BPM VISION window (default ±6) is applied. A new query is dispatched that also constrains BPM to the computed range. Any future change to that deck's speed multiplier automatically re-dispatches the query within 150 ms.

8. The DJ activates the KEY MATCH toggle. The active key of the currently loaded deck is read from the ValueTree. The query is further constrained to tracks in the same key and its harmonic relatives. The active table row count updates immediately.

9. The DJ adjusts the BPM VISION input from 6 to 3. The field accepts only integers between 1 and 50. On commit (Enter key or focus-out), the active BPM MATCH query is re-dispatched with the new window. The table updates.

10. The DJ clicks a column header ("BPM"). The table re-sorts ascending by BPM. A downward-pointing pixel-art arrow appears in the header cell. Clicking the same header again reverses to descending. Clicking a different header ("TITLE") clears the BPM sort and sorts ascending by title. If a query is already in flight when the header is clicked, the sort change is applied to the next completed query; the in-flight result is discarded.

11. The DJ clicks "PLAYLISTS" in the sidebar. The item inverts colors to show it is active. Playlist nodes expand inline within the sidebar. Clicking a playlist node filters the table to tracks in that playlist, ordered by position.

12. The DJ right-clicks a track row. A context menu appears with a 2 px offset dithered shadow (50% checkerboard of `#000000`, zero blur, zero `border-radius`). Items include "Load to Deck A", "Load to Deck B", "Add to Playlist", "Analyze Track", "Separate Stems", and "Remove from Library." Clicking "Load to Deck A" writes the track's `file_path` and `track_id` into the ValueTree under the Deck A subtree. No direct call is made to any Deck module.

13. The DJ double-clicks a track row. The track loads into the focused deck (the deck whose ValueTree subtree carries a `hasFocus = true` property). Behavior is identical to "Load to Deck A" from the context menu when Deck A is focused.

14. The DJ drags a track row from the table and drops it onto Deck A's drop zone. The track loads. The drag description is the track's `track_id` as a string. The receiving deck component reads `track_id` from the drop description and resolves the file path via `TrackDatabase`.

15. A track row shows a warning glyph in the indicator column. This means `is_missing = 1`. Double-clicking or right-clicking "Load to Deck A" opens a modal dialog offering "Relocate File" or "Cancel." The track does not load until the file is successfully relocated.

16. The DJ resizes the application window to a very small size (minimum 800 × 480 px). The sidebar collapses to icon-only mode below 200 px total sidebar width. The filter bar never truncates the search bar below 160 px. The BPM VISION input is hidden below 900 px total window width. The track table always occupies the remaining horizontal space.

17. The DJ uses the keyboard. Arrow keys move the selection highlight through the track table. Enter loads the selected track to the focused deck. `Cmd+F` focuses the search bar. Tab cycles focus through the filter bar controls, sidebar, and track table. Shift+Tab reverses the cycle.

## 1.4. Acceptance Criteria

### 1.4.1. Layout and Structure

- [ ] The `LibraryComponent` renders a two-column layout: a fixed-width left `SidebarMolecule` and a flexible right area containing `FilterBarMolecule` stacked above `TrackTableMolecule`.
- [ ] The sidebar has a default width of 200 px. Its width is user-adjustable via a resize handle. The chosen width is persisted to the `juce::ValueTree` and restored on next launch.
- [ ] Below a total window width of 800 px, the sidebar collapses to an icon-only strip of 40 px. Sidebar item text labels are hidden; tooltip labels appear on hover.
- [ ] The filter bar spans the full width of the right area and has a fixed height of 40 px. Its contents from left to right are: the `SearchBarAtom`, the KEY MATCH button, the BPM MATCH button, and the BPM VISION input.
- [ ] The track table fills all remaining vertical and horizontal space below the filter bar.
- [ ] All surfaces use only the following design-system colors: `#000000` (primary), `#f9f9f9` (surface), `#f3f3f4` (surface-container-low), `#e2e2e2` (surface-container-highest), `#ffffff` (surface-container-lowest). No other hex color values appear anywhere in the component tree.
- [ ] Every UI element in `LibraryComponent` and its child atoms, molecules, and the organism itself has a `border-radius` of exactly 0 px. No rounded corners appear.
- [ ] No CSS or JUCE gradient fills are used. Any visual depth effect uses a dithered 50% checkerboard pattern of `#000000`.

### 1.4.2. Search Bar

- [ ] The `SearchBarAtom` is a text input rendered on a `#ffffff` (surface-container-lowest) background with a 1 px `#000000` border and zero `border-radius`.
- [ ] When the field is empty and unfocused, the placeholder text "Search library..." is rendered in `#000000` at 50% opacity.
- [ ] On focus, a secondary hint area appears listing the supported scope operators: `bpm:`, `key:`, `rating:`, `title:`, `artist:`, `album:`. This hint is dismissed as soon as the user types at least one character or moves focus away.
- [ ] Input changes start a 150 ms debounce timer. The query is dispatched to `LibraryQueryThread` only after the timer fires without a subsequent keystroke. Rapid consecutive keystrokes do not cause multiple simultaneous in-flight queries.
- [ ] While a query is in flight, the table retains its previous result set. A subtle loading indicator (e.g., an animated 1 px `#000000` underline on the header row) is shown. No spinner or circular animation is used.
- [ ] A clear button (`×` glyph, right-aligned inside the search field) appears only when the field contains text. Clicking it clears the field and dispatches a new unfiltered query for the current sidebar context.
- [ ] Clearing the search field (via the clear button or keyboard select-all-and-delete) triggers the 150 ms debounce and then dispatches a query returning the full current sidebar context with no text filter.
- [ ] The scope operators `bpm:`, `key:`, `rating:`, `title:`, `artist:`, `album:` are parsed client-side before SQL dispatch. Unrecognized prefixes are treated as free-text terms and searched across the FTS5 index covering title, artist, and album.
- [ ] A `bpm:VALUE` token matches tracks with stored BPM within ±0.5 of VALUE. A `bpm:LOW-HIGH` token matches tracks with BPM between LOW and HIGH inclusive.

### 1.4.3. KEY MATCH and BPM MATCH Toggles

- [ ] The KEY MATCH button is rendered with an all-caps label "KEY MATCH" in `spaceGrotesk` bold. Inactive state: `#f9f9f9` background, `#000000` text. Active state: `#000000` background, `#f9f9f9` text. The state change is instantaneous with no transition or animation.
- [ ] The BPM MATCH button follows the same binary-inversion rule with the label "BPM MATCH".
- [ ] Activating BPM MATCH immediately re-dispatches the current query with the deck-aware BPM filter applied using the current BPM VISION window. Deactivating it immediately re-dispatches the query without the filter.
- [ ] Activating KEY MATCH immediately re-dispatches the current query with the deck-aware key filter applied. Deactivating it immediately re-dispatches without the filter.
- [ ] If no deck has a track loaded when BPM MATCH or KEY MATCH is activated, the toggle becomes active visually but the re-dispatched query carries no additional BPM or key constraint. A tooltip on the active button reads "No track loaded on any deck."
- [ ] While BPM MATCH is active, any change to a deck's `speedMultiplier` or `bpm` property in the ValueTree triggers a new debounced query (150 ms) with the updated effective tempo. The debounce ensures rapid speed-fader drags do not cause a query storm.

### 1.4.4. BPM VISION Input

- [ ] The BPM VISION input is a numeric field displayed with a "±" prefix label and a "BPM" suffix label. It is rendered immediately to the right of the BPM MATCH button.
- [ ] The default value is 6. The accepted input range is integers from 1 to 50 inclusive. Non-integer or out-of-range input causes the field to revert to the last valid value on commit.
- [ ] On commit (Enter key or focus-out), if BPM MATCH is currently active, the query is immediately re-dispatched with the updated tolerance window.
- [ ] When BPM MATCH is inactive, the BPM VISION input is visually disabled: its label and value are rendered at 50% opacity and pointer events are ignored.
- [ ] Below a total window width of 900 px, the BPM VISION input is hidden. Its last committed value is preserved in memory and applied when the window widens past 900 px again.

### 1.4.5. Sidebar Navigation

- [ ] The `SidebarMolecule` renders three top-level `SidebarItemAtom` nodes with all-caps `spaceGrotesk` bold labels: "COLLECTION", "FOLDERS", and "PLAYLISTS".
- [ ] The active sidebar item uses inverted colors: `#000000` background and `#f9f9f9` text. All other items use `#f9f9f9` background and `#000000` text. State change is instantaneous with no animation.
- [ ] Clicking "COLLECTION" dispatches a query returning all tracks from `library_tracks` ordered by date added descending.
- [ ] Clicking "FOLDERS" expands a tree of watched folder paths inline within the sidebar. Each folder node is indented one level and displays only the folder's base name. Clicking a folder node dispatches a query filtering by that folder path prefix.
- [ ] Clicking "PLAYLISTS" expands a list of playlist nodes. Clicking a playlist node dispatches a query returning tracks for that playlist ordered by `playlist_tracks.position`. The History and Preparation nodes appear with fixed names "History" and "Preparation" and cannot be renamed or deleted.
- [ ] If a query is already in flight when the sidebar selection changes, the in-flight result is discarded on arrival and a new query for the newly selected context is dispatched immediately.

### 1.4.6. Track Table and Virtual Rendering

- [ ] The `TrackTableMolecule` is backed by `juce::TableListBox` in virtual rendering mode. `getNumRows()` returns the count of rows in the current result buffer. `paintCell()` reads only from the result buffer; it does not invoke any database, file I/O, or network operation.
- [ ] The result buffer is a `std::vector<LibraryTrackRow>` swapped on the JUCE Message Thread after each query completes. `paintCell()` accesses this buffer without acquiring any lock.
- [ ] The table renders the following columns in this left-to-right order: indicator (fixed 24 px width), title, artist, BPM, key, duration, rating, played.
- [ ] Row height is uniform at 24 px for all rows in all states.
- [ ] Rows alternate between `#f9f9f9` (odd-indexed rows) and `#f3f3f4` (even-indexed rows). No solid horizontal divider line is drawn between rows.
- [ ] The row currently playing on any deck renders with fully inverted colors: `#000000` background, `#f9f9f9` text across all cells.
- [ ] The indicator column displays a `[>]` pixel-art glyph for the currently playing row. For rows with `is_missing = 1`, it displays a `!` pixel-art glyph. The playing indicator takes visual precedence over the missing indicator when both conditions are true.
- [ ] All row text uses `spaceGrotesk` Body-MD at 0.875 rem equivalent.
- [ ] The column header row background is `#e2e2e2` (surface-container-highest). Header labels are all-caps `spaceGrotesk` bold.
- [ ] The rating column displays exactly 5 square pixel-art glyphs per row: filled squares for awarded stars and empty squares for unawarded stars. Clicking a glyph sets the track's rating to the clicked position (1–5) and persists the value to `library_tracks.rating` immediately via `TrackDatabase`.
- [ ] The duration column displays `MM:SS` for durations under 60 minutes and `HH:MM:SS` for longer tracks.
- [ ] The BPM column displays the analyzed BPM with two decimal places (e.g., "128.00"). Unanalyzed tracks display a dash (`-`).
- [ ] The key column displays Camelot notation (e.g., "8A", "11B"). Unanalyzed tracks display a dash (`-`).
- [ ] The played column displays a filled square pixel-art glyph when the track's `play_count` has been incremented at least once in the current application session, and is empty otherwise.

### 1.4.7. Column Sorting

- [ ] Clicking a sortable column header dispatches a new query with an `ORDER BY` clause for that column ascending. A downward-pointing pixel-art triangle indicator appears in the active sort column header.
- [ ] Clicking the same column header a second time reverses the sort direction to descending. The indicator changes to an upward-pointing triangle.
- [ ] Clicking a different sortable column header clears the previous sort and applies ascending sort on the new column.
- [ ] The indicator and played columns are not sortable. Clicking their headers has no effect and shows no sort indicator.
- [ ] If a query is in flight when a column header is clicked, the new sort preference is stored and applied to the next query dispatched after the in-flight result is discarded.
- [ ] The active sort column index and direction are persisted to the `juce::ValueTree` and restored on next application launch.
- [ ] Column widths are resizable by dragging the divider between column headers. Each column's width is persisted to the `juce::ValueTree` and restored on next launch. The minimum width for any column is 40 px.

### 1.4.8. Track Loading

- [ ] Double-clicking a track row writes the track's `file_path` and `track_id` as properties into the `juce::ValueTree` subtree of the currently focused deck. No function in `Source/Features/Deck/` or `Source/Features/AudioEngine/` is called directly.
- [ ] The right-click context menu on a track row includes "Load to Deck A", "Load to Deck B" (and further deck entries for any additional active decks), "Add to Playlist", "Analyze Track", "Separate Stems", and "Remove from Library". The menu has zero `border-radius` and a 2 px offset dithered shadow with a 50% checkerboard of `#000000` and zero blur.
- [ ] Each "Load to Deck X" menu item writes the track's `file_path` and `track_id` into the corresponding deck's `juce::ValueTree` subtree.
- [ ] Dragging a track row onto a deck drop zone initiates a `juce::DragAndDropContainer` transfer. The drag description string contains the track's `track_id`. The drop target reads `track_id` and resolves the file path through `TrackDatabase`.
- [ ] Attempting to load a track with `is_missing = 1` (via double-click, context menu, or drag-and-drop) opens a modal dialog with "Relocate File" and "Cancel" options before any ValueTree write occurs. "Relocate File" opens a `juce::FileChooser` filtered to supported audio formats. A successful selection updates `library_tracks.file_path` in the database and then proceeds with the load. "Cancel" dismisses the dialog without any load or ValueTree modification.
- [ ] Multi-select is supported via Shift+Click (contiguous range) and Cmd+Click (additive). A right-click on a multi-selection disables all "Load to Deck" items and enables "Analyze Track(s)" and "Add to Playlist."

### 1.4.9. Empty and Zero-Result States

- [ ] When the result buffer contains zero rows because a search or filter returned no matches, the table area renders the centered text "No tracks found." in `spaceGrotesk` Body-MD, `#000000` on `#f9f9f9`. No image, icon, or secondary button is shown.
- [ ] When the library has never been scanned (zero records in `library_tracks`), the table area renders "No tracks in library. Add a Music Folder to get started." in the same style.
- [ ] The empty-state messages are always vertically and horizontally centered within the table area regardless of window size.

### 1.4.10. Keyboard Navigation

- [ ] Arrow Down and Arrow Up move the row selection one position at a time within the track table. The table scrolls to keep the selected row in view.
- [ ] Enter while a row is selected loads the selected track into the focused deck, identical in behavior to a double-click on that row.
- [ ] `Cmd+F` focuses the `SearchBarAtom` from anywhere within `LibraryComponent`.
- [ ] Tab moves focus in order: `SearchBarAtom` → KEY MATCH button → BPM MATCH button → BPM VISION input → `SidebarMolecule` → `TrackTableMolecule`. Shift+Tab reverses this cycle.
- [ ] Pressing Escape while the `SearchBarAtom` has focus and contains text clears the field. Pressing Escape again when the field is already empty moves focus to the `TrackTableMolecule`.

### 1.4.11. Module Boundary and Architecture

- [ ] No file under `Source/Features/Library/` contains a `#include` directive pointing to any header under `Source/Features/Deck/`, `Source/Features/AudioEngine/`, or any other Feature directory.
- [ ] All deck state consumed by `LibraryComponent` — effective BPM, `speedMultiplier`, active key, loaded track identifier — is read exclusively from the root `juce::ValueTree` via `valueTreePropertyChanged()` listener callbacks.
- [ ] `LibraryComponent` is constructed with exactly two external dependencies: a root `juce::ValueTree&` and a `TrackDatabase&`. No other constructor parameters are accepted.
- [ ] The component hierarchy follows Atomic Design: atoms (`SearchBarAtom`, `SidebarItemAtom`, `RatingAtom`) compose into molecules (`FilterBarMolecule`, `SidebarMolecule`, `TrackTableMolecule`), which assemble into the `LibraryComponent` organism.
- [ ] The UI responds to deck state changes exclusively through JUCE Listener callbacks implementing the Observer Pattern. No polling timer or repeated `getProperty()` call in a paint or timer callback is used to detect deck state changes.