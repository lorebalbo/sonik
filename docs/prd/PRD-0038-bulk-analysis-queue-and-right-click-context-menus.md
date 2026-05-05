---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0033
---

# 1. PRD-0038: Bulk Analysis Queue and Right-Click Context Menus

## 1.1. Problem

DJ software collections routinely contain thousands of tracks, many of which arrive unanalyzed — lacking BPM, key, waveform data, or separated stems. Without a bulk analysis mechanism, the DJ must open each track individually to trigger analysis, which scales poorly and is incompatible with a professional pre-show preparation workflow.

Without right-click context menus, the library panel is read-only and non-interactive. Track loading, analysis, playlist management, and file maintenance all require navigating away from the main library view, breaking the flow of collection browsing.

Without CPU throttling on background analysis, a DJ who queues hundreds of tracks for analysis during a live performance risks saturating all CPU cores. The resulting starvation of the JUCE audio thread causes audible dropouts, which is unacceptable in a professional setting.

The combination of these three missing capabilities forces DJs into slow, error-prone workflows and makes the library module feel passive rather than a command center for collection management.

## 1.2. Objective

The user is able to right-click any track row (or a multi-selection of rows) in the library table and immediately access a contextual action menu. The menu offers track loading, analysis, stem separation, playlist management, rating, and file maintenance actions appropriate to the selection type and file state.

The system ensures that all analysis jobs dispatched from context menus are routed through a `LibraryAnalysisQueue` that enforces a hard ceiling of 2 concurrent analysis workers and 1 concurrent stem separation job, preventing CPU saturation during live playback.

The system provides real-time per-row progress feedback in a dedicated status column, reflecting the full job lifecycle from queued through in-progress to complete or failed. All status updates are delivered to the Message Thread via `juce::MessageManager::callAsync()`, ensuring the UI remains responsive and paint operations never block on background work.

Jobs are cooperatively cancellable at any point between pipeline stages via `shouldExit()`, and all queued work is gracefully cancelled when the application quits. The `LibraryAnalysisQueue` is owned by `SonikApplication` and injected as a dependency — it is never accessed via a singleton.

## 1.3. User Flow

### 1.3.1. Single Track Right-Click

The DJ right-clicks a single analyzed track row in the library table. A context menu appears with the following items:
- "Load to Deck 1"
- "Load to Deck 2"
- "Analyze Track" (greyed out if already fully analyzed; "Force Re-analyze" appears as an alternative)
- "Separate Stems" (greyed out if stems are already separated)
- "Add to Playlist..." (opens a submenu listing all existing playlists plus a "New Playlist..." option)
- "Rate..." (submenu with options 1 Star through 5 Stars)
- "Remove from Library"

The DJ selects "Load to Deck 1". The track is dispatched to `AudioFileLoader` for Deck 1. The context menu closes.

### 1.3.2. Single Missing File Right-Click

The DJ right-clicks a track row that has `is_missing = 1` (displayed with a warning indicator). The context menu shows only:
- "Relocate File..." (opens `juce::FileChooser` to repoint the path)
- "Remove from Library"

"Analyze Track", "Separate Stems", "Load to Deck X", and "Add to Playlist..." are absent because the file cannot be read.

### 1.3.3. Multi-Track Right-Click

The DJ selects 50 tracks using Shift-click and Cmd-click, then right-clicks. The context menu shows:
- "Analyze Tracks (50)" — queues all 50 as analysis jobs
- "Separate Stems (50)" — queues all 50 as stem separation jobs
- "Add to Playlist..." (submenu listing all playlists)
- "Rate..." (submenu; sets the same rating on all selected tracks)
- "Remove from Library (50)"

"Load to Deck X" is absent because loading multiple tracks to a single deck simultaneously is undefined. If all 50 selected tracks are already fully analyzed, "Analyze Tracks" is greyed out; "Force Re-analyze (50)" appears in its place.

### 1.3.4. Bulk Analysis Progress

After the DJ selects "Analyze Tracks (50)", all 50 jobs are added to the `LibraryAnalysisQueue`. The status column of each affected row immediately updates to "Queued". As workers pick up jobs (max 2 simultaneously), each active row transitions its status column through "Analyzing 0%" → "Analyzing N%" → "Complete" or "Failed". Rows not yet picked up remain at "Queued". The library table remains fully scrollable and interactive throughout.

### 1.3.5. Queue Cancellation on App Quit

The DJ initiates a bulk analysis of 500 tracks and then quits the application. During the shutdown sequence, `LibraryAnalysisQueue::cancelAllJobs()` is called. Each active `juce::ThreadPoolJob` returns `true` from `shouldExit()` at the next pipeline stage boundary. All queued jobs that have not started are removed from the pool without execution. The application exits cleanly without waiting for jobs to complete their current stage beyond the next `shouldExit()` check.

### 1.3.6. Already-Analyzed Track Handling

The DJ right-clicks a track that already has valid BPM, key, and waveform data. The "Analyze Track" item appears greyed out. A secondary item "Force Re-analyze" is visible and enabled. If the DJ selects "Force Re-analyze", a new analysis job is queued regardless of existing data, and the existing analysis results are overwritten upon job completion.

### 1.3.7. Stem Separation Failure

The DJ queues stem separation on a track encoded in an unsupported format. The `StemSeparationManager` reports failure. The row status column updates to "Stem Failed" via `callAsync()`. No crash occurs. The next job in the stem queue proceeds normally.

## 1.4. Acceptance Criteria

### 1.4.1. Right-Click Context Menu — Single Track Selection

1. Right-clicking a single non-missing track row opens a context menu containing exactly the following items: "Load to Deck 1", "Load to Deck 2", "Analyze Track" (or "Force Re-analyze" if already analyzed), "Separate Stems" (or greyed out if already separated), "Add to Playlist..." (with playlist submenu), "Rate..." (submenu with 1–5 Stars), and "Remove from Library".
2. The context menu renders with a 2-pixel dithered (checkerboard) drop shadow at a 2-pixel x/y offset, zero blur, and zero border-radius, conforming to the dithered shadow specification in `DESIGN.md`.
3. The context menu background and text use the strict monochrome palette: `#000000` text on `#f9f9f9` background (or inverted for the hovered item), with no border-radius on any item or the container.
4. Selecting "Load to Deck 1" dispatches the track to `AudioFileLoader` for Deck 1 and closes the menu; the deck display updates to reflect the loaded track.
5. Selecting "Load to Deck 2" dispatches the track to `AudioFileLoader` for Deck 2 and closes the menu.
6. Selecting "Analyze Track" on an unanalyzed track creates one `LibraryAnalysisQueue` job for that track and closes the menu; the row status column immediately shows "Queued".
7. "Analyze Track" is greyed out (not clickable) when the track already has complete BPM, key, and waveform analysis data stored in the database.
8. "Force Re-analyze" is enabled and clickable when the track is already fully analyzed; selecting it creates a new analysis job that will overwrite existing BPM, key, and waveform data upon completion.
9. Selecting "Separate Stems" on a track that has not yet had stems separated creates one stem separation job in the `LibraryAnalysisQueue` and closes the menu; the row status column immediately shows "Queued (Stems)".
10. "Separate Stems" is greyed out when stem separation data already exists for the track.

### 1.4.2. Right-Click Context Menu — Missing File

11. Right-clicking a row where `is_missing = 1` opens a restricted context menu containing only "Relocate File..." and "Remove from Library"; all other actions are absent.
12. Selecting "Relocate File..." opens a `juce::FileChooser` modal; upon the user confirming a valid audio file path, `file_path` is updated in the `library_tracks` record, `is_missing` is set to `0`, and the row repaints without the warning indicator.
13. Selecting "Remove from Library" on a missing file row removes the record from `library_tracks` and from all `playlist_tracks` entries; the row disappears from the table on the next repaint.

### 1.4.3. Right-Click Context Menu — Multi-Track Selection

14. Right-clicking a multi-track selection opens a context menu that contains "Analyze Tracks (N)", "Separate Stems (N)", "Add to Playlist...", "Rate...", and "Remove from Library (N)", where N is the count of selected tracks; "Load to Deck X" items are absent.
15. If every track in the multi-selection is already fully analyzed, "Analyze Tracks (N)" is greyed out and "Force Re-analyze (N)" is enabled in its place.
16. Selecting "Analyze Tracks (N)" enqueues one `LibraryAnalysisQueue` analysis job per selected track; each row's status column immediately shows "Queued".
17. Selecting "Separate Stems (N)" enqueues one stem separation job per selected track; each row's status column immediately shows "Queued (Stems)".
18. Selecting "Rate..." from a multi-selection submenu applies the chosen star rating to all selected tracks simultaneously, writing each rating to the database and repainting all affected rows.
19. Selecting "Remove from Library (N)" removes all N tracks from `library_tracks` and all associated `playlist_tracks` records in a single database transaction; all N rows disappear from the table on the next repaint.

### 1.4.4. LibraryAnalysisQueue Concurrency Contract

20. The `LibraryAnalysisQueue` never runs more than 2 analysis jobs (waveform + BPM + key via Essentia) concurrently at any time; a third analysis job submitted while 2 are active remains in the "Queued" state until a slot opens.
21. The `LibraryAnalysisQueue` never runs more than 1 stem separation job concurrently at any time; a second stem job submitted while one is active remains in the "Queued (Stems)" state until the active job completes or fails.
22. Analysis jobs and stem separation jobs occupy separate concurrency pools; 2 active analysis jobs do not prevent the 1 stem separation job from running.
23. Manually triggered jobs (from right-click context menus) are inserted at the front of their respective queues and are picked up before any background scan-triggered jobs.

### 1.4.5. Row Status Column Lifecycle

24. Each track row that has a job in the queue displays one of the following states in its status column: "Unanalyzed", "Queued", "Queued (Stems)", "Analyzing N%", "Separating Stems N%", "Complete", "Stem Complete", "Failed", or "Stem Failed"; no other string values appear in this column.
25. When an analysis job transitions from queued to active, the row status column updates to "Analyzing 0%" on the Message Thread via `juce::MessageManager::callAsync()`; subsequent progress updates increment the percentage as each Essentia pipeline stage completes.
26. When a stem separation job transitions from queued to active, the row status column updates to "Separating Stems 0%" on the Message Thread via `callAsync()`; subsequent updates increment the percentage.
27. Upon successful completion of an analysis job, the row status column updates to "Complete" via `callAsync()`, and the BPM, key, and waveform data written to the database become immediately available for display in their respective columns.
28. Upon failure of a stem separation job (e.g., unsupported format or `StemSeparationManager` error), the row status column updates to "Stem Failed" via `callAsync()`; the failure does not affect any other queued job.

### 1.4.6. Cooperative Cancellation and App Quit

29. Every `juce::ThreadPoolJob` in the `LibraryAnalysisQueue` checks `shouldExit()` between each Essentia pipeline stage (waveform, BPM, key) and between each stem separation phase; if `shouldExit()` returns `true`, the job terminates immediately without writing partial data to the database.
30. When the application begins its shutdown sequence, `LibraryAnalysisQueue::cancelAllJobs()` is called before the `juce::ThreadPool` destructor; all queued jobs that have not yet started are removed without execution, and all active jobs exit at their next `shouldExit()` check.
31. The application shutdown does not deadlock or hang waiting for an analysis job to complete a long pipeline stage; the maximum additional shutdown latency introduced by in-progress jobs is bounded by the duration of one pipeline stage (waveform render, BPM pass, or key pass).

### 1.4.7. Architecture and Dependency Rules

32. `LibraryAnalysisQueue` is constructed and owned exclusively by `SonikApplication` and passed by reference or pointer into `LibraryComponent` at construction time; no static instance, global variable, or `getInstance()` accessor exists anywhere in the codebase.
33. All status updates from background `juce::ThreadPoolJob` workers to the `juce::TableListBox` rows are posted exclusively via `juce::MessageManager::callAsync()`, never via direct method calls from the worker thread.
34. `Source/Features/Library/` does not include any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, or `Source/Features/Stems/` directly; cross-module access is limited to the root `juce::ValueTree` and `TrackDatabase` reference passed at construction, as mandated by EPIC-0004 Section 1.3.8.
35. Stem separation jobs submitted via the `LibraryAnalysisQueue` delegate exclusively to the existing `StemSeparationManager`; the `LibraryAnalysisQueue` does not reimplement stem separation logic.
<!--
Describe step by step how the user interacts with this feature.
Use a numbered list that follows the user's real journey, from entry to exit.
Also include relevant alternative cases (e.g., errors, empty states, missing permissions).
-->

## 4. Acceptance Criteria
<!--
List the criteria that must be met for the feature to be considered complete.
Use a checklist in the format "[ ] The system/user...".
Each criterion must be binary: either it is met or it is not. Avoid ambiguous or subjective criteria.
-->