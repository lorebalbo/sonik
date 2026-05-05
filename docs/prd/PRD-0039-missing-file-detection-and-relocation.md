---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0031
  - PRD-0033
---

# 1. PRD-0039: Missing File Detection and Relocation

## 1.1. Problem

Professional DJs regularly reorganize music folders, rename tracks, archive files to external drives, or migrate entire libraries between computers. When any of these operations move or rename a file that is already catalogued in the Sonik library, the database record becomes an orphan: its `file_path` column points to a location that no longer exists on disk. The application currently has no mechanism to detect this state proactively. The DJ discovers the problem only at the worst possible moment — when attempting to load a track to a deck during a live performance. The application either fails silently or throws a non-descriptive error, with no guidance on how to recover. For a professional tool used in high-stakes live settings, silent failures of this kind are unacceptable.

The problem compounds at scale. A library spanning tens of thousands of tracks distributed across multiple external drives — some of which are intermittently connected — means it is practically impossible for the DJ to know at a glance how many tracks are currently unreachable. Without a proactive detection pass, without a visual indicator on affected rows, and without a structured recovery flow, the DJ is left to discover broken references one track at a time under time pressure.

## 1.2. Objective

The system is able to detect all missing files automatically on every application startup, mark their rows visually in the library UI, and provide the DJ with actionable recovery paths — either relocating the track to a new file path or permanently removing the record from the library. Detection runs as a background pass so it never blocks the UI or delays startup. Missing rows are updated progressively as the pass runs, giving the DJ immediate feedback without waiting for the full scan to complete. The same pass that detects newly missing files also restores records for files that have reappeared since the last launch, resetting their `is_missing` flag to `0`.

On load-to-deck, a synchronous existence re-check ensures that a file which became missing after the startup pass is caught before audio dispatch. A right-click context menu on any missing row provides the same relocation and removal actions as the on-load dialog, enabling bulk recovery workflows. A sidebar count badge and a "Missing Only" filter toggle allow the DJ to assess and resolve the full extent of missing files in one focused session. All analysis data — BPM, key, waveform cache — is preserved across relocation; only `file_path` changes in the database.

## 1.3. User Flow

### 1.3.1. Startup Background Pass

1. Sonik launches and the watch-folder scanner completes its ingestion pass (adding new files, updating changed files) as defined in PRD-0031.
2. Immediately after the watch-folder scan completes, a second background task is dispatched on the LibraryQueryThread. It queries all `library_tracks` rows — both `is_missing = 0` and `is_missing = 1`.
3. For each record, the background task calls `juce::File(file_path).existsAsFile()`.
4. If the file is not found and the record has `is_missing = 0`, the task executes `UPDATE library_tracks SET is_missing = 1 WHERE id = ?` and posts a notification to the Message Thread to repaint that row.
5. If the file is found and the record has `is_missing = 1` (the file reappeared since the last launch), the task executes `UPDATE library_tracks SET is_missing = 0 WHERE id = ?` and posts a notification to the Message Thread to repaint that row to its normal state.
6. Records that are consistent with their `is_missing` flag are left unchanged.
7. The Message Thread receives each notification as it arrives and repaints the corresponding row in the library track table immediately, without waiting for the full pass to finish. Rows newly marked missing receive the "Glitch" dithered background pattern defined in DESIGN.md (random monochrome noise applied to the row background, no color change, strictly `#000000` on `#f9f9f9`). Rows restored to non-missing revert to standard zebra-stripe rendering.
8. Once the full pass is complete, the sidebar missing file count badge displayed next to the "Collection" label is set to the exact current count of all `library_tracks` rows where `is_missing = 1`.
9. If the application is closed before the background pass finishes, all SQLite writes already committed are durable. On the next launch, the pass restarts from the full record set — the check is idempotent and safe to resume.

### 1.3.2. On Load-to-Deck Synchronous Check

1. The DJ double-clicks a track row or drags it onto a deck slot.
2. Before dispatching to `AudioFileLoader`, the system performs a synchronous `juce::File(file_path).existsAsFile()` check on the Message Thread.
3. If the check returns `false`, audio dispatch is blocked entirely and the Missing File Dialog opens immediately (see section 1.3.3).
4. If the check returns `true` for a record where `is_missing = 1` (the file reappeared during the session, e.g., an external drive was reconnected), the system executes `UPDATE library_tracks SET is_missing = 0 WHERE id = ?` on the LibraryQueryThread, repaints the row to its normal state, decrements the sidebar badge count, and proceeds with audio dispatch — with no dialog shown to the DJ.
5. If the check returns `true` and `is_missing = 0`, the system proceeds directly to audio dispatch with no additional steps.

### 1.3.3. Missing File Dialog

1. A modal dialog appears with zero `border-radius`, strictly monochrome styling, and a dithered shadow (2px offset, 50% checkerboard pattern of `#000000`, zero blur) as specified in the DESIGN.md Elevation section.
2. The dialog header reads "File Not Found".
3. A single line below the header displays the track title and the full broken `file_path`.
4. Two primary action buttons are presented: "Relocate" and "Remove from Library".
5. A "Cancel" text link is present at the bottom of the dialog. Pressing it, or pressing Escape, closes the dialog with no changes to any database record; `is_missing` remains `1` and `file_path` is unchanged.

### 1.3.4. Relocate Flow

1. The DJ clicks "Relocate" in the Missing File Dialog (or selects "Relocate File…" from the right-click context menu on a missing row — see section 1.3.6).
2. A `juce::FileChooser` opens titled "Choose replacement file". No file format filter is applied; format validation is delegated to `AudioFileLoader` at the point of audio dispatch.
3. The DJ selects a file and confirms the chooser.
4. Before writing, the system checks whether the selected canonical absolute path already exists in `library_tracks.file_path` for any record with a different `id` (deduplication check):
   - If a duplicate is found, an inline error appears inside the dialog: "This file is already in your library." The FileChooser is closed. No SQLite write is performed. The DJ may click "Relocate" again to choose a different file.
   - If no duplicate is found, the system executes `UPDATE library_tracks SET file_path = ?, is_missing = 0 WHERE id = ?` on the LibraryQueryThread.
5. On successful write: the dialog closes, the row repaints to its normal state, the sidebar badge count is decremented, all previously computed analysis data (BPM, key, waveform cache) is retained in the database — only `file_path` was changed. Audio dispatch proceeds immediately with the new path.
6. If the DJ dismisses the `FileChooser` without selecting a file, the dialog remains open and no database write is performed.

### 1.3.5. Remove from Library Flow

1. The DJ clicks "Remove from Library" in the Missing File Dialog (or selects the equivalent item from the right-click context menu on a missing row — see section 1.3.6).
2. A nested confirmation step appears inside the dialog: "This will also remove the track from all playlists. This action cannot be undone."
3. Two buttons are shown: "Confirm Remove" and "Cancel".
4. Clicking "Cancel" on the confirmation step returns the DJ to the main Missing File Dialog with no writes performed.
5. Clicking "Confirm Remove" dispatches a single SQLite transaction on the LibraryQueryThread that atomically executes:
   - `DELETE FROM playlist_tracks WHERE track_id = ?`
   - `DELETE FROM library_tracks WHERE id = ?`
6. On successful commit: the row is removed from the track table, all playlist track-count badges that referenced the track are updated on the Message Thread, and the sidebar missing file count badge is decremented.
7. If the deleted track had entries in the History playlist, those `playlist_tracks` rows are deleted in the same atomic transaction and the History list view is updated on the Message Thread.

### 1.3.6. Right-Click Relocation on a Missing Row

1. The DJ right-clicks any row rendered with the "Glitch" dithered background (a row where `is_missing = 1`).
2. The context menu includes two additional items at the top of the standard action list: "Relocate File…" and "Remove from Library".
3. Selecting "Relocate File…" opens the `juce::FileChooser` directly, bypassing the Missing File Dialog. The flow from that point is identical to section 1.3.4, steps 2 through 6.
4. Selecting "Remove from Library" shows the nested confirmation step directly, bypassing the Missing File Dialog. The flow from that point is identical to section 1.3.5, steps 2 through 7.

### 1.3.7. Show Missing Only Filter

1. A toggle labeled "Missing Only" is available in the library filter bar, alongside other filter controls.
2. When activated, the library query is scoped to `WHERE is_missing = 1`, displaying only records with unresolved missing files.
3. The toggle renders in its active state using the standard binary inversion from DESIGN.md (white text on black background when active, black text on white background when inactive). Zero border-radius.
4. The DJ can select all visible missing rows and use right-click "Relocate File…" or "Remove from Library" to perform bulk missing-file resolution in a single focused session.
5. Deactivating the toggle removes the `is_missing = 1` constraint from the query and restores all other previously active filters to their prior state.

## 1.4. Acceptance Criteria

### 1.4.1. Startup Background Pass

- AC-01: On every application launch, after the watch-folder ingestion pass completes, the system dispatches a background task on the LibraryQueryThread that calls `juce::File(file_path).existsAsFile()` for every record in `library_tracks`.
- AC-02: For each record where `is_missing = 0` and `existsAsFile()` returns `false`, the system executes `UPDATE library_tracks SET is_missing = 1 WHERE id = ?` and notifies the Message Thread to repaint that row.
- AC-03: For each record where `is_missing = 1` and `existsAsFile()` returns `true`, the system executes `UPDATE library_tracks SET is_missing = 0 WHERE id = ?` and notifies the Message Thread to repaint that row to its normal state.
- AC-04: Each missing row is repainted with the DESIGN.md "Glitch" dithered pattern (random monochrome noise on the row background, no color change, strictly `#000000` / `#f9f9f9` palette, zero `border-radius`).
- AC-05: Row repaints occur progressively as each affected record is discovered; the full track table does not need to wait for the pass to complete before individual rows are updated.
- AC-06: The background pass runs entirely on the LibraryQueryThread and does not block the JUCE Message Thread; the library UI remains fully interactive (scrollable, searchable, loadable) while the pass runs.
- AC-07: If the application is closed before the background pass completes, all SQLite writes already committed are durable; on the next launch the pass restarts from the full record set without data loss or corruption.
- AC-08: Once the full pass completes, the sidebar missing file count badge next to the "Collection" label is updated to the exact count of all `library_tracks` rows where `is_missing = 1`.

### 1.4.2. On Load-to-Deck Synchronous Check

- AC-09: When the DJ initiates a load-to-deck action (double-click or drag-and-drop), the system performs a synchronous `juce::File(file_path).existsAsFile()` check on the Message Thread before any call to `AudioFileLoader`.
- AC-10: If the synchronous check returns `false`, audio dispatch is blocked and the Missing File Dialog is shown immediately.
- AC-11: If the synchronous check returns `true` for a record where `is_missing = 1`, the system updates `library_tracks.is_missing` to `0` on the LibraryQueryThread, repaints the row to its normal state, decrements the sidebar badge count, and proceeds with audio dispatch — with no dialog shown.

### 1.4.3. Missing File Dialog

- AC-12: The Missing File Dialog has zero `border-radius`, is strictly monochrome, and renders a dithered shadow with a 2px offset, a 50% checkerboard pattern of `#000000`, and zero blur, conforming to DESIGN.md elevation rules.
- AC-13: The dialog displays the track title and the full broken file path as plain text.
- AC-14: Pressing "Cancel" or pressing Escape closes the dialog with no changes to any database record; `is_missing` remains `1` and `file_path` is unchanged.

### 1.4.4. Relocate Flow

- AC-15: Clicking "Relocate" (from the dialog) or "Relocate File…" (from the right-click context menu) opens a `juce::FileChooser` with no file format restriction applied.
- AC-16: If the selected file's canonical absolute path already exists in `library_tracks.file_path` for a record with a different `id`, the relocation is rejected, an inline error "This file is already in your library." is shown, and no SQLite write is performed.
- AC-17: If the selected path is not a duplicate, the system executes `UPDATE library_tracks SET file_path = ?, is_missing = 0 WHERE id = ?` on the LibraryQueryThread; the dialog closes, the row repaints to its normal state, and the sidebar badge count is decremented.
- AC-18: All analysis data for the track — BPM, key, waveform cache — is retained after a successful relocation; only `file_path` changes in the database, with no analysis columns modified.
- AC-19: If the DJ dismisses the `FileChooser` without selecting a file, the dialog remains open and no database write is performed.
- AC-20: After a successful relocation, audio dispatch to `AudioFileLoader` proceeds immediately using the updated `file_path`.

### 1.4.5. Remove from Library

- AC-21: Clicking "Remove from Library" (from the dialog or the right-click context menu) presents a nested confirmation step stating the track will be removed from all playlists and that the action cannot be undone.
- AC-22: Clicking "Cancel" on the confirmation step returns to the Missing File Dialog (or dismisses the context menu flow) with no database writes performed.
- AC-23: On confirmation, a single SQLite transaction atomically executes `DELETE FROM playlist_tracks WHERE track_id = ?` followed by `DELETE FROM library_tracks WHERE id = ?`; both deletes commit together or neither does.
- AC-24: After the transaction commits, the track row is removed from the library track table, all playlist track-count badges that referenced the track are updated on the Message Thread, and the sidebar missing file count badge is decremented.
- AC-25: If the removed track had entries in the History playlist, those `playlist_tracks` rows are deleted in the same atomic transaction and the History list view updates on the Message Thread.

### 1.4.6. Right-Click Relocation

- AC-26: Right-clicking a row where `is_missing = 1` exposes "Relocate File…" and "Remove from Library" at the top of the context menu, in addition to all standard context menu items available for non-missing rows.
- AC-27: "Relocate File…" from the context menu follows the identical flow as AC-15 through AC-20, bypassing the Missing File Dialog.
- AC-28: "Remove from Library" from the context menu follows the identical confirmation and atomic delete flow as AC-21 through AC-25, bypassing the Missing File Dialog.

### 1.4.7. Show Missing Only Filter

- AC-29: Activating the "Missing Only" toggle amends the active library query to include `WHERE is_missing = 1` and updates the track table to display only those rows.
- AC-30: The "Missing Only" toggle renders in its active state with white text on a black background (binary inversion per DESIGN.md) and in its inactive state with black text on a white background. Zero `border-radius` in both states.
- AC-31: Deactivating the "Missing Only" toggle removes the `is_missing = 1` constraint and restores all other previously active filters to their prior state.

### 1.4.8. File Reappears on Next Launch

- AC-32: On the next application launch after an external drive is reconnected, the startup background pass detects records where `is_missing = 1` and `existsAsFile()` returns `true`, resets each such record to `is_missing = 0`, repaints the corresponding row to its normal state, and decrements the sidebar badge count accordingly.

### 1.4.9. Edge Cases

- AC-33: The deduplication check in the relocate flow uses the canonical absolute file path returned by the `juce::FileChooser` and compares it against `library_tracks.file_path` for all records with a different `id`; the comparison is case-insensitive on macOS and Windows and case-sensitive on Linux.
- AC-34: If a track is actively playing on a deck at the moment the startup background pass marks it as missing, the row visual indicator is updated to the "Glitch" dithered pattern but playback is not interrupted and the deck state is unaffected.
- AC-35: The startup background pass processes a library of 10,000 or more records without blocking the JUCE Message Thread for more than 16ms at any point; all file-existence checks and SQLite writes are confined to the LibraryQueryThread.