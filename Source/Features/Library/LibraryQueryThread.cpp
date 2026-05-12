#include "LibraryQueryThread.h"
#include <sqlite3.h>
#include <algorithm>

// =============================================================================
// Internal helpers
// =============================================================================

namespace
{

/// Whitelisted sort-column names.  Anything not in this set falls back to
/// "date_added" so user input can never be interpolated into SQL.
const char* kAllowedSortColumns[] = {
    "date_added", "bpm", "title", "artist", "album",
    "rating", "duration_seconds", "play_count", nullptr
};

bool isSafeColumn (const juce::String& col)
{
    for (int i = 0; kAllowedSortColumns[i] != nullptr; ++i)
        if (col == kAllowedSortColumns[i])
            return true;
    return false;
}

/// Retrieve a UTF-8 column value as a juce::String (null-safe).
juce::String colText (sqlite3_stmt* stmt, int col)
{
    const auto* raw = reinterpret_cast<const char*> (sqlite3_column_text (stmt, col));
    return raw != nullptr ? juce::String::fromUTF8 (raw) : juce::String{};
}

bool execSql (sqlite3* db, const char* sql)
{
    return sqlite3_exec (db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void rollback (sqlite3* db)
{
    sqlite3_exec (db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

void postMutation (LibraryQueryThread::PlaylistMutationCallback callback,
                   bool ok,
                   juce::String mutationMessage,
                   int64_t playlistId)
{
    if (!callback)
        return;

    juce::MessageManager::callAsync ([postedCallback = std::move (callback), ok,
                                      postedMessage = std::move (mutationMessage), playlistId]() mutable
    {
        postedCallback (ok, postedMessage, playlistId);
    });
}

bool playlistNameExists (sqlite3* db, const juce::String& name, int64_t excludedId = 0)
{
    const char* sql =
        "SELECT 1 FROM playlists "
        "WHERE name = ? COLLATE NOCASE AND (? = 0 OR id != ?) LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2 (db, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text  (stmt, 1, name.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 2, excludedId);
        sqlite3_bind_int64 (stmt, 3, excludedId);
        exists = sqlite3_step (stmt) == SQLITE_ROW;
        sqlite3_finalize (stmt);
    }
    return exists;
}

juce::String playlistTypeForId (sqlite3* db, int64_t playlistId)
{
    sqlite3_stmt* stmt = nullptr;
    juce::String type;
    if (sqlite3_prepare_v2 (db, "SELECT type FROM playlists WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64 (stmt, 1, playlistId);
        if (sqlite3_step (stmt) == SQLITE_ROW)
            type = colText (stmt, 0);
        sqlite3_finalize (stmt);
    }
    return type;
}

int64_t historyPlaylistId (sqlite3* db)
{
    sqlite3_stmt* stmt = nullptr;
    int64_t id = 0;
    if (sqlite3_prepare_v2 (db,
                            "SELECT id FROM playlists WHERE type = 'history' ORDER BY id LIMIT 1;",
                            -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step (stmt) == SQLITE_ROW)
            id = sqlite3_column_int64 (stmt, 0);
        sqlite3_finalize (stmt);
    }
    return id;
}

int maxPlaylistPosition (sqlite3* db, int64_t playlistId)
{
    sqlite3_stmt* stmt = nullptr;
    int pos = 0;
    if (sqlite3_prepare_v2 (db,
                            "SELECT COALESCE(MAX(position), 0) FROM playlist_tracks WHERE playlist_id = ?;",
                            -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64 (stmt, 1, playlistId);
        if (sqlite3_step (stmt) == SQLITE_ROW)
            pos = sqlite3_column_int (stmt, 0);
        sqlite3_finalize (stmt);
    }
    return pos;
}

bool insertPlaylistTrackIds (sqlite3* db, int64_t playlistId,
                             const std::vector<int64_t>& trackIds,
                             int startPosition)
{
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2 (db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    int position = startPosition;
    bool ok = true;
    for (auto trackId : trackIds)
    {
        sqlite3_reset (stmt);
        sqlite3_clear_bindings (stmt);
        sqlite3_bind_int64 (stmt, 1, playlistId);
        sqlite3_bind_int64 (stmt, 2, trackId);
        sqlite3_bind_int   (stmt, 3, position++);
        if (sqlite3_step (stmt) != SQLITE_DONE)
        {
            ok = false;
            break;
        }
    }

    sqlite3_finalize (stmt);
    return ok;
}

std::vector<int64_t> trackIdsForFilePath (sqlite3* db, const juce::String& filePath)
{
    std::vector<int64_t> ids;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (db, "SELECT id FROM library_tracks WHERE file_path = ?;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step (stmt) == SQLITE_ROW)
            ids.push_back (sqlite3_column_int64 (stmt, 0));
        sqlite3_finalize (stmt);
    }
    return ids;
}

std::vector<PlaylistInfo> fetchPlaylists (sqlite3* db)
{
    std::vector<PlaylistInfo> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT p.id, p.name, p.type, COUNT(pt.id) "
        "FROM playlists p "
        "LEFT JOIN playlist_tracks pt ON pt.playlist_id = p.id "
        "GROUP BY p.id, p.name, p.type, p.created_at "
        "ORDER BY CASE WHEN p.type = 'history' THEN 0 ELSE 1 END, p.created_at ASC, p.name COLLATE NOCASE ASC;";

    if (sqlite3_prepare_v2 (db, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step (stmt) == SQLITE_ROW)
        {
            PlaylistInfo info;
            info.id = sqlite3_column_int64 (stmt, 0);
            info.name = colText (stmt, 1);
            info.type = colText (stmt, 2);
            info.trackCount = sqlite3_column_int (stmt, 3);
            result.push_back (std::move (info));
        }
        sqlite3_finalize (stmt);
    }

    return result;
}

bool renumberPlaylist (sqlite3* db, int64_t playlistId)
{
    std::vector<int64_t> entryIds;
    sqlite3_stmt* select = nullptr;
    if (sqlite3_prepare_v2 (db,
                            "SELECT id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position ASC, id ASC;",
                            -1, &select, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int64 (select, 1, playlistId);
    while (sqlite3_step (select) == SQLITE_ROW)
        entryIds.push_back (sqlite3_column_int64 (select, 0));
    sqlite3_finalize (select);

    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2 (db, "UPDATE playlist_tracks SET position = ? WHERE id = ?;",
                            -1, &update, nullptr) != SQLITE_OK)
        return false;

    bool ok = true;
    for (size_t i = 0; i < entryIds.size(); ++i)
    {
        sqlite3_reset (update);
        sqlite3_clear_bindings (update);
        sqlite3_bind_int   (update, 1, static_cast<int> (i + 1));
        sqlite3_bind_int64 (update, 2, entryIds[i]);
        if (sqlite3_step (update) != SQLITE_DONE)
        {
            ok = false;
            break;
        }
    }

    sqlite3_finalize (update);
    return ok;
}

} // namespace

struct LibraryQueryThread::SqlBind
{
    enum class Type { Text, Double, Int64 } type = Type::Text;
    std::string text;
    double doubleValue = 0.0;
    int64_t intValue = 0;

    static SqlBind textValue (const juce::String& value)
    {
        SqlBind bind;
        bind.type = Type::Text;
        bind.text = value.toStdString();
        return bind;
    }

    static SqlBind doubleValueOf (double value)
    {
        SqlBind bind;
        bind.type = Type::Double;
        bind.doubleValue = value;
        return bind;
    }

    static SqlBind intValueOf (int64_t value)
    {
        SqlBind bind;
        bind.type = Type::Int64;
        bind.intValue = value;
        return bind;
    }
};

// =============================================================================
// Construction / destruction
// =============================================================================

LibraryQueryThread::LibraryQueryThread (TrackDatabase& dbRef)
    : juce::Thread ("LibraryQueryThread")
    , db     (dbRef)
    , dbFile (dbRef.getDbFile())
    , wakeEvent (false) // auto-reset (false = manualReset off), initially unsignalled
{
    startThread (juce::Thread::Priority::low);
}

LibraryQueryThread::~LibraryQueryThread()
{
    // Signal BEFORE stopThread so the blocking wakeEvent.wait() is released
    // and the thread can observe threadShouldExit().
    wakeEvent.signal();
    stopThread (3000);
}

// =============================================================================
// Message-Thread API
// =============================================================================

void LibraryQueryThread::setResultCallback (ResultCallback cb)
{
    juce::ScopedLock sl (callbackLock);
    resultCallback = std::move (cb);
}

void LibraryQueryThread::dispatchQuery (QueryParams params)
{
    {
        std::lock_guard<std::mutex> lock (pendingMutex);
        pendingParams = std::move (params);
        hasPending    = true;
        cancelFlag.store (true, std::memory_order_release);
    }
    wakeEvent.signal();
}

void LibraryQueryThread::updateDeckFilter (DeckAwareFilterState state)
{
    // Nested scope ensures the mutex is released before signalling wakeEvent.
    {
        std::lock_guard<std::mutex> lock (pendingMutex);
        pendingParams.deckFilter = std::move (state);
        hasPending               = true;
        cancelFlag.store (true, std::memory_order_release);
    }
    wakeEvent.signal();
}

void LibraryQueryThread::setSortColumn (const juce::String& column, bool ascending)
{
    {
        std::lock_guard<std::mutex> lock (pendingMutex);
        pendingParams.sortColumn    = isSafeColumn (column) ? column : "date_added";
        pendingParams.sortAscending = ascending;
        hasPending                  = true;
        cancelFlag.store (true, std::memory_order_release);
    }
    wakeEvent.signal();
}

void LibraryQueryThread::enqueueOperation (std::function<void(sqlite3*)> operation)
{
    {
        std::lock_guard<std::mutex> lock (operationMutex);
        operations.push_back (std::move (operation));
    }
    wakeEvent.signal();
}

void LibraryQueryThread::requestPlaylists (PlaylistListCallback callbackIn)
{
    enqueueOperation ([listCallback = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        auto fetchedPlaylists = fetchPlaylists (handle);
        if (listCallback)
        {
            juce::MessageManager::callAsync ([postedCallback = std::move (listCallback),
                                              postedPlaylists = std::move (fetchedPlaylists)]() mutable
            {
                postedCallback (std::move (postedPlaylists));
            });
        }
    });
}

void LibraryQueryThread::createPlaylist (juce::String name, PlaylistMutationCallback callback)
{
    createPlaylistWithTracks (std::move (name), {}, std::move (callback));
}

void LibraryQueryThread::createPlaylistWithTracks (juce::String nameIn,
                                                   std::vector<int64_t> trackIdsIn,
                                                   PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistName = std::move (nameIn), idsToAdd = std::move (trackIdsIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        const auto trimmedName = playlistName.trim();
        if (trimmedName.isEmpty())
        {
            postMutation (std::move (completion), false, "Playlist name is required", 0);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", 0);
            return;
        }

        if (playlistNameExists (handle, trimmedName))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "A playlist with this name already exists", 0);
            return;
        }

        sqlite3_stmt* insert = nullptr;
        const char* sql =
            "INSERT INTO playlists (name, type, created_at) "
            "VALUES (?, 'normal', CAST(strftime('%s', 'now') AS INTEGER));";
        if (sqlite3_prepare_v2 (handle, sql, -1, &insert, nullptr) != SQLITE_OK)
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not create playlist", 0);
            return;
        }

        sqlite3_bind_text (insert, 1, trimmedName.toRawUTF8(), -1, SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step (insert) == SQLITE_DONE;
        sqlite3_finalize (insert);

        if (!inserted)
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not create playlist", 0);
            return;
        }

        const int64_t playlistId = sqlite3_last_insert_rowid (handle);
        if (!idsToAdd.empty() && !insertPlaylistTrackIds (handle, playlistId, idsToAdd, 1))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not add tracks to playlist", 0);
            return;
        }

        if (!execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not save playlist", 0);
            return;
        }

        postMutation (std::move (completion), true, {}, playlistId);
    });
}

void LibraryQueryThread::renamePlaylist (int64_t playlistId, juce::String newNameIn,
                                         PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, requestedName = std::move (newNameIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        const auto trimmedName = requestedName.trim();
        if (trimmedName.isEmpty())
        {
            postMutation (std::move (completion), false, "Playlist name is required", playlistId);
            return;
        }

        if (playlistTypeForId (handle, playlistId) != "normal")
        {
            postMutation (std::move (completion), false, "Only normal playlists can be renamed", playlistId);
            return;
        }

        if (playlistNameExists (handle, trimmedName, playlistId))
        {
            postMutation (std::move (completion), false, "A playlist with this name already exists", playlistId);
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (handle, "UPDATE playlists SET name = ? WHERE id = ? AND type = 'normal';",
                                -1, &stmt, nullptr) != SQLITE_OK)
        {
            postMutation (std::move (completion), false, "Could not rename playlist", playlistId);
            return;
        }

        sqlite3_bind_text  (stmt, 1, trimmedName.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 2, playlistId);
        const bool ok = sqlite3_step (stmt) == SQLITE_DONE && sqlite3_changes (handle) > 0;
        sqlite3_finalize (stmt);
        postMutation (std::move (completion), ok, ok ? juce::String{} : "Could not rename playlist", playlistId);
    });
}

void LibraryQueryThread::deletePlaylist (int64_t playlistId, PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", playlistId);
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (handle, "DELETE FROM playlists WHERE id = ? AND type = 'normal';",
                                -1, &stmt, nullptr) != SQLITE_OK)
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not delete playlist", playlistId);
            return;
        }

        sqlite3_bind_int64 (stmt, 1, playlistId);
        const bool ok = sqlite3_step (stmt) == SQLITE_DONE && sqlite3_changes (handle) > 0;
        sqlite3_finalize (stmt);

        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Only normal playlists can be deleted", playlistId);
            return;
        }

        postMutation (std::move (completion), ok, ok ? juce::String{} : "Only normal playlists can be deleted", playlistId);
    });
}

void LibraryQueryThread::addTracksToPlaylist (int64_t playlistId, std::vector<int64_t> trackIdsIn,
                                              PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, idsToAdd = std::move (trackIdsIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        if (idsToAdd.empty())
        {
            postMutation (std::move (completion), true, {}, playlistId);
            return;
        }

        if (playlistTypeForId (handle, playlistId) != "normal")
        {
            postMutation (std::move (completion), false, "Tracks can only be added to normal playlists", playlistId);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", playlistId);
            return;
        }

        const auto startPosition = maxPlaylistPosition (handle, playlistId) + 1;
        const bool ok = insertPlaylistTrackIds (handle, playlistId, idsToAdd, startPosition);
        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not add tracks to playlist", playlistId);
            return;
        }

        postMutation (std::move (completion), true, {}, playlistId);
    });
}

void LibraryQueryThread::addFilePathToPlaylist (int64_t playlistId, juce::String filePathIn,
                                                PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, requestedPath = std::move (filePathIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        auto idsToAdd = trackIdsForFilePath (handle, requestedPath);
        if (idsToAdd.empty())
        {
            postMutation (std::move (completion), false, "Track is no longer in the library", playlistId);
            return;
        }

        if (playlistTypeForId (handle, playlistId) != "normal")
        {
            postMutation (std::move (completion), false, "Tracks can only be added to normal playlists", playlistId);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", playlistId);
            return;
        }

        const auto startPosition = maxPlaylistPosition (handle, playlistId) + 1;
        const bool ok = insertPlaylistTrackIds (handle, playlistId, idsToAdd, startPosition);
        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not add tracks to playlist", playlistId);
            return;
        }

        postMutation (std::move (completion), true, {}, playlistId);
    });
}

void LibraryQueryThread::removePlaylistEntries (int64_t playlistId, std::vector<int64_t> entryIdsIn,
                                                PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, idsToRemove = std::move (entryIdsIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        if (idsToRemove.empty())
        {
            postMutation (std::move (completion), true, {}, playlistId);
            return;
        }

        if (playlistTypeForId (handle, playlistId) != "normal")
        {
            postMutation (std::move (completion), false, "Tracks can only be removed from normal playlists", playlistId);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", playlistId);
            return;
        }

        juce::String sql = "DELETE FROM playlist_tracks WHERE playlist_id = ? AND id IN (";
        for (size_t i = 0; i < idsToRemove.size(); ++i)
            sql << (i == 0 ? "?" : ",?");
        sql << ");";

        sqlite3_stmt* stmt = nullptr;
        bool ok = sqlite3_prepare_v2 (handle, sql.toRawUTF8(), -1, &stmt, nullptr) == SQLITE_OK;
        if (ok)
        {
            sqlite3_bind_int64 (stmt, 1, playlistId);
            for (size_t i = 0; i < idsToRemove.size(); ++i)
                sqlite3_bind_int64 (stmt, static_cast<int> (i + 2), idsToRemove[i]);
            ok = sqlite3_step (stmt) == SQLITE_DONE;
            sqlite3_finalize (stmt);
        }

        ok = ok && renumberPlaylist (handle, playlistId);
        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not remove tracks from playlist", playlistId);
            return;
        }

        postMutation (std::move (completion), true, {}, playlistId);
    });
}

void LibraryQueryThread::movePlaylistEntry (int64_t playlistId, int64_t entryId, int newZeroBasedIndex,
                                            PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([playlistId, entryId, newZeroBasedIndex,
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        if (playlistTypeForId (handle, playlistId) != "normal")
        {
            postMutation (std::move (completion), false, "Only normal playlists can be reordered", playlistId);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start playlist update", playlistId);
            return;
        }

        std::vector<int64_t> ids;
        sqlite3_stmt* select = nullptr;
        bool ok = sqlite3_prepare_v2 (handle,
                                      "SELECT id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position ASC, id ASC;",
                                      -1, &select, nullptr) == SQLITE_OK;
        if (ok)
        {
            sqlite3_bind_int64 (select, 1, playlistId);
            while (sqlite3_step (select) == SQLITE_ROW)
                ids.push_back (sqlite3_column_int64 (select, 0));
            sqlite3_finalize (select);
        }

        auto it = std::find (ids.begin(), ids.end(), entryId);
        if (it == ids.end())
            ok = false;
        else
        {
            ids.erase (it);
            const auto clamped = juce::jlimit (0, static_cast<int> (ids.size()), newZeroBasedIndex);
            ids.insert (ids.begin() + clamped, entryId);
        }

        sqlite3_stmt* update = nullptr;
        ok = ok && sqlite3_prepare_v2 (handle, "UPDATE playlist_tracks SET position = ? WHERE id = ? AND playlist_id = ?;",
                                       -1, &update, nullptr) == SQLITE_OK;
        if (ok)
        {
            for (size_t i = 0; i < ids.size(); ++i)
            {
                sqlite3_reset (update);
                sqlite3_clear_bindings (update);
                sqlite3_bind_int   (update, 1, static_cast<int> (i + 1));
                sqlite3_bind_int64 (update, 2, ids[i]);
                sqlite3_bind_int64 (update, 3, playlistId);
                if (sqlite3_step (update) != SQLITE_DONE)
                {
                    ok = false;
                    break;
                }
            }
            sqlite3_finalize (update);
        }

        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not reorder playlist", playlistId);
            return;
        }

        postMutation (std::move (completion), true, {}, playlistId);
    });
}

void LibraryQueryThread::appendHistoryEntryForFilePath (juce::String filePathIn,
                                                        PlaylistMutationCallback callbackIn)
{
    enqueueOperation ([requestedPath = std::move (filePathIn),
                       completion = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        const auto ids = trackIdsForFilePath (handle, requestedPath);
        if (ids.empty())
        {
            postMutation (std::move (completion), false, "Track is no longer in the library", 0);
            return;
        }

        const auto historyId = historyPlaylistId (handle);
        if (historyId <= 0)
        {
            postMutation (std::move (completion), false, "History playlist is missing", 0);
            return;
        }

        if (!execSql (handle, "BEGIN IMMEDIATE;"))
        {
            postMutation (std::move (completion), false, "Could not start history update", historyId);
            return;
        }

        sqlite3_stmt* insert = nullptr;
        const char* insertSql =
            "INSERT INTO playlist_tracks (playlist_id, track_id, position, played_at) "
            "VALUES (?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));";
        bool ok = sqlite3_prepare_v2 (handle, insertSql, -1, &insert, nullptr) == SQLITE_OK;
        if (ok)
        {
            sqlite3_bind_int64 (insert, 1, historyId);
            sqlite3_bind_int64 (insert, 2, ids.front());
            sqlite3_bind_int   (insert, 3, maxPlaylistPosition (handle, historyId) + 1);
            ok = sqlite3_step (insert) == SQLITE_DONE;
            sqlite3_finalize (insert);
        }

        sqlite3_stmt* prune = nullptr;
        const char* pruneSql =
            "DELETE FROM playlist_tracks "
            "WHERE playlist_id = ? AND id NOT IN ("
            "  SELECT id FROM playlist_tracks "
            "  WHERE playlist_id = ? "
            "  ORDER BY played_at DESC, id DESC "
            "  LIMIT 500"
            ");";
        ok = ok && sqlite3_prepare_v2 (handle, pruneSql, -1, &prune, nullptr) == SQLITE_OK;
        if (ok)
        {
            sqlite3_bind_int64 (prune, 1, historyId);
            sqlite3_bind_int64 (prune, 2, historyId);
            ok = sqlite3_step (prune) == SQLITE_DONE;
            sqlite3_finalize (prune);
        }

        if (!ok || !execSql (handle, "COMMIT;"))
        {
            rollback (handle);
            postMutation (std::move (completion), false, "Could not update History", historyId);
            return;
        }

        postMutation (std::move (completion), true, {}, historyId);
    });
}

void LibraryQueryThread::countMissingTracks (CountCallback callbackIn)
{
    if (! callbackIn)
        return;

    enqueueOperation ([cb = std::move (callbackIn)] (sqlite3* handle) mutable
    {
        int count = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (handle,
                "SELECT COUNT(*) FROM library_tracks WHERE is_missing = 1;",
                -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                count = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }

        juce::MessageManager::callAsync ([postedCb = std::move (cb), count]() mutable
        {
            postedCb (count);
        });
    });
}

// =============================================================================
// Static: scope operator parser
// =============================================================================

// static
QueryParams LibraryQueryThread::parseSearchString (const juce::String& raw)
{
    QueryParams p;
    p.rawSearchString = raw;

    juce::StringArray tokens;
    tokens.addTokens (raw.trim(), " ", "\"");

    juce::StringArray bareWords;

    for (const auto& token : tokens)
    {
        if (token.isEmpty())
            continue;

        if (token.startsWith ("bpm:"))
        {
            const auto val = token.fromFirstOccurrenceOf ("bpm:", false, false);
            if (val.contains ("-"))
            {
                p.bpmMin = val.upToFirstOccurrenceOf   ("-", false, false).getDoubleValue();
                p.bpmMax = val.fromFirstOccurrenceOf   ("-", false, false).getDoubleValue();
            }
            else
            {
                const double centre = val.getDoubleValue();
                p.bpmMin = centre - 0.5;
                p.bpmMax = centre + 0.5;
            }
            p.hasBpmRange = true;
        }
        else if (token.startsWith ("key:"))
        {
            const auto keyStr = token.fromFirstOccurrenceOf ("key:", false, false);
            p.keyIndex    = camelotKeyToIndex (keyStr);
            p.hasKeyFilter = (p.keyIndex >= 0);
        }
        else if (token.startsWith ("rating:"))
        {
            p.ratingMin      = token.fromFirstOccurrenceOf ("rating:", false, false).getIntValue();
            p.hasRatingFilter = (p.ratingMin >= 1 && p.ratingMin <= 5);
        }
        else if (token.startsWith ("title:"))
        {
            p.ftsTitleTerm = token.fromFirstOccurrenceOf ("title:", false, false);
        }
        else if (token.startsWith ("artist:"))
        {
            p.ftsArtistTerm = token.fromFirstOccurrenceOf ("artist:", false, false);
        }
        else if (token.startsWith ("album:"))
        {
            p.ftsAlbumTerm = token.fromFirstOccurrenceOf ("album:", false, false);
        }
        else
        {
            bareWords.add (token);
        }
    }

    p.ftsTerms = bareWords.joinIntoString (" ");
    return p;
}

// static
int LibraryQueryThread::camelotKeyToIndex (const juce::String& key)
{
    const auto upper = key.trim().toUpperCase();
    const bool isB   = upper.endsWithChar ('B');
    const bool isA   = upper.endsWithChar ('A');
    if (!isA && !isB)
        return -1;
    const int num = upper.dropLastCharacters (1).getIntValue();
    if (num < 1 || num > 12)
        return -1;
    return isA ? (num - 1) : (num - 1 + 12);
}

// =============================================================================
// juce::Thread entry point
// =============================================================================

void LibraryQueryThread::run()
{
    // Open a dedicated sqlite3 connection — never shared with Message Thread.
    sqlite3* bgDb = nullptr;
    if (sqlite3_open (dbFile.getFullPathName().toRawUTF8(), &bgDb) != SQLITE_OK)
    {
        DBG ("LibraryQueryThread: failed to open background DB connection");
        return;
    }

    // WAL mode + FK enforcement + busy timeout so we retry on SQLITE_BUSY.
    sqlite3_exec (bgDb, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec (bgDb, "PRAGMA foreign_keys=ON;",    nullptr, nullptr, nullptr);
    sqlite3_exec (bgDb, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout (bgDb, 50); // 50 ms max wait per step

    // Dispatch an initial blank query to populate the full library on startup.
    {
        std::lock_guard<std::mutex> lock (pendingMutex);
        hasPending = true;
        // pendingParams defaults are fine: empty search, date_added DESC.
    }
    wakeEvent.signal(); // Wake ourselves to run the initial blank query.

    // -------------------------------------------------------------------------
    // Query-dispatch loop
    // -------------------------------------------------------------------------
    while (!threadShouldExit())
    {
        wakeEvent.wait (-1); // Sleep until dispatchQuery() or destructor wakes us.

        if (threadShouldExit())
            break;

        while (true)
        {
            std::deque<std::function<void(sqlite3*)>> pendingOperations;
            {
                std::lock_guard<std::mutex> lock (operationMutex);
                if (operations.empty())
                    break;
                pendingOperations.swap (operations);
            }

            for (auto& op : pendingOperations)
            {
                if (threadShouldExit())
                    break;
                op (bgDb);
            }

            if (threadShouldExit())
                break;
        }

        if (threadShouldExit())
            break;

        // Drain all pending queries, keeping only the latest.
        while (true)
        {
            QueryParams params;
            {
                std::lock_guard<std::mutex> lock (pendingMutex);
                if (!hasPending)
                    break;
                params     = pendingParams;
                hasPending = false;
            }

            cancelFlag.store (false, std::memory_order_release);
            executeQuery (bgDb, params);

            if (threadShouldExit())
                break;
        }
    }

    sqlite3_close (bgDb);
}

// =============================================================================
// buildSql
// =============================================================================

juce::String LibraryQueryThread::buildSql (const QueryParams& params, std::vector<SqlBind>& binds)
{
    // -------------------------------------------------------------------------
    // Determine whether an FTS subquery is needed.
    // -------------------------------------------------------------------------
    const bool hasFts = params.ftsTerms.isNotEmpty()
                     || params.ftsTitleTerm.isNotEmpty()
                     || params.ftsArtistTerm.isNotEmpty()
                     || params.ftsAlbumTerm.isNotEmpty();

    // Build the FTS MATCH expression if needed.
    // All parts joined with AND; the wildcard (*) is appended by code, not
    // user input, so it cannot be exploited for injection.
    juce::String ftsMatchExpr;
    if (hasFts)
    {
        juce::StringArray ftsParts;

        if (params.ftsTerms.isNotEmpty())
        {
            // Multiple bare words: each word becomes an AND-ed prefix term.
            juce::StringArray words;
            words.addTokens (params.ftsTerms, " ", "\"");
            for (const auto& w : words)
                if (w.isNotEmpty())
                    ftsParts.add (w + "*");
        }

        if (params.ftsTitleTerm.isNotEmpty())
            ftsParts.add ("title:" + params.ftsTitleTerm + "*");

        if (params.ftsArtistTerm.isNotEmpty())
            ftsParts.add ("artist:" + params.ftsArtistTerm + "*");

        if (params.ftsAlbumTerm.isNotEmpty())
            ftsParts.add ("album:" + params.ftsAlbumTerm + "*");

        ftsMatchExpr = ftsParts.joinIntoString (" AND ");
    }

    // -------------------------------------------------------------------------
    // Base SELECT
    // -------------------------------------------------------------------------
    const bool isPlaylistView = params.playlistId > 0;
    const bool isPreparationView = params.playlistType == "preparation";

    juce::String sql =
        "SELECT lt.id, lt.file_path, lt.content_hash, lt.title, lt.artist, lt.album,"
        "       lt.bpm, lt.key, lt.key_index, lt.duration_seconds, lt.file_size_bytes,"
        "       lt.date_added, lt.is_missing, lt.play_count, lt.rating,";

    if (isPlaylistView)
    {
        sql += "       pt.id, pt.position, COALESCE(pt.played_at, '') FROM playlist_tracks pt JOIN library_tracks lt ON lt.id = pt.track_id";
    }
    else if (isPreparationView && !params.preparationTrackIds.empty())
    {
        sql += "       0, prep.prep_order + 1, '' FROM (";
        for (int i = 0; i < static_cast<int> (params.preparationTrackIds.size()); ++i)
        {
            sql += (i == 0 ? "SELECT ? AS prep_order, ? AS track_id"
                           : " UNION ALL SELECT ?, ?");
            binds.push_back (SqlBind::intValueOf (i));
            binds.push_back (SqlBind::intValueOf (params.preparationTrackIds[static_cast<size_t> (i)]));
        }
        sql += ") prep JOIN library_tracks lt ON lt.id = prep.track_id";
    }
    else
    {
        sql += "       0, 0, '' FROM library_tracks lt";
    }

    // -------------------------------------------------------------------------
    // WHERE clauses
    // -------------------------------------------------------------------------
    juce::StringArray whereClauses;

    // FTS MATCH via correlated subquery — avoids FTS5 JOIN alias ambiguity.
    // The wildcard is appended by this code, not supplied by the user.
    if (hasFts)
    {
        whereClauses.add ("lt.id IN (SELECT rowid FROM library_fts WHERE library_fts MATCH ?)");
        binds.push_back (SqlBind::textValue (ftsMatchExpr));
    }

    if (isPlaylistView)
    {
        whereClauses.add ("pt.playlist_id = ?");
        binds.push_back (SqlBind::intValueOf (params.playlistId));
    }

    if (isPreparationView)
    {
        if (params.preparationTrackIds.empty())
            whereClauses.add ("0 = 1");
    }

    if (params.folderPath.isNotEmpty())
    {
        auto folderPrefix = params.folderPath.trimCharactersAtEnd ("/") + "/%";
        whereClauses.add ("lt.file_path LIKE ?");
        binds.push_back (SqlBind::textValue (folderPrefix));
    }

    // Explicit BPM range from search string
    if (params.hasBpmRange)
    {
        whereClauses.add ("lt.bpm BETWEEN ? AND ?");
        binds.push_back (SqlBind::doubleValueOf (params.bpmMin));
        binds.push_back (SqlBind::doubleValueOf (params.bpmMax));
    }

    // Key filter from search string
    if (params.hasKeyFilter)
    {
        whereClauses.add ("lt.key_index = ?");
        binds.push_back (SqlBind::intValueOf (params.keyIndex));
    }

    // Rating filter
    if (params.hasRatingFilter)
    {
        whereClauses.add ("lt.rating >= ?");
        binds.push_back (SqlBind::intValueOf (params.ratingMin));
    }

    // Missing-only filter (PRD-0039 AC-29)
    if (params.showMissingOnly)
    {
        whereClauses.add ("lt.is_missing = 1");
    }

    // Deck-aware BPM windows (union)
    if (params.deckFilter.bpmMatchActive && !params.deckFilter.bpmWindows.isEmpty())
    {
        juce::StringArray windowParts;
        for (const auto& [lo, hi] : params.deckFilter.bpmWindows)
        {
            windowParts.add ("lt.bpm BETWEEN ? AND ?");
            binds.push_back (SqlBind::doubleValueOf (lo));
            binds.push_back (SqlBind::doubleValueOf (hi));
        }
        whereClauses.add ("(" + windowParts.joinIntoString (" OR ") + ")");
    }

    // Deck-aware key IN-list
    if (params.deckFilter.keyMatchActive && !params.deckFilter.compatibleKeyIndices.isEmpty())
    {
        juce::StringArray placeholders;
        for (int idx : params.deckFilter.compatibleKeyIndices)
        {
            placeholders.add ("?");
            binds.push_back (SqlBind::intValueOf (idx));
        }
        whereClauses.add ("lt.key_index IN (" + placeholders.joinIntoString (", ") + ")");
    }

    if (!whereClauses.isEmpty())
        sql += " WHERE " + whereClauses.joinIntoString (" AND ");

    // -------------------------------------------------------------------------
    // ORDER BY (whitelisted column only)
    // -------------------------------------------------------------------------
    if (isPlaylistView && params.playlistType == "history")
    {
        sql += " ORDER BY pt.played_at DESC, pt.id DESC";
    }
    else if (isPlaylistView)
    {
        sql += " ORDER BY pt.position ASC, pt.id ASC";
    }
    else if (isPreparationView)
    {
        sql += params.preparationTrackIds.empty() ? " ORDER BY lt.date_added DESC" : " ORDER BY prep.prep_order ASC";
    }
    else
    {
        const juce::String safeCol = isSafeColumn (params.sortColumn)
                                         ? params.sortColumn
                                         : "date_added";
        sql += " ORDER BY lt." + safeCol;
        sql += params.sortAscending ? " ASC" : " DESC";
    }

    return sql;
}

// =============================================================================
// executeQuery
// =============================================================================

void LibraryQueryThread::executeQuery (sqlite3* bgDb, const QueryParams& params)
{
    // Reset the cancellation flag at the start of each new query.
    cancelFlag.store (false, std::memory_order_release);

    // Build SQL and binding list in placeholder order.
    std::vector<SqlBind> binds;
    const juce::String sql = buildSql (params, binds);

    // Prepare statement.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (bgDb, sql.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        DBG ("LibraryQueryThread: prepare failed: " + juce::String (sqlite3_errmsg (bgDb)));
        DBG ("LibraryQueryThread: SQL was: " + sql);
        return;
    }

    int bindIdx = 1;
    for (const auto& bind : binds)
    {
        switch (bind.type)
        {
            case SqlBind::Type::Text:
                sqlite3_bind_text (stmt, bindIdx++, bind.text.c_str(), -1, SQLITE_TRANSIENT);
                break;
            case SqlBind::Type::Double:
                sqlite3_bind_double (stmt, bindIdx++, bind.doubleValue);
                break;
            case SqlBind::Type::Int64:
                sqlite3_bind_int64 (stmt, bindIdx++, bind.intValue);
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Step through rows with cancellation checks.
    // -------------------------------------------------------------------------
    std::vector<LibraryTrackRow> results;

    int busyRetries = 0;

    while (true)
    {
        if (cancelFlag.load (std::memory_order_acquire) || threadShouldExit())
        {
            sqlite3_finalize (stmt);
            return; // Do NOT post results for a cancelled query.
        }

        const int rc = sqlite3_step (stmt);

        if (rc == SQLITE_ROW)
        {
            busyRetries = 0; // Reset retry counter on success.

            LibraryTrackRow row;
            row.id              = sqlite3_column_int64  (stmt, 0);
            row.filePath        = colText (stmt, 1);
            row.contentHash     = colText (stmt, 2);
            row.title           = colText (stmt, 3);
            row.artist          = colText (stmt, 4);
            row.album           = colText (stmt, 5);
            row.bpm             = sqlite3_column_double (stmt, 6);
            row.key             = colText (stmt, 7);
            row.keyIndex        = sqlite3_column_int    (stmt, 8);
            row.durationSeconds = sqlite3_column_double (stmt, 9);
            row.fileSizeBytes   = sqlite3_column_int64  (stmt, 10);
            row.dateAdded       = sqlite3_column_int64  (stmt, 11);
            row.isMissing       = sqlite3_column_int    (stmt, 12);
            row.playCount       = sqlite3_column_int    (stmt, 13);
            row.rating          = sqlite3_column_int    (stmt, 14);
            row.playlistEntryId = sqlite3_column_int64  (stmt, 15);
            row.playlistPosition = sqlite3_column_int   (stmt, 16);
            row.playedAt        = colText (stmt, 17);
            results.push_back (std::move (row));
        }
        else if (rc == SQLITE_DONE)
        {
            break; // All rows read cleanly.
        }
        else if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
        {
            ++busyRetries;
            if (busyRetries > 5)
            {
                DBG ("LibraryQueryThread: SQLITE_BUSY exceeded retry limit — query abandoned");
                sqlite3_finalize (stmt);
                return;
            }
            // sqlite3_busy_timeout(50) is already set; this sleep is additional
            // defence if the timeout was not honoured (e.g. locked by writer).
            juce::Thread::sleep (10);
        }
        else
        {
            DBG ("LibraryQueryThread: sqlite3_step error: " + juce::String (sqlite3_errmsg (bgDb)));
            break;
        }
    }

    sqlite3_finalize (stmt);

    // Final cancellation check before posting.
    if (cancelFlag.load (std::memory_order_acquire) || threadShouldExit())
        return;

    // -------------------------------------------------------------------------
    // Post results to Message Thread via callAsync.
    // -------------------------------------------------------------------------
    juce::WeakReference<LibraryQueryThread> weakThis (this);
    juce::MessageManager::callAsync (
        [weakThis, rows = std::move (results)]() mutable
        {
            if (auto* self = weakThis.get())
            {
                juce::ScopedLock sl (self->callbackLock);
                if (self->resultCallback)
                    self->resultCallback (std::move (rows));
            }
        });
}
