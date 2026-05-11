#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Deck/Database/TrackDatabase.h"
#include <sqlite3.h>

class LibraryDatabaseSchemaTests : public juce::UnitTest
{
public:
    LibraryDatabaseSchemaTests()
        : juce::UnitTest ("Library Database Schema and Migration", "Sonik") {}

    void runTest() override
    {
        testWalMode();
        testForeignKeysEnabled();
        testSchemaVersionTable();
        testLibraryTracksColumns();
        testLibraryFtsVirtualTable();
        testWatchedFoldersTable();
        testPlaylistsTable();
        testPlaylistTracksTable();
        testPlaylistTracksAllowDuplicates();
        testHistoryPlaylistSeeded();
        testPlaylistNamesAreCaseInsensitiveUnique();
        testTriggersExist();
        testIdempotency();
        testFtsInsertTrigger();
        testFtsDeleteTrigger();
        testFtsUpdateTrigger();
        testAnalysisProjectionUpdatesByFilePath();
        testExistingTablesUntouched();
        testForeignKeyCascade();
    }

private:
    // -----------------------------------------------------------------------
    // Utility: query a single text value, empty string if no row
    juce::String queryText (sqlite3* h, const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        juce::String result;
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
            {
                const auto* text = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
                if (text != nullptr)
                    result = juce::String::fromUTF8 (text);
            }
            sqlite3_finalize (stmt);
        }
        return result;
    }

    // Utility: query a single integer value, -1 if no row / error
    int queryInt (sqlite3* h, const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        int result = -1;
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                result = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return result;
    }

    // Utility: query a single double value, -1.0 if no row / error
    double queryDouble (sqlite3* h, const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        double result = -1.0;
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                result = sqlite3_column_double (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return result;
    }

    // Utility: execute DML/DDL, returns SQLITE result code
    int execSql (sqlite3* h, const char* sql)
    {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec (h, sql, nullptr, nullptr, &errMsg);
        if (errMsg != nullptr) sqlite3_free (errMsg);
        return rc;
    }

    // Utility: true if a table/view exists in sqlite_master
    bool tableExists (sqlite3* h, const char* tableName)
    {
        const juce::String sql =
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type IN ('table','view') AND name='" + juce::String (tableName) + "';";
        return queryInt (h, sql.toRawUTF8()) > 0;
    }

    // Utility: true if a named column exists in PRAGMA table_info
    bool columnExists (sqlite3* h, const char* tableName, const char* columnName)
    {
        const juce::String sql =
            juce::String ("PRAGMA table_info(") + tableName + ");";
        sqlite3_stmt* stmt = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2 (h, sql.toRawUTF8(), -1, &stmt, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step (stmt) == SQLITE_ROW)
            {
                const auto* name = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
                if (name != nullptr && juce::String::fromUTF8 (name) == juce::String (columnName))
                {
                    found = true;
                    break;
                }
            }
            sqlite3_finalize (stmt);
        }
        return found;
    }

    // Utility: true if a trigger with the given name exists
    bool triggerExists (sqlite3* h, const char* triggerName)
    {
        const juce::String sql =
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='trigger' AND name='" + juce::String (triggerName) + "';";
        return queryInt (h, sql.toRawUTF8()) > 0;
    }

    // RAII context: creates a temp DB file and opens TrackDatabase on it
    struct DbContext
    {
        juce::File              dbFile;
        std::unique_ptr<TrackDatabase> db;

        DbContext()
        {
            dbFile = juce::File::createTempFile ("sonik_lib_schema_test.db");
            db     = std::make_unique<TrackDatabase> (dbFile);
        }

        ~DbContext()
        {
            db.reset();
            dbFile.deleteFile();
        }
    };

    // -----------------------------------------------------------------------
    // 1. WAL mode
    // -----------------------------------------------------------------------
    void testWalMode()
    {
        beginTest ("WAL mode - PRAGMA journal_mode returns 'wal' after construction");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (h != nullptr);
        const juce::String mode = queryText (h, "PRAGMA journal_mode;");
        expectEquals (mode, juce::String ("wal"));
    }

    // -----------------------------------------------------------------------
    // 2. Foreign keys enabled
    // -----------------------------------------------------------------------
    void testForeignKeysEnabled()
    {
        beginTest ("Foreign keys - PRAGMA foreign_keys returns 1");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        const int fk = queryInt (h, "PRAGMA foreign_keys;");
        expectEquals (fk, 1);
    }

    // -----------------------------------------------------------------------
    // 3. schema_version table exists and is seeded with version 2
    // -----------------------------------------------------------------------
    void testSchemaVersionTable()
    {
        beginTest ("schema_version - table exists after construction");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "schema_version"));

        beginTest ("schema_version - version row equals 2");
        const int version = queryInt (h, "SELECT version FROM schema_version LIMIT 1;");
        expectEquals (version, 2);
    }

    // -----------------------------------------------------------------------
    // 4. library_tracks columns
    // -----------------------------------------------------------------------
    void testLibraryTracksColumns()
    {
        beginTest ("library_tracks - table exists");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "library_tracks"));

        static const char* const requiredCols[] = {
            "id", "file_path", "content_hash",
            "title", "artist", "album",
            "bpm", "key", "key_index",
            "duration_seconds", "file_size_bytes",
            "date_added", "last_seen",
            "is_missing", "play_count", "rating"
        };
        for (auto* col : requiredCols)
        {
            beginTest (juce::String ("library_tracks - column '") + col + "' exists");
            expect (columnExists (h, "library_tracks", col));
        }
    }

    // -----------------------------------------------------------------------
    // 5. library_fts virtual table exists and is FTS5
    // -----------------------------------------------------------------------
    void testLibraryFtsVirtualTable()
    {
        beginTest ("library_fts - virtual table exists");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "library_fts"));

        beginTest ("library_fts - CREATE SQL references fts5");
        const juce::String createSql =
            queryText (h, "SELECT sql FROM sqlite_master WHERE name='library_fts';");
        expect (createSql.containsIgnoreCase ("fts5"));
    }

    // -----------------------------------------------------------------------
    // 6. watched_folders columns
    // -----------------------------------------------------------------------
    void testWatchedFoldersTable()
    {
        beginTest ("watched_folders - table exists");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "watched_folders"));

        static const char* const requiredCols[] = { "id", "folder_path", "last_scanned_at" };
        for (auto* col : requiredCols)
        {
            beginTest (juce::String ("watched_folders - column '") + col + "' exists");
            expect (columnExists (h, "watched_folders", col));
        }
    }

    // -----------------------------------------------------------------------
    // 7. playlists columns
    // -----------------------------------------------------------------------
    void testPlaylistsTable()
    {
        beginTest ("playlists - table exists");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "playlists"));

        static const char* const requiredCols[] = { "id", "name", "type", "created_at" };
        for (auto* col : requiredCols)
        {
            beginTest (juce::String ("playlists - column '") + col + "' exists");
            expect (columnExists (h, "playlists", col));
        }
    }

    // -----------------------------------------------------------------------
    // 8. playlist_tracks columns
    // -----------------------------------------------------------------------
    void testPlaylistTracksTable()
    {
        beginTest ("playlist_tracks - table exists");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        expect (tableExists (h, "playlist_tracks"));

        static const char* const requiredCols[] = { "id", "playlist_id", "track_id", "position", "played_at" };
        for (auto* col : requiredCols)
        {
            beginTest (juce::String ("playlist_tracks - column '") + col + "' exists");
            expect (columnExists (h, "playlist_tracks", col));
        }
    }

    void testPlaylistTracksAllowDuplicates()
    {
        beginTest ("playlist_tracks - duplicate track rows are allowed in a normal playlist");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/duplicate.mp3', 'hash_duplicate1', 'Duplicate', 'Artist', 'Album', 1000);");
        execSql (h,
            "INSERT INTO playlists (name, type, created_at) VALUES ('Duplicates', 'normal', 1000);");

        const int trackId = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/music/duplicate.mp3';");
        const int playlistId = queryInt (h, "SELECT id FROM playlists WHERE name='Duplicates';");
        expect (trackId > 0);
        expect (playlistId > 0);

        const juce::String insertSql =
            "INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES ("
            + juce::String (playlistId) + ", " + juce::String (trackId) + ", 1), ("
            + juce::String (playlistId) + ", " + juce::String (trackId) + ", 2);";
        expectEquals (execSql (h, insertSql.toRawUTF8()), SQLITE_OK);
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlist_tracks;"), 2);
    }

    // -----------------------------------------------------------------------
    // 9. History playlist seeded
    // -----------------------------------------------------------------------
    void testHistoryPlaylistSeeded()
    {
        beginTest ("playlists - exactly one History/history row seeded");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();
        const int historyCount = queryInt (
            h, "SELECT COUNT(*) FROM playlists WHERE name='History' AND type='history';");
        expectEquals (historyCount, 1);

        beginTest ("playlists - total row count is 1 (only the History row)");
        const int total = queryInt (h, "SELECT COUNT(*) FROM playlists;");
        expectEquals (total, 1);
    }

    void testPlaylistNamesAreCaseInsensitiveUnique()
    {
        beginTest ("playlists - names are unique case-insensitively");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        expectEquals (execSql (h,
            "INSERT INTO playlists (name, type, created_at) VALUES ('Warmup', 'normal', 1000);"), SQLITE_OK);
        expect (execSql (h,
            "INSERT INTO playlists (name, type, created_at) VALUES ('warmup', 'normal', 1001);") != SQLITE_OK);
    }

    // -----------------------------------------------------------------------
    // 10. Triggers exist
    // -----------------------------------------------------------------------
    void testTriggersExist()
    {
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        beginTest ("trigger - after_insert_library_tracks exists");
        expect (triggerExists (h, "after_insert_library_tracks"));

        beginTest ("trigger - after_update_library_tracks exists");
        expect (triggerExists (h, "after_update_library_tracks"));

        beginTest ("trigger - after_delete_library_tracks exists");
        expect (triggerExists (h, "after_delete_library_tracks"));
    }

    // -----------------------------------------------------------------------
    // 11. Idempotency: second construction leaves schema_version == 2
    // -----------------------------------------------------------------------
    void testIdempotency()
    {
        beginTest ("Idempotency - second TrackDatabase construction does not crash");
        const juce::File dbFile = juce::File::createTempFile ("sonik_idempotency_test.db");

        // First open: runs migration 1
        {
            TrackDatabase db1 (dbFile);
            expect (db1.getDbHandle() != nullptr);
        } // db1 closed here

        // Second open: should be a no-op for all migrations
        {
            TrackDatabase db2 (dbFile);
            expect (db2.getDbHandle() != nullptr);

            beginTest ("Idempotency - schema_version remains 2 after re-open");
            const int version =
                queryInt (db2.getDbHandle(), "SELECT version FROM schema_version LIMIT 1;");
            expectEquals (version, 2);

            beginTest ("Idempotency - schema_version has exactly one row");
            const int rowCount =
                queryInt (db2.getDbHandle(), "SELECT COUNT(*) FROM schema_version;");
            expectEquals (rowCount, 1);
        } // db2 closed here

        dbFile.deleteFile();
    }

    // -----------------------------------------------------------------------
    // 12. FTS insert trigger: inserted track is immediately searchable
    // -----------------------------------------------------------------------
    void testFtsInsertTrigger()
    {
        beginTest ("FTS insert trigger - track is searchable via MATCH after INSERT");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        const int rc = execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/beyonce.mp3', 'hash_bey1', "
            "   'Crazy In Love', 'Beyonce', 'Dangerously In Love', 1000);");
        expectEquals (rc, SQLITE_OK);

        beginTest ("FTS insert trigger - prefix match 'bey*' returns 1 row");
        const int count =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'bey*';");
        expectEquals (count, 1);
    }

    // -----------------------------------------------------------------------
    // 13. FTS delete trigger: deleted track is no longer searchable
    // -----------------------------------------------------------------------
    void testFtsDeleteTrigger()
    {
        beginTest ("FTS delete trigger - row is searchable before DELETE");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/adele.mp3', 'hash_adele1', 'Hello', 'Adele', '25', 1001);");

        const int before =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'adele';");
        expectEquals (before, 1);

        execSql (h, "DELETE FROM library_tracks WHERE file_path='/music/adele.mp3';");

        beginTest ("FTS delete trigger - row is no longer searchable after DELETE");
        const int after =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'adele';");
        expectEquals (after, 0);
    }

    // -----------------------------------------------------------------------
    // 14. FTS update trigger: old entry gone, new entry searchable
    // -----------------------------------------------------------------------
    void testFtsUpdateTrigger()
    {
        beginTest ("FTS update trigger - old title is initially searchable");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/track.mp3', 'hash_upd1', "
            "   'OldTitle', 'OldArtist', 'OldAlbum', 1002);");

        const int oldBefore =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'OldTitle';");
        expectEquals (oldBefore, 1);

        execSql (h,
            "UPDATE library_tracks "
            "SET title='NewTitle', artist='NewArtist' "
            "WHERE file_path='/music/track.mp3';");

        beginTest ("FTS update trigger - old title is no longer searchable after UPDATE");
        const int oldAfter =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'OldTitle';");
        expectEquals (oldAfter, 0);

        beginTest ("FTS update trigger - new title is searchable after UPDATE");
        const int newAfter =
            queryInt (h, "SELECT COUNT(*) FROM library_fts WHERE library_fts MATCH 'NewTitle';");
        expectEquals (newAfter, 1);
    }

    // -----------------------------------------------------------------------
    // 15. Analysis projection uses file_path as the library row identity
    // -----------------------------------------------------------------------
    void testAnalysisProjectionUpdatesByFilePath()
    {
        beginTest ("Analysis projection - updates library row when deck hash differs");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        const int rc = execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/analyzed.mp3', 'scanner_sha256_hash', "
            "   'Analyzed', 'Artist', 'Album', 1003);");
        expectEquals (rc, SQLITE_OK);

        ctx.db->updateLibraryTrackBpm ("/music/analyzed.mp3", "deck_loader_hash", 128.5);
        ctx.db->updateLibraryTrackKey ("/music/analyzed.mp3", "deck_loader_hash", "8A", 7);

        const double bpm = queryDouble (
            h, "SELECT bpm FROM library_tracks WHERE file_path='/music/analyzed.mp3';");
        expectWithinAbsoluteError (bpm, 128.5, 0.001);

        const juce::String key = queryText (
            h, "SELECT key FROM library_tracks WHERE file_path='/music/analyzed.mp3';");
        expectEquals (key, juce::String ("8A"));

        const int keyIndex = queryInt (
            h, "SELECT key_index FROM library_tracks WHERE file_path='/music/analyzed.mp3';");
        expectEquals (keyIndex, 7);
    }

    // -----------------------------------------------------------------------
    // 16. Existing tables are untouched by migration
    // -----------------------------------------------------------------------
    void testExistingTablesUntouched()
    {
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        beginTest ("Existing tables - session_state still exists after migration");
        expect (tableExists (h, "session_state"));

        beginTest ("Existing tables - track_data still exists after migration");
        expect (tableExists (h, "track_data"));

        beginTest ("Existing tables - waveform_cache still exists after migration");
        expect (tableExists (h, "waveform_cache"));

        beginTest ("Existing tables - loops_data still exists after migration");
        expect (tableExists (h, "loops_data"));

        beginTest ("Existing tables - stems_data still exists after migration");
        expect (tableExists (h, "stems_data"));
    }

    // -----------------------------------------------------------------------
    // 16. Foreign-key CASCADE: deleting playlist removes its playlist_tracks rows
    // -----------------------------------------------------------------------
    void testForeignKeyCascade()
    {
        beginTest ("FK cascade - deleting a playlist removes its playlist_tracks rows");
        DbContext ctx;
        auto* h = ctx.db->getDbHandle();

        // Insert a library track
        execSql (h,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/music/cascade.mp3', 'hash_cascade1', "
            "   'Cascade Track', 'Artist', 'Album', 1003);");

        const int trackId =
            queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/music/cascade.mp3';");
        expect (trackId > 0);

        // Insert a new non-History playlist
        execSql (h,
            "INSERT INTO playlists (name, type, created_at) "
            "VALUES ('TestPlaylist', 'normal', 1003);");

        const int playlistId =
            queryInt (h, "SELECT id FROM playlists WHERE name='TestPlaylist';");
        expect (playlistId > 0);

        // Link track to playlist
        const juce::String linkSql =
            "INSERT INTO playlist_tracks (playlist_id, track_id, position) "
            "VALUES (" + juce::String (playlistId) + ", "
                       + juce::String (trackId)    + ", 0);";
        const int rc = execSql (h, linkSql.toRawUTF8());
        expectEquals (rc, SQLITE_OK);

        // Verify the link exists
        const int before = queryInt (h, "SELECT COUNT(*) FROM playlist_tracks;");
        expectEquals (before, 1);

        // Delete the playlist; ON DELETE CASCADE should remove the playlist_tracks row
        const juce::String delSql =
            "DELETE FROM playlists WHERE id=" + juce::String (playlistId) + ";";
        execSql (h, delSql.toRawUTF8());

        beginTest ("FK cascade - playlist_tracks row is gone after playlist deletion");
        const int after = queryInt (h, "SELECT COUNT(*) FROM playlist_tracks;");
        expectEquals (after, 0);
    }
};

static LibraryDatabaseSchemaTests libraryDatabaseSchemaTests;
