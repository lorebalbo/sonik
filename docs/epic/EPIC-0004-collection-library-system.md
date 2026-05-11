---
name: "EPIC-0004: Collection & Library System"
status: Open
---

# 1. EPIC-0004: Collection & Library System

## 1.1. Goal and Vision

Build a high-performance, SQLite-backed track management system that is the central hub of the DJ workflow. The library must handle collections of 50,000+ tracks without ever blocking the JUCE Message Thread, provide instant full-text search with Traktor-style scope operators, and offer "Deck-Aware" smart filtering that dynamically narrows the visible collection based on the effective tempo and key of currently loaded decks.

The guiding principle is that the Library module is a **passive observer** of deck state, never a direct dependent of any deck module. It receives the root `juce::ValueTree` and listens — nothing more.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

The following capabilities are included in this Epic:
- SQLite schema for a `library_tracks` table with FTS5 full-text search
- Auto-scan of user-configured "Music Folders" on startup, with tag extraction and content-hash ingestion
- An asynchronous `LibraryQueryThread` that owns its own SQLite connection and keeps all SQL off the Message Thread
- A Library UI Shell conforming to `DESIGN.md`: global search bar with Traktor-style scope/prefix operators, KEY MATCH and BPM MATCH toggle buttons, a BPM VISION parameter input, a left sidebar for navigation (Collection, Folders, Playlists), and a virtualized sortable `juce::TableListBox`
- Track loading into a deck via drag-and-drop and right-click "Load to Deck X" / double-click
- Deck-Aware Smart Filters: KEY MATCH filters by active deck musical keys; BPM MATCH filters by effective tempo (beatgrid BPM × speedMultiplier) within a configurable BPM VISION window; both filters update reactively when deck state changes
- Playlist creation, renaming, deletion, and track ordering
- Auto-generated session History playlist (appended on every play event)
- Preparation List (session-scoped in-memory scratch-pad)
- Track rating (1–5 stars), played/unplayed session indicator per row
- Right-click context menus on single tracks, multi-select, and on playlist nodes for bulk "Analyze Track(s)" and "Separate Stems" actions, routed through a CPU-throttled `LibraryAnalysisQueue`
- Missing file detection at startup and on load-to-deck, with a "Relocate File" dialog

### 1.2.2. Out of Scope

The following are explicitly excluded from this Epic:
- Rekordbox XML or iTunes Library import (deferred to a future Epic)
- Audio preview playback from the library (requires a dedicated headphone cue routing PRD)
- MIDI controller mapping for library navigation (deferred to a MIDI Epic)
- Cloud sync or remote library features
- Waveform thumbnail strips in the Preview column (cover art thumbnail only for MVP)

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Thread Model

All SQLite access must happen on either the Message Thread (schema migrations, single-write operations at startup) or the dedicated `LibraryQueryThread`. Never run queries inside `paint()`, `resized()`, or any audio callback. The `LibraryQueryThread` owns its own `sqlite3*` handle — SQLite connections must not be shared across threads without a mutex, and in this architecture we choose connection-per-thread instead.

Search input must be debounced at 150 ms before dispatching a query to avoid query storms from fast typists.

### 1.3.2. Schema Migration Strategy

The new tables are added via a versioned migration block inside the existing `TrackDatabase::createTables()` method. A `schema_version` table gates which migrations have run. This is non-destructive: existing `cue_points`, `beatgrid_data`, `waveform_cache`, `loops_data`, and `stems_data` tables remain untouched.

New tables required:
- `library_tracks`: canonical track records with file path, content hash, tag metadata (title, artist, album, BPM, key, duration, file size, date added, last seen, is_missing flag, play count, rating)
- `library_fts`: FTS5 virtual table over `library_tracks(title, artist, album)`
- `watched_folders`: user-configured root scan directories with last-scan timestamp
- `playlists`: playlist records (id, name, type: normal/history/preparation, created_at)
- `playlist_tracks`: ordered junction table (playlist_id, track_id, position)

### 1.3.3. FTS5 Full-Text Search

`library_fts` is a content-backed FTS5 virtual table pointed at `library_tracks`. All INSERT/UPDATE/DELETE on `library_tracks` must be mirrored into `library_fts` via SQLite triggers. Queries use the `MATCH` operator with trailing wildcards (`query*`) for prefix matching.

The search bar supports Traktor-style scope operators parsed client-side before SQL dispatch:
- `bpm:128` — exact BPM match (±0.5 tolerance)
- `bpm:125-135` — BPM range
- `key:Am` — exact key filter (maps to `key_index`)
- `rating:4` — minimum star rating
- `title:word`, `artist:word`, `album:word` — field-scoped FTS
- Bare words with no prefix: full FTS across title + artist + album

### 1.3.4. Deck-Aware Filter: Effective Tempo

The BPM MATCH filter must read **effective tempo**: `beatgrid_bpm × speedMultiplier`, not the raw beatgrid BPM stored in the ValueTree. This is the same value published by `MasterClockPublisher`. The `LibraryComponent` listens to `IDs::speedMultiplier` and `IDs::bpm` on every Deck subtree via `valueTreePropertyChanged()`. On any change, it recomputes the `DeckAwareFilterState` and re-dispatches the current query.

### 1.3.5. Virtualized Table Rendering

The `juce::TableListBox` must be configured for virtual rendering (row heights uniform, `getNumRows()` returns the query result count, `paintCell()` reads from a result buffer). The result buffer is a `std::vector<LibraryTrackRow>` swapped atomically on the Message Thread after each query completes. No row data is fetched inside `paintCell()`.

### 1.3.6. Bulk Analysis Queue CPU Contract

The `LibraryAnalysisQueue` enforces:
- Maximum 2 concurrent analysis workers (waveform/BPM/key analysis via Essentia)
- Maximum 1 concurrent stem separation job (delegates to the existing `StemSeparationManager`)
- Jobs are queued as `juce::ThreadPoolJob` entries; `shouldExit()` is checked between pipeline stages for cooperative cancellation
- Each completed job calls `juce::MessageManager::callAsync()` to update the row status column
- The queue is owned by `SonikApplication` and injected into `LibraryComponent` — no singleton

### 1.3.7. Missing File Reconciliation

On startup, after the watch-folder scan completes, a second background pass checks `juce::File(file_path).existsAsFile()` for all records where `is_missing = 0`. Records that fail are updated to `is_missing = 1` and the table repaints their row with a warning indicator. On load-to-deck, existence is re-checked synchronously before dispatching to `AudioFileLoader`. If missing, a dialog offers "Relocate" (opens `juce::FileChooser`, updates `file_path`) or "Remove from Library".

### 1.3.8. Module Boundary Rule

`Source/Features/Library/` must not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, or any other Feature module directly. The only cross-module dependency allowed is the root `juce::ValueTree` passed by reference at construction, and the `TrackDatabase` reference for read/write. All deck state is read exclusively through ValueTree property keys defined in `DeckIdentifiers.h`.

## 1.4. PRD Roadmap

- [x] PRD-0030: Library Database Schema & Migration
- [x] PRD-0031: Watch Folder Scanner & Track Ingestion
- [x] PRD-0032: Async Library Query Engine (LibraryQueryThread)
- [x] PRD-0033: Library UI Shell (Table, Search Bar, Sidebar, Navigation)
- [x] PRD-0034: Track Loading & Drag-and-Drop to Deck
- [x] PRD-0035: Deck-Aware Smart Filters (KEY MATCH / BPM MATCH / BPM VISION)
- [ ] PRD-0036: Playlist Management (Playlists, History, Preparation List)
- [ ] PRD-0037: Track Rating & Played Session Indicator
- [ ] PRD-0038: Bulk Analysis Queue & Right-Click Context Menus
- [ ] PRD-0039: Missing File Detection & Relocation