---
status: Not Implemented
epic: EPIC-0004
depends-on: []
---

# 1. PRD-0030: Library Database Schema and Migration

## 1.1. Problem

Every higher-level Library feature — folder scanning, full-text search, playlist management, missing-file detection — depends on a well-defined, versioned SQLite schema. Without it, the application has no canonical location to persist track metadata across sessions. Users who build collections of thousands of tracks have no guarantee their ratings, play counts, or playlist memberships survive a restart. New installs start with no schema at all, while existing installs risk silent data loss if a future release drops and recreates tables without a migration guard.

The absence of a schema versioning mechanism is particularly damaging for long-term maintainability. If a developer adds a column to `library_tracks` by simply re-running `CREATE TABLE IF NOT EXISTS`, already-indexed tracks are not affected and the column remains `NULL` for the entire existing collection, producing invisible inconsistencies. Without triggers keeping the FTS5 virtual table in sync, full-text search returns stale or missing results the moment any track record is modified after its initial insert. The result is a product that silently degrades as the user's collection grows and evolves.

Professional DJs with collections of 50,000 or more tracks are directly affected. A single startup failure caused by an inconsistent or partially-migrated schema can render the entire library inaccessible, preventing the user from loading tracks to a deck.

## 1.2. Objective

The system must create and maintain a versioned SQLite schema that serves as the single source of truth for all track and playlist data. Specifically:
- The system ensures that all five new tables (`library_tracks`, `library_fts`, `watched_folders`, `playlists`, `playlist_tracks`) exist with their correct columns and types after the first startup.
- The system ensures that every migration runs exactly once, guarded by a `schema_version` table, so repeated application restarts are idempotent.
- The system ensures that existing tables (`cue_points`, `beatgrid_data`, `waveform_cache`, `loops_data`, `stems_data`) are never modified or dropped during the migration, protecting data written by prior PRDs.
- The system ensures that the FTS5 virtual table `library_fts` remains in perfect sync with `library_tracks` at all times through database-level triggers, without requiring application-layer synchronization logic.
- The system ensures that a failed or interrupted migration leaves the database in a consistent state by wrapping the entire migration block in a single SQLite transaction that is rolled back on any error.
- The system ensures that all migration work happens on the JUCE Message Thread at startup, before any `LibraryQueryThread` connection is opened.

## 1.3. User Flow

### 1.3.1. Normal Startup (First Run)

1. `SonikApplication::initialise()` constructs `TrackDatabase` and calls `createTables()` on the Message Thread.
2. `createTables()` opens the SQLite connection, enables WAL journal mode (`PRAGMA journal_mode=WAL`), and enables foreign key enforcement (`PRAGMA foreign_keys=ON`).
3. The method checks for the existence of the `schema_version` table. It does not exist on a first run, so the method creates it with a single `version INTEGER PRIMARY KEY` column.
4. The method reads the current version from `schema_version`. The table is empty, so the version is treated as 0.
5. Because the current version is below migration 1, the method opens a transaction and executes the full migration-1 block: creating `library_tracks`, `library_fts` (FTS5 content-backed virtual table), `watched_folders`, `playlists`, `playlist_tracks`, and all three FTS sync triggers.
6. On success, the method inserts version 1 into `schema_version` and commits the transaction.
7. The application proceeds to construct the `LibraryQueryThread` and open its independent SQLite connection. The schema is now complete and stable.

### 1.3.2. Startup After a Previous Install (Upgrade Path)

1. `createTables()` opens the connection and reads the `schema_version` table. It finds version 1.
2. All migration blocks for versions up to and including 1 are skipped because their guard conditions are not met.
3. Any future migration blocks (version 2, version 3, etc.) are each checked in order. If a block's version is above the stored version, it is executed inside a transaction, followed by an update to `schema_version`. Otherwise it is skipped.
4. The existing `library_tracks` data, playlists, and cue points are untouched. The user's collection is available immediately.

### 1.3.3. Mid-Migration Crash Recovery

1. The application crashes or is force-quit after the transaction for migration 1 was opened but before it was committed.
2. On the next startup, SQLite's WAL rollback recovery restores the database to its pre-migration state automatically. No manual intervention is required.
3. `createTables()` detects `schema_version` still at 0 and re-executes migration 1 from the beginning.
4. The migration succeeds and the user proceeds normally. No data loss occurs because the pre-migration database contained no library data yet.

### 1.3.4. Corrupt Database at Startup

1. The SQLite `sqlite3_open` call succeeds but the first integrity-sensitive operation (reading `schema_version`) returns a non-`SQLITE_OK` result code due to file corruption.
2. `createTables()` logs the error via `JUCE_ASSERT` and returns without proceeding.
3. The application surfaces an alert dialog informing the user that the database file could not be read and offering two options: quit, or reset the database (delete the file and restart with an empty schema).
4. If the user chooses reset, the corrupt file is renamed to `sonik_db_backup_<timestamp>.sqlite` for diagnostic purposes and a new, empty database is created with a fresh migration run.

### 1.3.5. FTS5 Trigger Lifecycle

1. A new track record is inserted into `library_tracks` by the scanner. The `after_insert_library_tracks` trigger fires automatically and inserts the corresponding row into `library_fts`.
2. A tag re-read updates the `title`, `artist`, or `album` columns of an existing row in `library_tracks`. The `after_update_library_tracks` trigger fires: it deletes the stale FTS row and inserts a fresh one.
3. A track is permanently deleted from `library_tracks` (e.g., "Remove from Collection"). The `after_delete_library_tracks` trigger fires and removes the corresponding FTS row, ensuring no ghost entries remain in the search index.

## 1.4. Acceptance Criteria

- [ ] The system opens the SQLite database with `PRAGMA journal_mode=WAL` immediately after the connection is established and before any DDL is executed.
- [ ] The system enables `PRAGMA foreign_keys=ON` on every connection (both Message Thread and `LibraryQueryThread`) before any DML is executed.
- [ ] The system creates a `schema_version` table with a single `version INTEGER PRIMARY KEY` column if it does not already exist.
- [ ] The system reads the current schema version before executing any migration block and skips all blocks whose version is less than or equal to the stored version.
- [ ] The system wraps the entire migration-1 block (all five table creations plus all three trigger creations) in a single `BEGIN IMMEDIATE` / `COMMIT` transaction.
- [ ] The system rolls back the migration transaction and does not update `schema_version` if any DDL statement within the migration block returns a non-`SQLITE_OK` result code.
- [ ] The system creates the `library_tracks` table with columns: `id INTEGER PRIMARY KEY AUTOINCREMENT`, `file_path TEXT NOT NULL UNIQUE`, `content_hash TEXT NOT NULL`, `title TEXT`, `artist TEXT`, `album TEXT`, `bpm REAL`, `key TEXT`, `key_index INTEGER`, `duration_seconds REAL`, `file_size_bytes INTEGER`, `date_added INTEGER NOT NULL`, `last_seen INTEGER`, `is_missing INTEGER NOT NULL DEFAULT 0`, `play_count INTEGER NOT NULL DEFAULT 0`, `rating INTEGER NOT NULL DEFAULT 0`.
- [ ] The system creates the `library_fts` table as a content-backed FTS5 virtual table (`CREATE VIRTUAL TABLE library_fts USING fts5(title, artist, album, content=library_tracks, content_rowid=id)`).
- [ ] The system creates the `watched_folders` table with columns: `id INTEGER PRIMARY KEY AUTOINCREMENT`, `folder_path TEXT NOT NULL UNIQUE`, `last_scanned_at INTEGER`.
- [ ] The system creates the `playlists` table with columns: `id INTEGER PRIMARY KEY AUTOINCREMENT`, `name TEXT NOT NULL`, `type TEXT NOT NULL DEFAULT 'normal'`, `created_at INTEGER NOT NULL`.
- [ ] The system creates the `playlist_tracks` table with columns: `playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE`, `track_id INTEGER NOT NULL REFERENCES library_tracks(id) ON DELETE CASCADE`, `position INTEGER NOT NULL`, `PRIMARY KEY (playlist_id, track_id)`.
- [ ] The system creates a trigger `after_insert_library_tracks` that executes `INSERT INTO library_fts(rowid, title, artist, album) VALUES (new.id, new.title, new.artist, new.album)` after every INSERT on `library_tracks`.
- [ ] The system creates a trigger `after_update_library_tracks` that executes a delete of the stale FTS row followed by an insert of the updated row after every UPDATE on `library_tracks` where `title`, `artist`, or `album` has changed.
- [ ] The system creates a trigger `after_delete_library_tracks` that executes `INSERT INTO library_fts(library_fts, rowid, title, artist, album) VALUES ('delete', old.id, old.title, old.artist, old.album)` after every DELETE on `library_tracks`.
- [ ] The system does not drop, alter, or otherwise modify the `cue_points`, `beatgrid_data`, `waveform_cache`, `loops_data`, or `stems_data` tables at any point during the migration.
- [ ] The system inserts or updates the `schema_version` row to `1` only after the migration-1 transaction has been successfully committed.
- [ ] The system executes the entire `createTables()` call synchronously on the JUCE Message Thread before constructing any `LibraryQueryThread` instance.
- [ ] The system does not execute any migration SQL inside the `LibraryQueryThread`'s connection; that connection opens only after `createTables()` has returned successfully.
- [ ] The system is idempotent: calling `createTables()` a second time on an already-migrated database produces no DDL changes and returns without error.
- [ ] The system can run migration-1 followed by a hypothetical migration-2 in a single startup if the stored version is 0, executing each migration block in sequence inside its own transaction.
- [ ] The system's FTS5 table returns correct results for a prefix-match query (`title MATCH 'bey*'`) immediately after an INSERT into `library_tracks` without any explicit application-layer sync call.
- [ ] The system's FTS5 table no longer returns a result for a deleted track immediately after a DELETE on `library_tracks`, confirmed by a query returning zero rows.
- [ ] The system's FTS5 table returns updated results reflecting a renamed title immediately after an UPDATE on `library_tracks`, with the old title no longer matching and the new title matching.
- [ ] The system renames the corrupt database file to `sonik_db_backup_<timestamp>.sqlite` and creates a new empty database when the user chooses the reset option in the corruption recovery dialog, rather than deleting the corrupt file.