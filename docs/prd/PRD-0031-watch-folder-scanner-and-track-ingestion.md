---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
---
# 1. PRD-0031: Watch Folder Scanner and Track Ingestion

## 1.1. Problem

Professional DJs manage libraries that typically span thousands of tracks distributed across multiple folders, external drives, and nested directory hierarchies. Without automatic folder scanning, every track must be imported manually, which is error-prone, time-consuming, and impossible to maintain at scale. When files are moved or renamed outside the application, the library silently becomes stale, and the DJ only discovers broken references at the worst possible moment — mid-set. Additionally, duplicate files are common in DJ workflows (backups, multiple copies of the same edit), and without content-aware detection, the library fills with redundant entries that clutter browsing and waste cognitive load. The absence of a watch-folder system means Sonik is unusable for anyone with a real collection.

## 1.2. Objective

The system must provide a fully automatic, background watch-folder scanner that ingests audio tracks into the `library_tracks` table without ever blocking the UI thread. The user must be able to add and remove watched folders at any time. On every startup — and on manual re-scan — the scanner walks all configured watched folders recursively, extracts tag metadata and a SHA-256 content hash from each supported audio file, and inserts or updates the corresponding record in `library_tracks`. A second background pass after the scan reconciles missing files by checking physical existence and updating the `is_missing` flag accordingly. The user must receive real-time progress feedback during the scan and must be able to cancel it at any time.

## 1.3. User Flow

### 1.3.1. Adding a Watched Folder

1. The user opens the Library panel and navigates to the Watched Folders section.
2. The user clicks "Add Folder", which opens a native OS folder-picker dialog.
3. The user selects a root directory (e.g., `/Users/dj/Music`).
4. The system inserts a new row into `watched_folders` with `folder_path` set to the selected path and `last_scan_at` set to `NULL`.
5. The system immediately launches a background scan of the newly added folder.
6. A progress bar and status label appear in the Library panel showing the current file being processed and the number of files scanned so far.
7. When the scan completes, the track count for the folder updates and the Library table repaints with the newly ingested records.

### 1.3.2. Startup Scan (Normal Case)

1. The application starts and the audio engine initialises.
2. On the Message Thread, the system reads all rows from `watched_folders`.
3. For each row, the system spawns a single `juce::Thread` (or enqueues work on a background thread pool) to perform the scan; the Message Thread is never blocked.
4. The scanner walks the folder recursively, collecting all files whose extension matches a supported format.
5. For each candidate file, the scanner checks whether `last_scan_at` is set and whether the file's last-modified timestamp is older than `last_scan_at`; if so, the file is skipped (incremental scan).
6. For new or modified files, the scanner reads the file content, computes a SHA-256 hash, extracts tag metadata, and upserts a record into `library_tracks`.
7. After all watched folders have been scanned, the system starts the missing-file reconciliation pass on a background thread.
8. The reconciliation pass iterates all `library_tracks` rows where `is_missing = 0`, calls `juce::File(file_path).existsAsFile()` for each, and sets `is_missing = 1` for any record whose file is no longer present.
9. The Library table repaints rows whose `is_missing` flag changed, showing a visual warning indicator for missing tracks.
10. `last_scan_at` is updated for each `watched_folders` row once its scan phase completes.

### 1.3.3. Manual Re-scan

1. The user right-clicks a watched folder entry and selects "Rescan Now".
2. The system resets `last_scan_at` to `NULL` for that folder, forcing a full (non-incremental) scan.
3. The background scan proceeds as described in section 1.3.2 from step 3 onwards.

### 1.3.4. Removing a Watched Folder

1. The user right-clicks a watched folder entry and selects "Remove Folder".
2. The system deletes the row from `watched_folders`.
3. Existing `library_tracks` records that originated from that folder are NOT deleted automatically; they remain in the library with their current `is_missing` state.

### 1.3.5. Scan Cancellation

1. While a scan is in progress, the user clicks the "Cancel" button in the progress area.
2. The background thread observes the cancellation flag (checked at each file iteration) and exits cleanly.
3. The progress indicator dismisses and the Library table reflects whatever records were ingested before cancellation.
4. `last_scan_at` is NOT updated for folders whose scan was cancelled, so the next scan re-processes them from the beginning.

### 1.3.6. Edge Cases

The following non-happy-path conditions must be handled gracefully:
- A candidate file cannot be opened (permission denied, corrupt file): the file is skipped and the error is logged; the scan continues.
- A file has no ID3/metadata tags: the system inserts a record with `title` derived from the filename, and all other tag fields set to `NULL` or `0`.
- Two files share the same SHA-256 content hash (exact duplicate): the system keeps the existing record and does not insert a second row; instead it logs the duplicate path.
- A watched folder path itself no longer exists at scan time: the scan for that folder is skipped; all `library_tracks` records under that path remain untouched until the next reconciliation pass.
- The application is quit mid-scan: the background thread is signalled to stop via its cancellation flag during `juce::JUCEApplicationBase::systemRequestedQuit`; any partially written DB transaction is rolled back.

## 1.4. Acceptance Criteria

- [ ] The scanner runs exclusively on a background `juce::Thread`; the JUCE Message Thread (UI thread) is never blocked during any phase of the scan.
- [ ] The system supports scanning the following audio file extensions: `.mp3`, `.flac`, `.wav`, `.aiff`, `.aif`, `.ogg`, `.m4a`.
- [ ] Files with extensions not in the supported list are silently ignored during the directory walk.
- [ ] The directory walk is fully recursive; all nested subdirectories of a watched folder are scanned regardless of depth.
- [ ] For each valid audio file, the system extracts the following tag fields and stores them in `library_tracks`: `title`, `artist`, `album`, `bpm` (from tag, not analysis), `duration_seconds`, `file_size_bytes`, and `file_path`.
- [ ] If a tag field is absent or empty in the file's metadata, the corresponding `library_tracks` column is stored as `NULL` (or `0` for numeric fields), never as an empty string.
- [ ] For a file with no readable tags at all, `title` is set to the filename without extension and all other tag columns are set to `NULL` or `0`.
- [ ] The system computes a SHA-256 hash of the full file content (not the file path) for every ingested file and stores it in `library_tracks.content_hash`.
- [ ] When a file is encountered whose `file_path` already exists in `library_tracks` and whose `content_hash` matches the stored value, no database write is performed (no-op).
- [ ] When a file is encountered whose `file_path` already exists in `library_tracks` but whose `content_hash` differs, the system updates all tag metadata columns and `content_hash` for that record, and sets `last_seen_at` to the current timestamp.
- [ ] When a file is encountered whose `content_hash` matches an existing record but with a different `file_path` (file was moved), the system updates `file_path` and `last_seen_at` rather than inserting a duplicate row.
- [ ] When two distinct files share the same `content_hash` (true duplicate), only one record is kept in `library_tracks`; the duplicate path is written to the application log and the second file is not inserted.
- [ ] The scanner performs an incremental scan: files whose last-modified timestamp on disk is older than the `watched_folders.last_scan_at` value for their parent folder are skipped without reading or hashing.
- [ ] A full (non-incremental) scan is triggered when `watched_folders.last_scan_at` is `NULL` for a given folder (i.e., first-ever scan or after a manual "Rescan Now").
- [ ] `watched_folders.last_scan_at` is updated to the current UTC timestamp only after the scan for that folder completes successfully and without cancellation.
- [ ] The user can add a new watched folder via a native OS folder-picker dialog; the selected path is inserted into `watched_folders` and a background scan begins immediately.
- [ ] The user can remove a watched folder; the corresponding row is deleted from `watched_folders` and no `library_tracks` records are deleted as a side effect.
- [ ] While a scan is in progress, the Library panel displays a progress indicator showing at minimum the current file being processed and a running count of files scanned.
- [ ] The user can cancel an in-progress scan at any time; the background thread exits cleanly within one file-processing iteration of receiving the cancellation signal.
- [ ] When a scan is cancelled, `watched_folders.last_scan_at` is NOT updated, ensuring the next scan reprocesses the folder in full.
- [ ] After the watch-folder scan phase completes, the system runs a separate background reconciliation pass that calls `juce::File(file_path).existsAsFile()` for every `library_tracks` row where `is_missing = 0`.
- [ ] Any record that fails the existence check during the reconciliation pass has its `is_missing` column set to `1` in the database.
- [ ] The Library table row for a missing track renders a visual warning indicator (distinct from a normal row) whenever `is_missing = 1`.
- [ ] If a previously missing track is found again during a subsequent scan (its file has been restored or the path has been corrected), `is_missing` is reset to `0` and the warning indicator is removed.
- [ ] A file that cannot be read due to a permission error or corruption is skipped; the error is written to the application log and the scan continues with the next file without crashing.
- [ ] If a watched folder's root path does not exist at scan time, the scan for that folder is skipped entirely and a warning is written to the application log; other folders are scanned normally.
- [ ] The application handles a quit signal while a scan is in progress by signalling the background thread to stop and rolling back any open database transaction before exit.
- [ ] All database writes performed during ingestion are wrapped in a single transaction per folder scan to ensure atomicity and performance.