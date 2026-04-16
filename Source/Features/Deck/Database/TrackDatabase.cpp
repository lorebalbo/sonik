#include "TrackDatabase.h"
#include <sqlite3.h>

TrackDatabase::TrackDatabase (const juce::File& dbFile)
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
}

void TrackDatabase::exec (const juce::String& sql)
{
    if (dbHandle == nullptr) return;

    char* errMsg = nullptr;
    auto result = sqlite3_exec (dbHandle, sql.toRawUTF8(), nullptr, nullptr, &errMsg);

    if (result != SQLITE_OK)
    {
        DBG ("TrackDatabase SQL error: " + juce::String (errMsg != nullptr ? errMsg : "unknown"));
        if (errMsg != nullptr)
            sqlite3_free (errMsg);
    }
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
