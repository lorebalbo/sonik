---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0033
---

# 1. PRD-0036: Playlist Management

## 1.1. Problem

A professional DJ needs to organize tracks before and during a live performance. Without a playlist system, there is no way to pre-sequence a set, no persistent record of which tracks were played in a previous session, and no mechanism to stage a shortlist of candidate tracks on the fly. Every time the application is opened, the DJ must rediscover and re-sort music from the full collection, which is impractical for a live environment and incompatible with professional DJ workflows. The absence of an auto-generated play history also makes post-session review and set reconstruction impossible.

## 1.2. Objective

The DJ is able to create, rename, and delete named playlists, add and remove tracks, and reorder them by drag-and-drop. The system automatically records every track play event in a persistent, read-only History playlist, allowing the DJ to review any past session. A session-scoped Preparation List provides an in-memory scratch-pad for staging candidate tracks without committing to a permanent playlist. All playlists are visible in the sidebar under a collapsible Playlists section and are immediately available for loading tracks to decks. The system enforces name uniqueness, prevents mutation of system-managed playlists, and cleans up orphaned playlist references whenever a track is removed from the library.

## 1.3. User Flow

### 1.3.1. Creating a Normal Playlist

1. DJ right-clicks the "Playlists" section header in the left sidebar.
2. Selects "New Playlist" from the context menu.
3. An inline text input appears in the sidebar immediately below the header.
4. DJ types a name and presses Enter.
5. If the name is unique (case-insensitive match against all existing playlist names), a new playlist node appears in the sidebar with a track count badge of `0` and a `playlists` row is written to SQLite on the LibraryQueryThread.
6. If the name already exists, the inline field is highlighted with an error indicator, an error label reads "A playlist with this name already exists", and no row is written.
7. Pressing Escape at any point cancels creation with no side effects.

### 1.3.2. Renaming a Normal Playlist

1. DJ double-clicks the playlist node in the sidebar, or right-clicks and selects "Rename".
2. The playlist label becomes an inline editable text field pre-filled with the current name.
3. DJ edits the name and presses Enter (or clicks away to confirm).
4. If the new name is unique, the `name` column of the `playlists` row is updated on the LibraryQueryThread and the sidebar label refreshes.
5. If the new name is a duplicate (case-insensitive), the rename is rejected and the previous name is restored without any SQLite write.
6. System playlists (History, Preparation List) do not expose the rename action and double-clicking their sidebar nodes has no effect.

### 1.3.3. Deleting a Normal Playlist

1. DJ right-clicks a normal playlist node and selects "Delete Playlist".
2. A modal confirmation dialog appears: "Delete '[Playlist Name]'? Tracks in your library will not be affected."
3. DJ clicks "Delete" to confirm. The `playlists` row and all associated `playlist_tracks` rows are deleted in a single SQLite transaction on the LibraryQueryThread.
4. The sidebar node is removed and track count badges for other playlists are unaffected.
5. All `library_tracks` rows for tracks that were in the deleted playlist remain intact.

### 1.3.4. Adding Tracks to a Playlist

1. DJ selects one or more rows in the library track table.
2. Right-clicks and chooses "Add to Playlist" from the context menu, then selects a playlist name from the submenu; or drags the selection onto a playlist node in the sidebar.
3. Each selected track is appended at the end of the target playlist with `position` = max existing position + 1.
4. If a track is already present in the playlist, it is added again as a second entry. No warning is shown; duplicates are permitted by design.
5. The track count badge for the target playlist updates immediately.

### 1.3.5. Removing Tracks from a Playlist

1. With a normal playlist selected in the sidebar, the track table shows only the tracks belonging to that playlist.
2. DJ selects one or more rows, right-clicks, and chooses "Remove from Playlist".
3. The corresponding `playlist_tracks` rows are deleted on the LibraryQueryThread.
4. Remaining rows for the same playlist are renumbered sequentially (1, 2, 3, …) in the same transaction.
5. The table and the track count badge update immediately.

### 1.3.6. Reordering Tracks Within a Playlist

1. With a normal playlist selected, the DJ drags a row in the track table to a new position.
2. A drop indicator shows the insertion point between rows.
3. On drop, the `position` values of the moved row and all displaced rows are updated atomically on the LibraryQueryThread.
4. The table repaints immediately using the updated in-memory result buffer; no query round-trip is required before the repaint.

### 1.3.7. History Playlist

1. A `playlists` row with `type = history` and `name = "History"` is created during the initial schema migration and is always present.
2. Each time a deck's transport transitions to the playing state (detected via `IDs::isPlaying` on the deck ValueTree subtree), a `playlist_tracks` row is inserted for the History playlist with a `played_at` timestamp stored as ISO-8601 UTC.
3. Loading a track to a deck without pressing play does not create a History entry.
4. The History playlist view displays entries ordered by `played_at` descending (most recent at top).
5. The list is capped at 500 entries. Before each new insertion, if the current count equals 500, the oldest entry (smallest `played_at`) is deleted in the same transaction.
6. No user action — rename, delete, add, remove, or reorder — is exposed for the History playlist.

### 1.3.8. Preparation List

1. A node labeled "Preparation List" is always present in the sidebar under Playlists and is not backed by a `playlists` SQLite row.
2. DJ adds tracks to it via the same right-click "Add to Playlist" submenu or by dragging tracks onto its sidebar node.
3. All Preparation List state lives in a `std::vector<int64_t>` (track IDs) in memory on the Message Thread; no `playlist_tracks` rows are written.
4. If the Preparation List is non-empty when the application attempts to quit, a dialog appears: "Your Preparation List has [N] track(s) and will be lost. Export to a playlist before quitting?" with three buttons: "Export to Playlist", "Discard", and "Cancel".
5. "Export to Playlist" opens a name-input dialog. On confirmation of a valid unique name, a new normal playlist is created in SQLite with those tracks in their current order, then the quit proceeds.
6. "Discard" clears the list and proceeds with quit.
7. "Cancel" aborts the quit; the application remains open and the Preparation List is unchanged.
8. On the next application launch, the Preparation List is always empty.

## 1.4. Acceptance Criteria

### 1.4.1. Create Playlist

- AC-01: Right-clicking the Playlists section header and selecting "New Playlist" renders an inline text input in the sidebar with keyboard focus.
- AC-02: Submitting a name that is unique (case-insensitive comparison against all existing `playlists.name` values) inserts a `playlists` row with `type = normal` on the LibraryQueryThread and adds a sidebar node with a track count badge of `0`.
- AC-03: Submitting a name that matches an existing playlist name (case-insensitive) is rejected: no SQLite row is inserted, the inline field is visually highlighted with an error state, and an error label "A playlist with this name already exists" is shown.
- AC-04: Pressing Escape during name input cancels creation with no SQLite writes and no new sidebar node.
- AC-05: Submitting a name that is empty or contains only whitespace characters is treated identically to pressing Escape — no playlist is created.

### 1.4.2. Rename Playlist

- AC-06: Double-clicking a normal playlist node, or selecting "Rename" from its right-click context menu, opens an inline text field pre-filled with the current playlist name.
- AC-07: Confirming a unique new name executes an UPDATE on the `playlists` row on the LibraryQueryThread and refreshes the sidebar label to the new name.
- AC-08: If the new name is a duplicate (case-insensitive) of any existing playlist, the rename is rejected, no SQLite UPDATE is executed, and the previous name is restored in the sidebar.
- AC-09: Pressing Escape during inline rename restores the previous name without any SQLite write.
- AC-10: The History and Preparation List nodes do not show "Rename" in their right-click menus. Double-clicking these nodes does not open an inline edit field.

### 1.4.3. Delete Playlist

- AC-11: Right-clicking a normal playlist node exposes "Delete Playlist" in the context menu.
- AC-12: Selecting "Delete Playlist" displays a modal confirmation dialog with the message "Delete '[Playlist Name]'? Tracks in your library will not be affected." and two buttons: "Delete" and "Cancel".
- AC-13: Clicking "Delete" in the confirmation dialog removes the `playlists` row and all associated `playlist_tracks` rows in a single atomic SQLite transaction on the LibraryQueryThread. The sidebar node is removed immediately after the transaction commits.
- AC-14: All `library_tracks` rows for tracks that were referenced by the deleted playlist remain in the database and in the collection view.
- AC-15: The History playlist and the Preparation List do not expose "Delete Playlist" in their right-click context menus and cannot be deleted by any user-initiated action.

### 1.4.4. Add Tracks to a Playlist

- AC-16: The right-click context menu on any track row in the library table includes an "Add to Playlist" item with a cascading submenu listing all normal playlist names. The History playlist and Preparation List are listed in this submenu as valid targets.
- AC-17: Selecting a normal playlist from the submenu appends the selected tracks to that playlist. Each track is inserted into `playlist_tracks` with `position` = current max position + 1, incrementing per track in selection order.
- AC-18: Dragging a track (or multi-selection) from the library table onto a playlist node in the sidebar performs the same append operation as AC-17.
- AC-19: If a track being added is already present in the target playlist, it is inserted as a second `playlist_tracks` row. No warning or rejection occurs; duplicates are permitted by design and documented.
- AC-20: After any add operation, the track count badge on the target playlist node in the sidebar reflects the new count.

### 1.4.5. Remove Tracks from a Playlist

- AC-21: When a normal playlist is the active sidebar selection, right-clicking a row in the track table shows "Remove from Playlist" in the context menu.
- AC-22: Selecting "Remove from Playlist" deletes the `playlist_tracks` row(s) for the selected tracks on the LibraryQueryThread and renumbers the `position` values of all remaining rows in the same playlist sequentially starting from 1, in a single transaction.
- AC-23: "Remove from Playlist" is not present in the context menu when the active sidebar selection is the Collection view or the History playlist.
- AC-24: The track count badge for the affected playlist updates to the new count after the removal.

### 1.4.6. Drag-and-Drop Reordering

- AC-25: In the track table when a normal playlist is selected, rows expose a drag handle. Initiating a drag shows a drop indicator line between rows as the cursor moves.
- AC-26: Releasing a dragged row at a new position triggers a batch UPDATE on the LibraryQueryThread that reassigns `position` values for the moved row and all rows whose positions shifted, executed atomically.
- AC-27: The in-memory result buffer is updated on the Message Thread immediately after the drop event, and the table repaints without waiting for a new database query.
- AC-28: Drag-and-drop reordering is disabled (rows are not draggable) when the active sidebar selection is the History playlist or the Collection view.

### 1.4.7. History Playlist

- AC-29: A `playlists` row with `name = "History"` and `type = history` is created during the schema migration defined in PRD-0030 if it does not already exist. This row is created on the Message Thread at application startup before the UI is shown.
- AC-30: Each time a deck's `IDs::isPlaying` ValueTree property transitions from `false` to `true`, a `playlist_tracks` row is inserted for the History playlist with a `played_at` column containing the current UTC timestamp in ISO-8601 format. This insert is dispatched to the LibraryQueryThread.
- AC-31: A track being loaded to a deck without triggering playback does not generate a History entry.
- AC-32: The History playlist view orders entries by `played_at` descending so the most recently played track appears at row 0.
- AC-33: The History playlist is capped at 500 entries. Each play-event insert is wrapped in a transaction that first deletes the oldest entry (by `played_at` ascending) if `COUNT(*) >= 500`, then inserts the new row.
- AC-34: The History playlist node in the sidebar does not expose any of the following actions in its context menu or via keyboard shortcuts: rename, delete, "Add to Playlist", "Remove from Playlist", or row reordering.
- AC-35: The History playlist is always shown at the top of the Playlists section in the sidebar, above all normal playlists and below the Preparation List.

### 1.4.8. Preparation List

- AC-36: A node labeled "Preparation List" is always present at the top of the Playlists section in the sidebar, above the History playlist. It is not backed by any row in the `playlists` table.
- AC-37: Tracks can be added to the Preparation List via the "Add to Playlist" submenu and via drag-and-drop onto its sidebar node, using the same interaction as normal playlists.
- AC-38: All Preparation List state is stored in a `std::vector<int64_t>` (track IDs with their order) held on the JUCE Message Thread. No write to `playlist_tracks` is ever performed for the Preparation List.
- AC-39: The track count badge on the Preparation List sidebar node reflects the current in-memory count and updates after every add or remove operation.
- AC-40: When the application initiates a quit sequence and the Preparation List contains one or more tracks, a modal dialog is shown with the text "Your Preparation List has [N] track(s) and will be lost. Export to a playlist before quitting?" and three buttons: "Export to Playlist", "Discard", and "Cancel".
- AC-41: Selecting "Export to Playlist" in the quit dialog opens a name-input field. If the entered name is valid and unique, a new `playlists` row of `type = normal` is written and its tracks are inserted into `playlist_tracks` on the LibraryQueryThread; once the transaction commits, the quit proceeds.
- AC-42: Selecting "Discard" in the quit dialog clears the in-memory vector and allows the quit sequence to proceed without any SQLite writes.
- AC-43: Selecting "Cancel" in the quit dialog dismisses it and cancels the quit; the application remains running with the Preparation List unchanged.
- AC-44: On every application launch, the Preparation List in-memory vector is initialized empty regardless of any previous session state.

### 1.4.9. Sidebar Display

- AC-45: Each playlist node in the sidebar displays the playlist name in the primary label and the current track count in a secondary badge. The count is derived from `COUNT(*)` on `playlist_tracks` for the given playlist and is cached in-memory; it updates after every add, remove, or delete operation via `juce::MessageManager::callAsync()`.
- AC-46: The Playlists section header in the sidebar is clickable and toggles the expanded or collapsed state of the playlist node list.
- AC-47: Clicking a playlist node highlights it using the inverted selection style defined in DESIGN.md and populates the library track table with the tracks of that playlist in their stored `position` order.
- AC-48: The Preparation List and History playlist nodes are always rendered above all normal playlists in the sidebar, in a fixed order of: Preparation List first, History second, then normal playlists sorted by `created_at` ascending.

### 1.4.10. Orphaned Track Cleanup

- AC-49: When a `library_tracks` row is deleted (by "Remove from Library" or by missing-file cleanup as defined in PRD-0039), all `playlist_tracks` rows referencing that `track_id` are deleted in the same SQLite transaction, across all playlists.
- AC-50: After orphaned-entry cleanup, the track count badges for all affected playlist nodes in the sidebar are updated on the Message Thread via `juce::MessageManager::callAsync()`.
- AC-51: If the deleted track was the only entry in a normal playlist, the playlist itself is retained with a count of `0`; it is not auto-deleted.

### 1.4.11. Thread Safety and SQLite Access

- AC-52: All `INSERT`, `UPDATE`, and `DELETE` operations on the `playlists` and `playlist_tracks` tables are executed exclusively on the LibraryQueryThread (or, for schema setup, on the Message Thread at startup before the UI renders). No SQLite write is triggered from `paint()`, `resized()`, or any audio-thread callback.
- AC-53: The Preparation List in-memory `std::vector` is read and mutated exclusively on the JUCE Message Thread. No other thread accesses or modifies this vector.
- AC-54: In the event of a concurrent rename where two rapid rename operations target the same playlist (race condition), the LibraryQueryThread processes requests sequentially via its existing job queue. The second rename sees the updated name from the first and performs a standard duplicate-check before committing.