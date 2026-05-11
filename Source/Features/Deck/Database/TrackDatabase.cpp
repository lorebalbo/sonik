#include "TrackDatabase.h"
#include <sqlite3.h>

TrackDatabase::TrackDatabase (const juce::File& dbFile)
    : dbFileStored (dbFile)
{
    dbFile.getParentDirectory().createDirectory();

    auto result = sqlite3_open (dbFile.getFullPathName().toRawUTF8(), &dbHandle);
    jassert (result == SQLITE_OK);

    if (result != SQLITE_OK)
    {
        DBG ("TrackDatabase: failed to open " + dbFile.getFullPathName());
        dbHandle = nullptr;
        return;
    }

    // Enable WAL journal mode for crash-safe writes and concurrent readers.
    execRc ("PRAGMA journal_mode=WAL;");
    // Enforce referential integrity (CASCADE deletes, FK constraints).
    execRc ("PRAGMA foreign_keys=ON;");

    createTables();
}

TrackDatabase::~TrackDatabase()
{
    if (dbHandle != nullptr)
        sqlite3_close (dbHandle);
}

void TrackDatabase::createTables()
{
    exec ("CREATE TABLE IF NOT EXISTS session_state ("
          "  key   TEXT PRIMARY KEY,"
          "  value TEXT"
          ");");

    exec ("CREATE TABLE IF NOT EXISTS track_data ("
          "  file_path              TEXT    NOT NULL,"
          "  content_hash           TEXT    NOT NULL,"
          "  cue_points_json        TEXT,"
          "  beatgrid_json          TEXT,"
          "  key_index              INTEGER DEFAULT -1,"
          "  key_confidence         REAL    DEFAULT 0.0,"
          "  key_manually_adjusted  INTEGER DEFAULT 0,"
          "  PRIMARY KEY (file_path, content_hash)"
          ");");

    exec ("CREATE TABLE IF NOT EXISTS waveform_cache ("
          "  content_hash TEXT PRIMARY KEY,"
          "  data         BLOB,"
          "  created_at   INTEGER"
          ");");

    exec ("CREATE TABLE IF NOT EXISTS loops_data ("
          "  content_hash TEXT PRIMARY KEY,"
          "  loops_json   TEXT NOT NULL,"
          "  updated_at   INTEGER"
          ");");

    exec ("CREATE TABLE IF NOT EXISTS stems_data ("
          "  content_hash    TEXT PRIMARY KEY,"
          "  model_version   TEXT,"
          "  status          TEXT DEFAULT 'pending',"
          "  vocal_path      TEXT,"
          "  drums_path      TEXT,"
          "  bass_path       TEXT,"
          "  other_path      TEXT,"
          "  created_at      INTEGER,"
          "  file_size_bytes INTEGER DEFAULT 0"
          ");");

    // -----------------------------------------------------------------------
    // Migration system (PRD-0030)
    // -----------------------------------------------------------------------
    exec ("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY);");

    // Determine the current schema version (0 when the table is empty).
    int currentVersion = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (dbHandle,
                                "SELECT version FROM schema_version LIMIT 1;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                currentVersion = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
    }

    // Each migration block is guarded by its minimum required version.
    if (currentVersion < 1)
    {
        const bool ok = applyMigration1();
        jassert (ok); // Migration 1 failed — inspect DBG output for SQL errors.
        (void) ok;
        currentVersion = 2;
    }

    if (currentVersion < 2)
    {
        const bool ok = applyMigration2();
        jassert (ok); // Migration 2 failed — inspect DBG output for SQL errors.
        (void) ok;
    }
}

int TrackDatabase::execRc (const char* sql) noexcept
{
    if (dbHandle == nullptr) return SQLITE_ERROR;

    char* errMsg = nullptr;
    const int rc = sqlite3_exec (dbHandle, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        DBG ("TrackDatabase SQL error: " + juce::String (errMsg != nullptr ? errMsg : "unknown"));
        if (errMsg != nullptr)
            sqlite3_free (errMsg);
    }

    return rc;
}

void TrackDatabase::exec (const juce::String& sql)
{
    execRc (sql.toRawUTF8());
}

// ---------------------------------------------------------------------------
// Migration 1 — Library schema (PRD-0030)
// All DDL is wrapped in a single BEGIN IMMEDIATE / COMMIT transaction.
// On any failure the transaction is rolled back and schema_version is NOT
// updated, so the migration will be retried on the next startup.
// ---------------------------------------------------------------------------
bool TrackDatabase::applyMigration1()
{
    if (execRc ("BEGIN IMMEDIATE;") != SQLITE_OK)
        return false;

    // Convenience: roll back and signal failure.
    const auto rollbackAndFail = [this]() -> bool
    {
        char* e = nullptr;
        sqlite3_exec (dbHandle, "ROLLBACK;", nullptr, nullptr, &e);
        if (e != nullptr) sqlite3_free (e);
        return false;
    };

    // -- library_tracks -------------------------------------------------------
    if (execRc (
        "CREATE TABLE library_tracks ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_path        TEXT    NOT NULL UNIQUE,"
        "  content_hash     TEXT    NOT NULL,"
        "  title            TEXT,"
        "  artist           TEXT,"
        "  album            TEXT,"
        "  bpm              REAL,"
        "  key              TEXT,"
        "  key_index        INTEGER,"
        "  duration_seconds REAL,"
        "  file_size_bytes  INTEGER,"
        "  date_added       INTEGER NOT NULL,"
        "  last_seen        INTEGER,"
        "  is_missing       INTEGER NOT NULL DEFAULT 0,"
        "  play_count       INTEGER NOT NULL DEFAULT 0,"
        "  rating           INTEGER NOT NULL DEFAULT 0"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    // -- library_fts (FTS5 content-backed virtual table) ----------------------
    if (execRc (
        "CREATE VIRTUAL TABLE library_fts USING fts5("
        "  title, artist, album,"
        "  content=library_tracks,"
        "  content_rowid=id"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    // -- watched_folders ------------------------------------------------------
    if (execRc (
        "CREATE TABLE watched_folders ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  folder_path     TEXT    NOT NULL UNIQUE,"
        "  last_scanned_at INTEGER"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    // -- playlists ------------------------------------------------------------
    if (execRc (
        "CREATE TABLE playlists ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name       TEXT    NOT NULL,"
        "  type       TEXT    NOT NULL DEFAULT 'normal',"
        "  created_at INTEGER NOT NULL"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc (
        "CREATE UNIQUE INDEX idx_playlists_name_nocase "
        "ON playlists(name COLLATE NOCASE);") != SQLITE_OK)
        return rollbackAndFail();

    // -- playlist_tracks ------------------------------------------------------
    if (execRc (
        "CREATE TABLE playlist_tracks ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        "  track_id    INTEGER NOT NULL REFERENCES library_tracks(id) ON DELETE CASCADE,"
        "  position    INTEGER NOT NULL,"
        "  played_at   TEXT"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    // -- FTS sync triggers ----------------------------------------------------
    if (execRc (
        "CREATE TRIGGER after_insert_library_tracks "
        "AFTER INSERT ON library_tracks BEGIN "
        "  INSERT INTO library_fts(rowid, title, artist, album) "
        "  VALUES (new.id, new.title, new.artist, new.album); "
        "END;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc (
        "CREATE TRIGGER after_update_library_tracks "
        "AFTER UPDATE ON library_tracks "
        "WHEN old.title != new.title OR old.artist != new.artist OR old.album != new.album BEGIN "
        "  INSERT INTO library_fts(library_fts, rowid, title, artist, album) "
        "  VALUES('delete', old.id, old.title, old.artist, old.album); "
        "  INSERT INTO library_fts(rowid, title, artist, album) "
        "  VALUES (new.id, new.title, new.artist, new.album); "
        "END;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc (
        "CREATE TRIGGER after_delete_library_tracks "
        "AFTER DELETE ON library_tracks BEGIN "
        "  INSERT INTO library_fts(library_fts, rowid, title, artist, album) "
        "  VALUES('delete', old.id, old.title, old.artist, old.album); "
        "END;") != SQLITE_OK)
        return rollbackAndFail();

    // -- Seed: built-in History playlist --------------------------------------
    if (execRc (
        "INSERT INTO playlists (name, type, created_at) "
        "VALUES ('History', 'history', CAST(strftime('%s', 'now') AS INTEGER));") != SQLITE_OK)
        return rollbackAndFail();

    // -- Stamp schema version -------------------------------------------------
    if (execRc ("INSERT INTO schema_version (version) VALUES (2);") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc ("COMMIT;") != SQLITE_OK)
    {
        // COMMIT failed; WAL crash-recovery will restore a clean state on next
        // open, and the migration will be retried because schema_version is
        // still empty.
        return false;
    }

    return true;
}

bool TrackDatabase::applyMigration2()
{
    if (execRc ("BEGIN IMMEDIATE;") != SQLITE_OK)
        return false;

    const auto rollbackAndFail = [this]() -> bool
    {
        char* e = nullptr;
        sqlite3_exec (dbHandle, "ROLLBACK;", nullptr, nullptr, &e);
        if (e != nullptr) sqlite3_free (e);
        return false;
    };

    if (execRc (
        "CREATE TABLE playlist_tracks_new ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        "  track_id    INTEGER NOT NULL REFERENCES library_tracks(id) ON DELETE CASCADE,"
        "  position    INTEGER NOT NULL,"
        "  played_at   TEXT"
        ");") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc (
        "INSERT INTO playlist_tracks_new (playlist_id, track_id, position) "
        "SELECT playlist_id, track_id, position FROM playlist_tracks "
        "ORDER BY playlist_id, position;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc ("DROP TABLE playlist_tracks;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc ("ALTER TABLE playlist_tracks_new RENAME TO playlist_tracks;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_playlists_name_nocase "
        "ON playlists(name COLLATE NOCASE);") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc ("UPDATE schema_version SET version = 2;") != SQLITE_OK)
        return rollbackAndFail();

    if (execRc ("COMMIT;") != SQLITE_OK)
        return false;

    return true;
}

void TrackDatabase::saveSessionState (int deckCount, const juce::String& activeDeckId,
                                       const juce::String& loadedTracksJson)
{
    if (dbHandle == nullptr) return;

    auto upsert = [this] (const char* key, const juce::String& value)
    {
        const char* sql = "INSERT OR REPLACE INTO session_state (key, value) VALUES (?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_text (stmt, 2, value.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_step (stmt);
            sqlite3_finalize (stmt);
        }
    };

    upsert ("deckCount",    juce::String (deckCount));
    upsert ("activeDeckId", activeDeckId);
    upsert ("loadedTracks", loadedTracksJson);
}

SessionData TrackDatabase::loadSessionState()
{
    SessionData data;
    if (dbHandle == nullptr) return data;

    auto query = [this] (const char* key) -> juce::String
    {
        const char* sql = "SELECT value FROM session_state WHERE key = ?;";
        sqlite3_stmt* stmt = nullptr;
        juce::String result;

        if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, key, -1, SQLITE_STATIC);
            if (sqlite3_step (stmt) == SQLITE_ROW)
                result = juce::String::fromUTF8 (
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0)));
            sqlite3_finalize (stmt);
        }
        return result;
    };

    auto deckCountStr = query ("deckCount");
    if (deckCountStr.isNotEmpty())
        data.deckCount = deckCountStr.getIntValue();

    auto activeId = query ("activeDeckId");
    if (activeId.isNotEmpty())
        data.activeDeckId = activeId;

    data.loadedTracksJson = query ("loadedTracks");

    return data;
}

void TrackDatabase::saveTrackData (const juce::String& filePath, const juce::String& contentHash,
                                    const juce::String& cuePointsJson, const juce::String& beatgridJson,
                                    int keyIndex, float keyConfidence, bool keyManuallyAdjusted)
{
    if (dbHandle == nullptr) return;

    const char* sql =
        "INSERT OR REPLACE INTO track_data "
        "(file_path, content_hash, cue_points_json, beatgrid_json, "
        " key_index, key_confidence, key_manually_adjusted) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text  (stmt, 1, filePath.toRawUTF8(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, contentHash.toRawUTF8(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, cuePointsJson.toRawUTF8(),-1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, beatgridJson.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 5, keyIndex);
        sqlite3_bind_double(stmt, 6, static_cast<double> (keyConfidence));
        sqlite3_bind_int   (stmt, 7, keyManuallyAdjusted ? 1 : 0);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

std::optional<TrackPersistentData> TrackDatabase::loadTrackData (const juce::String& filePath,
                                                                  const juce::String& contentHash)
{
    if (dbHandle == nullptr) return std::nullopt;

    const char* sql =
        "SELECT cue_points_json, beatgrid_json, key_index, key_confidence, key_manually_adjusted "
        "FROM track_data WHERE file_path = ? AND content_hash = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);

    std::optional<TrackPersistentData> result;

    if (sqlite3_step (stmt) == SQLITE_ROW)
    {
        TrackPersistentData data;
        auto col0 = sqlite3_column_text (stmt, 0);
        if (col0 != nullptr)
            data.cuePointsJson = juce::String::fromUTF8 (reinterpret_cast<const char*> (col0));

        auto col1 = sqlite3_column_text (stmt, 1);
        if (col1 != nullptr)
            data.beatgridJson = juce::String::fromUTF8 (reinterpret_cast<const char*> (col1));

        data.keyIndex           = sqlite3_column_int    (stmt, 2);
        data.keyConfidence      = static_cast<float> (sqlite3_column_double (stmt, 3));
        data.keyManuallyAdjusted = sqlite3_column_int   (stmt, 4) != 0;

        result = data;
    }

    sqlite3_finalize (stmt);
    return result;
}

void TrackDatabase::updateLibraryTrackBpm (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           double bpm)
{
    if (dbHandle == nullptr || filePath.isEmpty() || bpm <= 0.0)
        return;

    juce::ignoreUnused (contentHash);

    const char* sql =
        "UPDATE library_tracks SET bpm = ? "
        "WHERE file_path = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_double (stmt, 1, bpm);
        sqlite3_bind_text   (stmt, 2, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

void TrackDatabase::updateLibraryTrackKey (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           const juce::String& key,
                                           int keyIndex)
{
    if (dbHandle == nullptr || filePath.isEmpty() || key.isEmpty() || keyIndex < 0)
        return;

    juce::ignoreUnused (contentHash);

    const char* sql =
        "UPDATE library_tracks SET key = ?, key_index = ? "
        "WHERE file_path = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, key.toRawUTF8(),         -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt, 2, keyIndex);
        sqlite3_bind_text (stmt, 3, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

void TrackDatabase::storeWaveformData (const juce::String& contentHash,
                                       const juce::MemoryBlock& data)
{
    if (dbHandle == nullptr)
        return;

    const char* sql =
        "INSERT OR REPLACE INTO waveform_cache (content_hash, data, created_at) VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob (stmt, 2, data.getData(), static_cast<int> (data.getSize()), SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 3, static_cast<sqlite3_int64> (juce::Time::currentTimeMillis() / 1000));
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

bool TrackDatabase::loadWaveformData (const juce::String& contentHash,
                                      juce::MemoryBlock& data)
{
    if (dbHandle == nullptr)
        return false;

    const char* sql = "SELECT data FROM waveform_cache WHERE content_hash = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text (stmt, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step (stmt) == SQLITE_ROW)
    {
        const void* blob = sqlite3_column_blob (stmt, 0);
        int blobSize = sqlite3_column_bytes (stmt, 0);

        if (blob != nullptr && blobSize > 0)
        {
            data.replaceAll (blob, static_cast<size_t> (blobSize));
            found = true;
        }
    }

    sqlite3_finalize (stmt);
    return found;
}

void TrackDatabase::saveCuePointsJson (const juce::String& filePath,
                                       const juce::String& contentHash,
                                       const juce::String& json)
{
    if (dbHandle == nullptr)
        return;

    const char* sql =
        "INSERT INTO track_data (file_path, content_hash, cue_points_json) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(file_path, content_hash) "
        "DO UPDATE SET cue_points_json = excluded.cue_points_json;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, json.toRawUTF8(),        -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

juce::String TrackDatabase::loadCuePointsJson (const juce::String& contentHash)
{
    if (dbHandle == nullptr)
        return {};

    const char* sql =
        "SELECT cue_points_json FROM track_data WHERE content_hash = ? LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    juce::String result;

    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step (stmt) == SQLITE_ROW)
        {
            auto col = sqlite3_column_text (stmt, 0);
            if (col != nullptr)
                result = juce::String::fromUTF8 (reinterpret_cast<const char*> (col));
        }
        sqlite3_finalize (stmt);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Loop persistence (PRD-0014)
// ---------------------------------------------------------------------------

void TrackDatabase::saveLoopsJson (const juce::String& contentHash,
                                    const juce::String& json)
{
    if (dbHandle == nullptr)
        return;

    const char* sql =
        "INSERT OR REPLACE INTO loops_data (content_hash, loops_json, updated_at) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text  (stmt, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, json.toRawUTF8(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 3, static_cast<sqlite3_int64> (
            juce::Time::currentTimeMillis() / 1000));
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

juce::String TrackDatabase::loadLoopsJson (const juce::String& contentHash)
{
    if (dbHandle == nullptr)
        return {};

    const char* sql = "SELECT loops_json FROM loops_data WHERE content_hash = ?;";

    sqlite3_stmt* stmt = nullptr;
    juce::String result;

    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step (stmt) == SQLITE_ROW)
        {
            auto col = sqlite3_column_text (stmt, 0);
            if (col != nullptr)
                result = juce::String::fromUTF8 (reinterpret_cast<const char*> (col));
        }
        sqlite3_finalize (stmt);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Stem cache persistence (PRD-0020)
// ---------------------------------------------------------------------------

void TrackDatabase::insertStemRecord (const juce::String& contentHash,
                                       const juce::String& modelVersion,
                                       const juce::String& status)
{
    if (dbHandle == nullptr) return;

    const char* sql =
        "INSERT OR REPLACE INTO stems_data "
        "(content_hash, model_version, status, created_at) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt* stmt2 = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text  (stmt2, 1, contentHash.toRawUTF8(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 2, modelVersion.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 3, status.toRawUTF8(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt2, 4, static_cast<sqlite3_int64> (
            juce::Time::currentTimeMillis() / 1000));
        sqlite3_step (stmt2);
        sqlite3_finalize (stmt2);
    }
}

void TrackDatabase::updateStemRecord (const juce::String& contentHash,
                                       const juce::String& status,
                                       int64_t fileSizeBytes,
                                       const juce::String& vocalPath,
                                       const juce::String& drumsPath,
                                       const juce::String& bassPath,
                                       const juce::String& otherPath)
{
    if (dbHandle == nullptr) return;

    const char* sql =
        "UPDATE stems_data SET status = ?, file_size_bytes = ?, "
        "vocal_path = ?, drums_path = ?, bass_path = ?, other_path = ? "
        "WHERE content_hash = ?;";

    sqlite3_stmt* stmt2 = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text  (stmt2, 1, status.toRawUTF8(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt2, 2, static_cast<sqlite3_int64> (fileSizeBytes));
        sqlite3_bind_text  (stmt2, 3, vocalPath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 4, drumsPath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 5, bassPath.toRawUTF8(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 6, otherPath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt2, 7, contentHash.toRawUTF8(),  -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt2);
        sqlite3_finalize (stmt2);
    }
}

bool TrackDatabase::hasStemRecord (const juce::String& contentHash) const
{
    if (dbHandle == nullptr) return false;

    const char* sql =
        "SELECT 1 FROM stems_data WHERE content_hash = ? AND status = 'complete' LIMIT 1;";

    sqlite3_stmt* stmt2 = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt2, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        found = (sqlite3_step (stmt2) == SQLITE_ROW);
        sqlite3_finalize (stmt2);
    }

    return found;
}

void TrackDatabase::deleteStemRecord (const juce::String& contentHash)
{
    if (dbHandle == nullptr) return;

    const char* sql = "DELETE FROM stems_data WHERE content_hash = ?;";

    sqlite3_stmt* stmt2 = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt2, 1, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt2);
        sqlite3_finalize (stmt2);
    }
}

juce::StringArray TrackDatabase::getPendingStemHashes()
{
    juce::StringArray hashes;
    if (dbHandle == nullptr) return hashes;

    const char* sql = "SELECT content_hash FROM stems_data WHERE status = 'pending';";

    sqlite3_stmt* stmt2 = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step (stmt2) == SQLITE_ROW)
        {
            auto col = sqlite3_column_text (stmt2, 0);
            if (col != nullptr)
                hashes.add (juce::String::fromUTF8 (reinterpret_cast<const char*> (col)));
        }
        sqlite3_finalize (stmt2);
    }

    return hashes;
}

std::vector<TrackDatabase::StemRecordInfo> TrackDatabase::getAllStemRecords()
{
    std::vector<StemRecordInfo> records;
    if (dbHandle == nullptr) return records;

    const char* sql =
        "SELECT content_hash, file_size_bytes, created_at "
        "FROM stems_data WHERE status = 'complete' ORDER BY created_at ASC;";

    sqlite3_stmt* stmt2 = nullptr;
    if (sqlite3_prepare_v2 (dbHandle, sql, -1, &stmt2, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step (stmt2) == SQLITE_ROW)
        {
            StemRecordInfo info;
            auto col = sqlite3_column_text (stmt2, 0);
            if (col != nullptr)
                info.contentHash = juce::String::fromUTF8 (reinterpret_cast<const char*> (col));
            info.fileSizeBytes = sqlite3_column_int64 (stmt2, 1);
            info.createdAt     = sqlite3_column_int64 (stmt2, 2);
            records.push_back (std::move (info));
        }
        sqlite3_finalize (stmt2);
    }

    return records;
}
