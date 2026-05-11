#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Library/LibraryQueryThread.h"
#include <sqlite3.h>

#include <atomic>
#include <vector>

// =============================================================================
// LibraryQueryThreadTests
// =============================================================================
class LibraryQueryThreadTests : public juce::UnitTest
{
public:
    LibraryQueryThreadTests()
        : juce::UnitTest ("Library Query Thread", "Sonik") {}

    void runTest() override
    {
        // Async integration tests (require DB + thread)
        testEmptyQueryReturnsAllTracks();
        testFtsSearch();
        testBpmExactFilter();
        testBpmRangeFilter();
        testRatingFilter();
        testTitleFilter();
        testArtistFilter();
        testCombinedArtistAndBpm();
        testSingleQuoteInjection();
        testSortAscendingByBpm();
        testPlaylistQueryPreservesDuplicatesAndPositions();
        testPreparationQueryPreservesOrder();
        testCreatePlaylistRejectsDuplicateName();
        testRenamePlaylistRejectsDuplicateAndSystemPlaylist();
        testPlaylistAddRemoveMoveAndDelete();
        testHistoryAppendCapsAtFiveHundred();
        testRequestPlaylistsOrdersSystemAndNormalNodes();

        // Static parsing tests (no DB required)
        testParseSearchStringBpmExact();
        testParseSearchStringBpmRange();
        testParseSearchStringKey();
        testParseSearchStringRating();
        testCamelotKeyToIndex();

        // Deck-aware filter integration tests
        testDeckAwareFilterBpm();
        testDeckAwareFilterKey();
    }

private:
    // =========================================================================
    // RAII test context
    // =========================================================================
    struct TestContext
    {
        juce::File                              tmpDir;
        juce::File                              dbFile;
        std::unique_ptr<TrackDatabase>          db;
        std::unique_ptr<LibraryQueryThread>     thread;
    };

    /// Create an isolated temp DB + thread.  Drains the initial blank query
    /// the thread fires on startup (no callback set -> silently discarded).
    TestContext makeContext (const juce::String& name)
    {
        TestContext ctx;

        ctx.tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("SonikLQT_" + name + "_"
                                        + juce::String (juce::Time::currentTimeMillis()));
        ctx.tmpDir.createDirectory();
        ctx.dbFile = ctx.tmpDir.getChildFile ("test.db");
        ctx.db     = std::make_unique<TrackDatabase> (ctx.dbFile);

        // Thread constructor calls startThread internally.
        ctx.thread = std::make_unique<LibraryQueryThread> (*ctx.db);

        // Pump the message loop long enough for the thread's self-dispatched
        // initial blank query to complete and post its callAsync.  No callback
        // is registered yet, so the result is dropped harmlessly.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

        return ctx;
    }

    void destroyContext (TestContext& ctx)
    {
        // Destructor signals the thread and calls stopThread(3000) internally.
        ctx.thread.reset();
        ctx.db.reset();
        ctx.tmpDir.deleteRecursively();
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    /// Insert a row directly into library_tracks via the Message-Thread handle.
    /// Triggers automatically populate library_fts.
    void insertTrack (sqlite3*    h,
                      const char* filePath,
                      const char* title,
                      const char* artist,
                      double      bpm,
                      int         keyIndex = -1,
                      int         rating   = 0)
    {
        const char* sql =
            "INSERT INTO library_tracks "
            "(file_path, content_hash, title, artist, album, bpm, key, key_index,"
            " duration_seconds, file_size_bytes, date_added, last_seen, is_missing,"
            " play_count, rating)"
            " VALUES (?, ?, ?, ?, '', ?, '', ?,"
            " 180.0, 0, strftime('%s','now'), strftime('%s','now'), 0, 0, ?)";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return;

        sqlite3_bind_text   (stmt, 1, filePath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text   (stmt, 2, filePath, -1, SQLITE_TRANSIENT); // path doubles as hash
        sqlite3_bind_text   (stmt, 3, title,    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text   (stmt, 4, artist,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_double (stmt, 5, bpm);
        sqlite3_bind_int    (stmt, 6, keyIndex);
        sqlite3_bind_int    (stmt, 7, rating);

        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }

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

    int execSql (sqlite3* h, const char* sql)
    {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec (h, sql, nullptr, nullptr, &errMsg);
        if (errMsg != nullptr) sqlite3_free (errMsg);
        return rc;
    }

    /// Pump the message loop until `received` becomes true or `timeoutMs` elapses.
    bool waitForResult (std::atomic<bool>& received, int timeoutMs = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMs);
        while (!received.load (std::memory_order_acquire))
        {
            if (juce::Time::getMillisecondCounter() >= deadline)
                return false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
        }
        return true;
    }

    // =========================================================================
    // Test 1: Empty query returns all tracks
    // =========================================================================
    void testEmptyQueryReturnsAllTracks()
    {
        beginTest ("Empty query returns all tracks (insert 3, expect 3)");

        auto ctx = makeContext ("EmptyQuery");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/eq/a.mp3", "Track A", "Artist A", 128.0);
        insertTrack (h, "/eq/b.mp3", "Track B", "Artist B", 135.0);
        insertTrack (h, "/eq/c.mp3", "Track C", "Artist C", 140.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString (""));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 3, "Expected 3 tracks");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 2: Bare-word FTS search
    // =========================================================================
    void testFtsSearch()
    {
        beginTest ("Bare word FTS: 'strobe' finds 'Deadmau5 Strobe'");

        auto ctx = makeContext ("Fts");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/fts/strobe.mp3", "Deadmau5 Strobe", "Deadmau5", 128.0);
        insertTrack (h, "/fts/acid.mp3",   "Acid House",      "DJ Test",  130.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("strobe"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected 1 matching track");
        if (!results.empty())
            expect (results[0].title.containsIgnoreCase ("Strobe"), "Title should contain 'Strobe'");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 3: bpm:128 returns only track with bpm=128 (+/-0.5)
    // =========================================================================
    void testBpmExactFilter()
    {
        beginTest ("bpm:128 returns only track with bpm=128, excludes bpm=140");

        auto ctx = makeContext ("BpmExact");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/be/t128.mp3", "Track 128", "Artist A", 128.0);
        insertTrack (h, "/be/t140.mp3", "Track 140", "Artist B", 140.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("bpm:128"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected 1 track");
        if (!results.empty())
            expectWithinAbsoluteError (results[0].bpm, 128.0, 0.5, "BPM should be approximately 128");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 4: bpm:125-135 range filter
    // =========================================================================
    void testBpmRangeFilter()
    {
        beginTest ("bpm:125-135 returns in-range tracks, excludes outside");

        auto ctx = makeContext ("BpmRange");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/br/t120.mp3", "Track 120", "A", 120.0);
        insertTrack (h, "/br/t128.mp3", "Track 128", "A", 128.0);
        insertTrack (h, "/br/t130.mp3", "Track 130", "A", 130.0);
        insertTrack (h, "/br/t140.mp3", "Track 140", "A", 140.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("bpm:125-135"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 2, "Expected 2 tracks in 125-135 range");
        for (const auto& r : results)
            expect (r.bpm >= 125.0 && r.bpm <= 135.0, "BPM must be within 125-135");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 5: rating:3 returns tracks with rating >= 3
    // =========================================================================
    void testRatingFilter()
    {
        beginTest ("rating:3 returns only tracks with rating >= 3");

        auto ctx = makeContext ("Rating");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/rat/r1.mp3", "Track 1", "A", 128.0, -1, 1);
        insertTrack (h, "/rat/r3.mp3", "Track 3", "A", 128.0, -1, 3);
        insertTrack (h, "/rat/r5.mp3", "Track 5", "A", 128.0, -1, 5);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("rating:3"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 2, "Expected 2 tracks with rating >= 3");
        for (const auto& r : results)
            expect (r.rating >= 3, "Rating must be >= 3");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 6: title:house
    // =========================================================================
    void testTitleFilter()
    {
        beginTest ("title:house returns track with title containing 'house'");

        auto ctx = makeContext ("TitleFilter");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/tf/house.mp3",  "Deep House Vibes",   "DJ A", 128.0);
        insertTrack (h, "/tf/techno.mp3", "Industrial Techno",  "DJ B", 140.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("title:house"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected 1 matching track");
        if (!results.empty())
            expect (results[0].title.containsIgnoreCase ("house"), "Title should contain 'house'");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 7: artist:techno
    // =========================================================================
    void testArtistFilter()
    {
        beginTest ("artist:techno returns track with artist containing 'techno'");

        auto ctx = makeContext ("ArtistFilter");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/af/a1.mp3", "Track 1", "Techno Master", 140.0);
        insertTrack (h, "/af/a2.mp3", "Track 2", "House DJ",      128.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("artist:techno"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected 1 matching track");
        if (!results.empty())
            expect (results[0].artist.containsIgnoreCase ("Techno"), "Artist should contain 'Techno'");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 8: Combined artist:dj bpm:128-132
    // =========================================================================
    void testCombinedArtistAndBpm()
    {
        beginTest ("Combined artist:dj bpm:128-132 returns only matching track");

        auto ctx = makeContext ("Combined");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/comb/dj128.mp3", "Track A", "DJ Max",   130.0); // matches both
        insertTrack (h, "/comb/dj140.mp3", "Track B", "DJ Star",  140.0); // artist matches, BPM fails
        insertTrack (h, "/comb/other.mp3", "Track C", "Producer", 130.0); // BPM matches, artist fails

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("artist:dj bpm:128-132"));

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected exactly 1 track matching both filters");
        if (!results.empty())
        {
            expect (results[0].artist.containsIgnoreCase ("DJ"), "Artist should contain 'DJ'");
            expect (results[0].bpm >= 128.0 && results[0].bpm <= 132.0, "BPM should be in 128-132");
        }

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 9: Single-quote injection must not crash
    // =========================================================================
    void testSingleQuoteInjection()
    {
        beginTest ("Single-quote injection: d'n'b dispatched without crash");

        auto ctx = makeContext ("Injection");

        std::atomic<bool> received { false };
        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow>) {
            received.store (true, std::memory_order_release);
        });

        // This must NOT crash - the parameterised query handles it safely.
        ctx.thread->dispatchQuery (LibraryQueryThread::parseSearchString ("d'n'b"));

        // We just need either a result or a graceful empty-result response.
        waitForResult (received, 3000);
        expect (true, "No crash on single-quote input");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 10: Sort ascending by BPM
    // =========================================================================
    void testSortAscendingByBpm()
    {
        beginTest ("Sort ASC by bpm: inserted 140,120,130 -> results ordered 120,130,140");

        auto ctx = makeContext ("SortBpm");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/sort/t140.mp3", "Track 140", "A", 140.0);
        insertTrack (h, "/sort/t120.mp3", "Track 120", "A", 120.0);
        insertTrack (h, "/sort/t130.mp3", "Track 130", "A", 130.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        auto params          = LibraryQueryThread::parseSearchString ("");
        params.sortColumn    = "bpm";
        params.sortAscending = true;
        ctx.thread->dispatchQuery (params);

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 3, "Expected 3 tracks");

        if (results.size() == 3)
        {
            expectWithinAbsoluteError (results[0].bpm, 120.0, 0.01, "1st should be 120 BPM");
            expectWithinAbsoluteError (results[1].bpm, 130.0, 0.01, "2nd should be 130 BPM");
            expectWithinAbsoluteError (results[2].bpm, 140.0, 0.01, "3rd should be 140 BPM");
        }

        destroyContext (ctx);
    }

    void testPlaylistQueryPreservesDuplicatesAndPositions()
    {
        beginTest ("Playlist query returns duplicate entries ordered by playlist position");

        auto ctx = makeContext ("PlaylistQuery");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/playlist/a.mp3", "Track A", "Artist", 128.0);
        insertTrack (h, "/playlist/b.mp3", "Track B", "Artist", 129.0);
        execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Set', 'normal', 1000);");

        const int playlistId = queryInt (h, "SELECT id FROM playlists WHERE name='Set';");
        const int trackA = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/playlist/a.mp3';");
        const int trackB = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/playlist/b.mp3';");
        const juce::String insertSql =
            "INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES ("
            + juce::String (playlistId) + ", " + juce::String (trackA) + ", 1), ("
            + juce::String (playlistId) + ", " + juce::String (trackA) + ", 2), ("
            + juce::String (playlistId) + ", " + juce::String (trackB) + ", 3);";
        expectEquals (execSql (h, insertSql.toRawUTF8()), SQLITE_OK);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;
        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        QueryParams params;
        params.playlistId = playlistId;
        params.playlistType = "normal";
        ctx.thread->dispatchQuery (params);

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 3, "Expected duplicate playlist entries");
        if (results.size() == 3)
        {
            expectEquals (static_cast<int> (results[0].id), trackA);
            expectEquals (static_cast<int> (results[1].id), trackA);
            expectEquals (static_cast<int> (results[2].id), trackB);
            expect (results[0].playlistEntryId != results[1].playlistEntryId);
            expectEquals (results[0].playlistPosition, 1);
            expectEquals (results[1].playlistPosition, 2);
            expectEquals (results[2].playlistPosition, 3);
        }

        destroyContext (ctx);
    }

    void testPreparationQueryPreservesOrder()
    {
        beginTest ("Preparation query returns in-memory IDs in requested order");

        auto ctx = makeContext ("PreparationQuery");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/prep/a.mp3", "Track A", "Artist", 128.0);
        insertTrack (h, "/prep/b.mp3", "Track B", "Artist", 129.0);
        const int trackA = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/prep/a.mp3';");
        const int trackB = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/prep/b.mp3';");

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;
        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        QueryParams params;
        params.playlistType = "preparation";
        params.preparationTrackIds = { trackB, trackA, trackB };
        ctx.thread->dispatchQuery (params);

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 3, "Expected duplicate preparation rows");
        if (results.size() == 3)
        {
            expectEquals (static_cast<int> (results[0].id), trackB);
            expectEquals (static_cast<int> (results[1].id), trackA);
            expectEquals (static_cast<int> (results[2].id), trackB);
            expectEquals (results[0].playlistPosition, 1);
            expectEquals (results[1].playlistPosition, 2);
            expectEquals (results[2].playlistPosition, 3);
        }

        destroyContext (ctx);
    }

    void testCreatePlaylistRejectsDuplicateName()
    {
        beginTest ("Create playlist accepts unique names and rejects case-insensitive duplicates");

        auto ctx = makeContext ("CreatePlaylist");
        auto* h  = ctx.db->getDbHandle();

        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Warmup', 'normal', 1000);"), SQLITE_OK);

        std::atomic<bool> received { false };
        bool ok = false;
        juce::String message;
        int64_t playlistId = 0;

        ctx.thread->createPlaylist ("Peak Time",
            [&] (bool resultOk, juce::String resultMessage, int64_t resultPlaylistId)
            {
                ok = resultOk;
                message = resultMessage;
                playlistId = resultPlaylistId;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Create callback timed out");
        expect (ok, "Unique playlist name should be accepted");
        expect (playlistId > 0, "New playlist id should be returned");
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlists WHERE name='Peak Time' AND type='normal';"), 1);

        received.store (false, std::memory_order_release);
        ok = true;
        message = {};
        playlistId = 0;

        ctx.thread->createPlaylist ("warmup",
            [&] (bool resultOk, juce::String resultMessage, int64_t resultPlaylistId)
            {
                ok = resultOk;
                message = resultMessage;
                playlistId = resultPlaylistId;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Duplicate create callback timed out");
        expect (! ok, "Duplicate playlist name should be rejected");
        expectEquals (message, juce::String ("A playlist with this name already exists"));
        expectEquals (playlistId, static_cast<int64_t> (0));
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlists WHERE name='Warmup' COLLATE NOCASE;"), 1);

        destroyContext (ctx);
    }

    void testRenamePlaylistRejectsDuplicateAndSystemPlaylist()
    {
        beginTest ("Rename playlist rejects duplicate names and system playlists");

        auto ctx = makeContext ("RenamePlaylist");
        auto* h  = ctx.db->getDbHandle();

        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Set A', 'normal', 1000);"), SQLITE_OK);
        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Warmup', 'normal', 1001);"), SQLITE_OK);

        const int64_t setId = queryInt (h, "SELECT id FROM playlists WHERE name='Set A';");
        const int64_t historyId = queryInt (h, "SELECT id FROM playlists WHERE type='history';");

        std::atomic<bool> received { false };
        bool ok = true;
        juce::String message;

        ctx.thread->renamePlaylist (setId, "warmup",
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Duplicate rename callback timed out");
        expect (! ok, "Duplicate rename should be rejected");
        expectEquals (message, juce::String ("A playlist with this name already exists"));
        expectEquals (queryText (h, "SELECT name FROM playlists WHERE id=(SELECT id FROM playlists WHERE name='Set A');"), juce::String ("Set A"));

        received.store (false, std::memory_order_release);
        ok = false;
        message = {};

        ctx.thread->renamePlaylist (setId, "After Hours",
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Unique rename callback timed out");
        expect (ok, "Unique rename should be committed");
        expectEquals (queryText (h, "SELECT name FROM playlists WHERE id=(SELECT id FROM playlists WHERE name='After Hours');"), juce::String ("After Hours"));

        received.store (false, std::memory_order_release);
        ok = true;
        message = {};

        ctx.thread->renamePlaylist (historyId, "Past Sets",
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "History rename callback timed out");
        expect (! ok, "History playlist should be read-only");
        expectEquals (queryText (h, "SELECT name FROM playlists WHERE type='history';"), juce::String ("History"));

        destroyContext (ctx);
    }

    void testPlaylistAddRemoveMoveAndDelete()
    {
        beginTest ("Playlist add/remove/move/delete preserves duplicates and library tracks");

        auto ctx = makeContext ("PlaylistMutations");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/mut/a.mp3", "Track A", "Artist", 128.0);
        insertTrack (h, "/mut/b.mp3", "Track B", "Artist", 129.0);
        insertTrack (h, "/mut/c.mp3", "Track C", "Artist", 130.0);
        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Set', 'normal', 1000);"), SQLITE_OK);

        const int64_t playlistId = queryInt (h, "SELECT id FROM playlists WHERE name='Set';");
        const int64_t trackA = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/mut/a.mp3';");
        const int64_t trackB = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/mut/b.mp3';");
        const int64_t trackC = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/mut/c.mp3';");

        std::atomic<bool> received { false };
        bool ok = false;
        juce::String message;

        ctx.thread->addTracksToPlaylist (playlistId, { trackA, trackB, trackC, trackA },
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Add tracks callback timed out");
        expect (ok, message);
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlist_tracks;"), 4);

        const juce::String orderAfterAddSql =
            "SELECT GROUP_CONCAT(track_id || ':' || position, ',') FROM ("
            "SELECT track_id, position FROM playlist_tracks WHERE playlist_id=" + juce::String (playlistId)
            + " ORDER BY position ASC, id ASC);";
        const juce::String expectedAddOrder = juce::String (trackA) + ":1," + juce::String (trackB) + ":2,"
                                           + juce::String (trackC) + ":3," + juce::String (trackA) + ":4";
        expectEquals (queryText (h, orderAfterAddSql.toRawUTF8()), expectedAddOrder);

        const juce::String entryAtTwoSql = "SELECT id FROM playlist_tracks WHERE playlist_id=" + juce::String (playlistId) + " AND position=2;";
        const int64_t entryAtTwo = queryInt (h, entryAtTwoSql.toRawUTF8());

        received.store (false, std::memory_order_release);
        ok = false;
        message = {};

        ctx.thread->removePlaylistEntries (playlistId, { entryAtTwo },
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Remove tracks callback timed out");
        expect (ok, message);
        const juce::String orderAfterRemoveSql =
            "SELECT GROUP_CONCAT(track_id || ':' || position, ',') FROM ("
            "SELECT track_id, position FROM playlist_tracks WHERE playlist_id=" + juce::String (playlistId)
            + " ORDER BY position ASC, id ASC);";
        const juce::String expectedRemoveOrder = juce::String (trackA) + ":1," + juce::String (trackC) + ":2,"
                                              + juce::String (trackA) + ":3";
        expectEquals (queryText (h, orderAfterRemoveSql.toRawUTF8()), expectedRemoveOrder);

        const juce::String entryTrackC = "SELECT id FROM playlist_tracks WHERE playlist_id=" + juce::String (playlistId)
                                       + " AND track_id=" + juce::String (trackC) + ";";
        const int64_t trackCEntryId = queryInt (h, entryTrackC.toRawUTF8());

        received.store (false, std::memory_order_release);
        ok = false;
        message = {};

        ctx.thread->movePlaylistEntry (playlistId, trackCEntryId, 0,
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Move entry callback timed out");
        expect (ok, message);
        const juce::String expectedMoveOrder = juce::String (trackC) + ":1," + juce::String (trackA) + ":2,"
                                            + juce::String (trackA) + ":3";
        expectEquals (queryText (h, orderAfterRemoveSql.toRawUTF8()), expectedMoveOrder);

        received.store (false, std::memory_order_release);
        ok = false;
        message = {};

        ctx.thread->deletePlaylist (playlistId,
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "Delete playlist callback timed out");
        expect (ok, message);
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlists WHERE name='Set';"), 0);
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM playlist_tracks;"), 0);
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM library_tracks WHERE file_path LIKE '/mut/%';"), 3);

        destroyContext (ctx);
    }

    void testHistoryAppendCapsAtFiveHundred()
    {
        beginTest ("History append records play time and caps entries at 500");

        auto ctx = makeContext ("HistoryCap");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/hist/old.mp3", "Old Track", "Artist", 120.0);
        insertTrack (h, "/hist/new.mp3", "New Track", "Artist", 121.0);

        const int64_t historyId = queryInt (h, "SELECT id FROM playlists WHERE type='history';");
        const int64_t oldTrackId = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/hist/old.mp3';");

        sqlite3_stmt* stmt = nullptr;
        expectEquals (sqlite3_prepare_v2 (h,
            "INSERT INTO playlist_tracks (playlist_id, track_id, position, played_at) VALUES (?, ?, ?, ?);",
            -1, &stmt, nullptr), SQLITE_OK);

        if (stmt != nullptr)
        {
            for (int i = 0; i < 500; ++i)
            {
                const auto playedAt = "2020-01-01T00:" + juce::String (i).paddedLeft ('0', 3) + "Z";
                sqlite3_reset (stmt);
                sqlite3_clear_bindings (stmt);
                sqlite3_bind_int64 (stmt, 1, historyId);
                sqlite3_bind_int64 (stmt, 2, oldTrackId);
                sqlite3_bind_int   (stmt, 3, i + 1);
                sqlite3_bind_text  (stmt, 4, playedAt.toRawUTF8(), -1, SQLITE_TRANSIENT);
                expectEquals (sqlite3_step (stmt), SQLITE_DONE);
            }
            sqlite3_finalize (stmt);
        }

        std::atomic<bool> received { false };
        bool ok = false;
        juce::String message;

        ctx.thread->appendHistoryEntryForFilePath ("/hist/new.mp3",
            [&] (bool resultOk, juce::String resultMessage, int64_t)
            {
                ok = resultOk;
                message = resultMessage;
                received.store (true, std::memory_order_release);
            });

        expect (waitForResult (received), "History append callback timed out");
        expect (ok, message);

        const juce::String countSql = "SELECT COUNT(*) FROM playlist_tracks WHERE playlist_id=" + juce::String (historyId) + ";";
        expectEquals (queryInt (h, countSql.toRawUTF8()), 500);

        const juce::String topPathSql =
            "SELECT lt.file_path FROM playlist_tracks pt "
            "JOIN library_tracks lt ON lt.id=pt.track_id "
            "WHERE pt.playlist_id=" + juce::String (historyId)
            + " ORDER BY pt.played_at DESC, pt.id DESC LIMIT 1;";
        expectEquals (queryText (h, topPathSql.toRawUTF8()), juce::String ("/hist/new.mp3"));

        const juce::String oldestSql = "SELECT COUNT(*) FROM playlist_tracks WHERE playlist_id=" + juce::String (historyId)
                                     + " AND played_at='2020-01-01T00:000Z';";
        expectEquals (queryInt (h, oldestSql.toRawUTF8()), 0);

        destroyContext (ctx);
    }

    void testRequestPlaylistsOrdersSystemAndNormalNodes()
    {
        beginTest ("Playlist list returns History first, then normal playlists by creation time with counts");

        auto ctx = makeContext ("PlaylistList");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/list/a.mp3", "Track A", "Artist", 128.0);
        const int64_t trackId = queryInt (h, "SELECT id FROM library_tracks WHERE file_path='/list/a.mp3';");
        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Later', 'normal', 2000);"), SQLITE_OK);
        expectEquals (execSql (h, "INSERT INTO playlists (name, type, created_at) VALUES ('Earlier', 'normal', 1000);"), SQLITE_OK);

        const int64_t laterId = queryInt (h, "SELECT id FROM playlists WHERE name='Later';");
        const juce::String addRows =
            "INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES ("
            + juce::String (laterId) + ", " + juce::String (trackId) + ", 1), ("
            + juce::String (laterId) + ", " + juce::String (trackId) + ", 2);";
        expectEquals (execSql (h, addRows.toRawUTF8()), SQLITE_OK);

        std::atomic<bool> received { false };
        std::vector<PlaylistInfo> playlists;
        ctx.thread->requestPlaylists ([&] (std::vector<PlaylistInfo> result)
        {
            playlists = std::move (result);
            received.store (true, std::memory_order_release);
        });

        expect (waitForResult (received), "Playlist list callback timed out");
        expectEquals (static_cast<int> (playlists.size()), 3);
        if (playlists.size() == 3)
        {
            expect (playlists[0].isHistory(), "History should be the first database playlist");
            expectEquals (playlists[1].name, juce::String ("Earlier"));
            expectEquals (playlists[2].name, juce::String ("Later"));
            expectEquals (playlists[2].trackCount, 2);
        }

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 11: parseSearchString("bpm:128") -> exact +/-0.5 range
    // =========================================================================
    void testParseSearchStringBpmExact()
    {
        beginTest ("parseSearchString(\"bpm:128\") -> hasBpmRange=true, min=127.5, max=128.5");

        const auto p = LibraryQueryThread::parseSearchString ("bpm:128");

        expect     (p.hasBpmRange, "hasBpmRange must be true");
        expectWithinAbsoluteError (p.bpmMin, 127.5, 0.01, "bpmMin should be 127.5");
        expectWithinAbsoluteError (p.bpmMax, 128.5, 0.01, "bpmMax should be 128.5");
    }

    // =========================================================================
    // Test 12: parseSearchString("bpm:125-135") -> explicit range
    // =========================================================================
    void testParseSearchStringBpmRange()
    {
        beginTest ("parseSearchString(\"bpm:125-135\") -> bpmMin=125.0, bpmMax=135.0");

        const auto p = LibraryQueryThread::parseSearchString ("bpm:125-135");

        expect     (p.hasBpmRange, "hasBpmRange must be true");
        expectWithinAbsoluteError (p.bpmMin, 125.0, 0.01, "bpmMin should be 125.0");
        expectWithinAbsoluteError (p.bpmMax, 135.0, 0.01, "bpmMax should be 135.0");
    }

    // =========================================================================
    // Test 13: parseSearchString("key:8A") -> hasKeyFilter=true, keyIndex=7
    // =========================================================================
    void testParseSearchStringKey()
    {
        beginTest ("parseSearchString(\"key:8A\") -> hasKeyFilter=true, keyIndex=7");

        const auto p = LibraryQueryThread::parseSearchString ("key:8A");

        expect     (p.hasKeyFilter, "hasKeyFilter must be true");
        expectEquals (p.keyIndex, 7, "keyIndex for 8A should be 7");
    }

    // =========================================================================
    // Test 14: parseSearchString("rating:4") -> hasRatingFilter=true, ratingMin=4
    // =========================================================================
    void testParseSearchStringRating()
    {
        beginTest ("parseSearchString(\"rating:4\") -> hasRatingFilter=true, ratingMin=4");

        const auto p = LibraryQueryThread::parseSearchString ("rating:4");

        expect     (p.hasRatingFilter, "hasRatingFilter must be true");
        expectEquals (p.ratingMin, 4, "ratingMin should be 4");
    }

    // =========================================================================
    // Test 15: camelotKeyToIndex - full boundary coverage
    // =========================================================================
    void testCamelotKeyToIndex()
    {
        beginTest ("camelotKeyToIndex: 1A->0, 12A->11, 1B->12, 12B->23, 8A->7, invalid->-1");

        expectEquals (LibraryQueryThread::camelotKeyToIndex ("1A"),     0,  "1A should map to 0");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("12A"),   11,  "12A should map to 11");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("1B"),    12,  "1B should map to 12");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("12B"),   23,  "12B should map to 23");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("8A"),     7,  "8A should map to 7");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("invalid"), -1, "invalid should return -1");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("0A"),    -1,  "0A (out of range) should return -1");
        expectEquals (LibraryQueryThread::camelotKeyToIndex ("13A"),   -1,  "13A (out of range) should return -1");
    }

    // =========================================================================
    // Test 16: DeckAwareFilter BPM window
    // =========================================================================
    void testDeckAwareFilterBpm()
    {
        beginTest ("DeckAwareFilter bpmMatchActive=true, window(125,135) -> only bpm=128 from {128,140,115}");

        auto ctx = makeContext ("DeckBpm");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/db/t128.mp3", "Track 128", "A", 128.0);
        insertTrack (h, "/db/t140.mp3", "Track 140", "A", 140.0);
        insertTrack (h, "/db/t115.mp3", "Track 115", "A", 115.0);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        QueryParams params;
        params.deckFilter.bpmMatchActive = true;
        params.deckFilter.bpmWindows.add ({ 125.0, 135.0 });
        ctx.thread->dispatchQuery (params);

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected 1 track within BPM window");
        if (!results.empty())
            expectWithinAbsoluteError (results[0].bpm, 128.0, 0.5, "Track BPM should be approximately 128");

        destroyContext (ctx);
    }

    // =========================================================================
    // Test 17: DeckAwareFilter key index
    // =========================================================================
    void testDeckAwareFilterKey()
    {
        beginTest ("DeckAwareFilter keyMatchActive=true, compatibleKeyIndices={7} -> only key_index=7");

        auto ctx = makeContext ("DeckKey");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/dk/k7.mp3",  "Track Key7",  "A", 128.0, 7);
        insertTrack (h, "/dk/k3.mp3",  "Track Key3",  "A", 128.0, 3);
        insertTrack (h, "/dk/k15.mp3", "Track Key15", "A", 128.0, 15);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> results;

        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> rows) {
            results = std::move (rows);
            received.store (true, std::memory_order_release);
        });

        QueryParams params;
        params.deckFilter.keyMatchActive = true;
        params.deckFilter.compatibleKeyIndices.add (7);
        ctx.thread->dispatchQuery (params);

        expect (waitForResult (received), "Callback timed out");
        expectEquals (static_cast<int> (results.size()), 1, "Expected exactly 1 track with key_index=7");
        if (!results.empty())
            expectEquals (results[0].keyIndex, 7, "key_index should be 7");

        destroyContext (ctx);
    }
};

static LibraryQueryThreadTests sLibraryQueryThreadTests;
