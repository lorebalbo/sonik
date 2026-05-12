#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Library/LibraryQueryThread.h"
#include "Features/Library/WatchFolderScanner.h"
#include <sqlite3.h>

#include <atomic>
#include <vector>

// =============================================================================
// MissingFileTests (PRD-0039)
// =============================================================================
class MissingFileTests : public juce::UnitTest
{
public:
    MissingFileTests() : juce::UnitTest ("Missing File Detection and Relocation", "Sonik") {}

    void runTest() override
    {
        testShowMissingOnlyParamFiltersResults();
        testReconciliationMarksAndRestoresMissing();
        testDedupCheckRejectsExistingPath();
        testRemoveTrackDeletesPlaylistEntriesAtomically();
        testCountMissingTracksReflectsState();
    }

private:
    // ------------------------------------------------------------------------
    // RAII context
    // ------------------------------------------------------------------------
    struct TestContext
    {
        juce::File                          tmpDir;
        juce::File                          dbFile;
        std::unique_ptr<TrackDatabase>      db;
        std::unique_ptr<LibraryQueryThread> thread;
    };

    TestContext makeContext (const juce::String& name)
    {
        TestContext ctx;
        ctx.tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("SonikMFT_" + name + "_"
                                        + juce::String (juce::Time::currentTimeMillis()));
        ctx.tmpDir.createDirectory();
        ctx.dbFile = ctx.tmpDir.getChildFile ("test.db");
        ctx.db     = std::make_unique<TrackDatabase> (ctx.dbFile);
        ctx.thread = std::make_unique<LibraryQueryThread> (*ctx.db);

        // Drain initial blank-query callback.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        return ctx;
    }

    void destroyContext (TestContext& ctx)
    {
        ctx.thread.reset();
        ctx.db.reset();
        ctx.tmpDir.deleteRecursively();
    }

    void insertTrack (sqlite3* h,
                      const char* filePath,
                      const char* title,
                      int isMissing)
    {
        const char* sql =
            "INSERT INTO library_tracks "
            "(file_path, content_hash, title, artist, album, bpm, key, key_index,"
            " duration_seconds, file_size_bytes, date_added, last_seen, is_missing,"
            " play_count, rating) "
            "VALUES (?, ?, ?, '', '', 0, '', -1, 0, 0, "
            "        strftime('%s','now'), strftime('%s','now'), ?, 0, 0)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return;
        sqlite3_bind_text (stmt, 1, filePath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 2, filePath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, title,    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt, 4, isMissing);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }

    int64_t lastInsertRowId (sqlite3* h)
    {
        return sqlite3_last_insert_rowid (h);
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

    bool waitForFlag (std::atomic<bool>& flag, int timeoutMs = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMs);
        while (! flag.load (std::memory_order_acquire))
        {
            if (juce::Time::getMillisecondCounter() >= deadline)
                return false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Test 1: QueryParams::showMissingOnly produces WHERE is_missing = 1
    // ------------------------------------------------------------------------
    void testShowMissingOnlyParamFiltersResults()
    {
        beginTest ("showMissingOnly query returns only is_missing = 1 rows");

        auto ctx = makeContext ("ShowMissingOnly");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/mft/present_a.mp3", "Present A", 0);
        insertTrack (h, "/mft/present_b.mp3", "Present B", 0);
        insertTrack (h, "/mft/missing_a.mp3", "Missing A", 1);
        insertTrack (h, "/mft/missing_b.mp3", "Missing B", 1);
        insertTrack (h, "/mft/missing_c.mp3", "Missing C", 1);

        std::atomic<bool> received { false };
        std::vector<LibraryTrackRow> rows;
        ctx.thread->setResultCallback ([&] (std::vector<LibraryTrackRow> r) {
            rows = std::move (r);
            received.store (true, std::memory_order_release);
        });

        QueryParams params;
        params.showMissingOnly = true;
        ctx.thread->dispatchQuery (params);

        expect (waitForFlag (received), "Callback timed out");
        expectEquals (static_cast<int> (rows.size()), 3, "Expected 3 missing rows");
        for (const auto& r : rows)
            expectEquals (r.isMissing, 1, "Every returned row must have is_missing = 1");

        destroyContext (ctx);
    }

    // ------------------------------------------------------------------------
    // Test 2: Reconciliation flips is_missing in both directions
    // ------------------------------------------------------------------------
    void testReconciliationMarksAndRestoresMissing()
    {
        beginTest ("Reconciliation pass marks gone files missing and restores returned ones");

        auto ctx = makeContext ("Reconcile");
        auto* h  = ctx.db->getDbHandle();

        // Track A: file present on disk, DB says present  → stays 0
        // Track B: file absent on disk,  DB says present  → flips to 1
        // Track C: file present on disk, DB says missing  → flips to 0
        auto realA = ctx.tmpDir.getChildFile ("track_a.wav");
        realA.create();
        realA.appendText ("aaa");
        auto fakeB = ctx.tmpDir.getChildFile ("ghost_b.wav");   // never created
        auto realC = ctx.tmpDir.getChildFile ("track_c.wav");
        realC.create();
        realC.appendText ("ccc");

        insertTrack (h, realA.getFullPathName().toRawUTF8(), "A", 0);
        insertTrack (h, fakeB.getFullPathName().toRawUTF8(), "B", 0);
        insertTrack (h, realC.getFullPathName().toRawUTF8(), "C", 1);

        // Watched folder needed so the scanner reaches the reconciliation pass.
        auto emptyDir = ctx.tmpDir.getChildFile ("watched_empty");
        emptyDir.createDirectory();
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2 (h,
                    "INSERT OR IGNORE INTO watched_folders (folder_path, last_scanned_at)"
                    " VALUES (?, NULL);", -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1, emptyDir.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }
        }

        // Scanner needs its own thread; tear down the query thread first to
        // avoid two connections fighting for the WAL during reconciliation.
        ctx.thread.reset();

        struct Listener : WatchFolderScanner::Listener
        {
            std::atomic<bool> done { false };
            void scanProgressUpdate (int, int, const juce::String&) override {}
            void scanCompleted() override { done = true; }
        };

        Listener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForFlag (listener.done, 8000), "Scan did not complete");

        auto fetchMissing = [&] (const char* path) -> int
        {
            sqlite3_stmt* stmt = nullptr;
            int result = -1;
            if (sqlite3_prepare_v2 (h,
                    "SELECT is_missing FROM library_tracks WHERE file_path=?;",
                    -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1, path, -1, SQLITE_TRANSIENT);
                if (sqlite3_step (stmt) == SQLITE_ROW)
                    result = sqlite3_column_int (stmt, 0);
                sqlite3_finalize (stmt);
            }
            return result;
        };

        expectEquals (fetchMissing (realA.getFullPathName().toRawUTF8()), 0, "A stays present");
        expectEquals (fetchMissing (fakeB.getFullPathName().toRawUTF8()), 1, "B is now missing");
        expectEquals (fetchMissing (realC.getFullPathName().toRawUTF8()), 0, "C is restored");

        scanner.removeListener (&listener);
        ctx.db.reset();
        ctx.tmpDir.deleteRecursively();
    }

    // ------------------------------------------------------------------------
    // Test 3: Dedup check rejects relocation to an already-catalogued path
    // ------------------------------------------------------------------------
    void testDedupCheckRejectsExistingPath()
    {
        beginTest ("Dedup query detects an existing file_path for a different id");

        auto ctx = makeContext ("Dedup");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/mft/existing.mp3", "Existing", 0);
        const int64_t existingId = lastInsertRowId (h);

        insertTrack (h, "/mft/broken.mp3", "Broken", 1);
        const int64_t brokenId = lastInsertRowId (h);

        // Simulate the dedup check performed by LibraryComponent::relocateTrackFile
        // and DeckShellComponent::showRelocateDialog: SELECT 1 FROM library_tracks
        // WHERE file_path=? AND id<>? LIMIT 1
        auto isDuplicate = [&] (const char* path, int64_t selfId) -> bool
        {
            sqlite3_stmt* stmt = nullptr;
            bool dup = false;
            if (sqlite3_prepare_v2 (h,
                    "SELECT 1 FROM library_tracks WHERE file_path=? AND id<>? LIMIT 1;",
                    -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text  (stmt, 1, path, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64 (stmt, 2, selfId);
                dup = (sqlite3_step (stmt) == SQLITE_ROW);
                sqlite3_finalize (stmt);
            }
            return dup;
        };

        expect (isDuplicate ("/mft/existing.mp3", brokenId),
                "Relocating broken row to existing path must be flagged as duplicate");
        expect (! isDuplicate ("/mft/existing.mp3", existingId),
                "Probing the same row against its own path must NOT flag a duplicate");
        expect (! isDuplicate ("/mft/brand_new.mp3", brokenId),
                "A truly new path must not be a duplicate");

        destroyContext (ctx);
    }

    // ------------------------------------------------------------------------
    // Test 4: Remove-from-library transaction wipes BOTH tables atomically
    // ------------------------------------------------------------------------
    void testRemoveTrackDeletesPlaylistEntriesAtomically()
    {
        beginTest ("Remove-from-library transaction deletes playlist_tracks and library_tracks");

        auto ctx = makeContext ("RemoveTxn");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/mft/remove_me.mp3", "Remove Me", 1);
        const int64_t trackId = lastInsertRowId (h);

        // Create a playlist and an entry pointing at the track.
        sqlite3_stmt* pl = nullptr;
        if (sqlite3_prepare_v2 (h,
                "INSERT INTO playlists (name, type, sort_order, created_at) "
                "VALUES ('Test', 'normal', 0, strftime('%s','now'));",
                -1, &pl, nullptr) == SQLITE_OK)
        {
            sqlite3_step (pl);
            sqlite3_finalize (pl);
        }
        const int64_t playlistId = lastInsertRowId (h);

        sqlite3_stmt* pt = nullptr;
        if (sqlite3_prepare_v2 (h,
                "INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES (?, ?, 0);",
                -1, &pt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64 (pt, 1, playlistId);
            sqlite3_bind_int64 (pt, 2, trackId);
            sqlite3_step (pt);
            sqlite3_finalize (pt);
        }

        // Sanity: both rows present pre-deletion.
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM library_tracks WHERE file_path='/mft/remove_me.mp3';"), 1);
        expectEquals (queryInt (h,
            "SELECT COUNT(*) FROM playlist_tracks WHERE track_id IN "
            "(SELECT id FROM library_tracks WHERE file_path='/mft/remove_me.mp3');"), 1);

        // Execute the same transactional delete used in production.
        const char* path = "/mft/remove_me.mp3";
        expect (sqlite3_exec (h, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK);

        sqlite3_stmt* del = nullptr;
        sqlite3_prepare_v2 (h,
            "DELETE FROM playlist_tracks WHERE track_id IN "
            "(SELECT id FROM library_tracks WHERE file_path=?);",
            -1, &del, nullptr);
        sqlite3_bind_text (del, 1, path, -1, SQLITE_TRANSIENT);
        expect (sqlite3_step (del) == SQLITE_DONE);
        sqlite3_finalize (del);

        sqlite3_prepare_v2 (h, "DELETE FROM library_tracks WHERE file_path=?;", -1, &del, nullptr);
        sqlite3_bind_text (del, 1, path, -1, SQLITE_TRANSIENT);
        expect (sqlite3_step (del) == SQLITE_DONE);
        sqlite3_finalize (del);

        expect (sqlite3_exec (h, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);

        // Both rows are gone.
        expectEquals (queryInt (h, "SELECT COUNT(*) FROM library_tracks WHERE file_path='/mft/remove_me.mp3';"), 0);
        expectEquals (queryInt (h, ("SELECT COUNT(*) FROM playlist_tracks WHERE track_id=" + juce::String (trackId) + ";").toRawUTF8()), 0);

        destroyContext (ctx);
    }

    // ------------------------------------------------------------------------
    // Test 5: countMissingTracks reflects DB state
    // ------------------------------------------------------------------------
    void testCountMissingTracksReflectsState()
    {
        beginTest ("countMissingTracks reports COUNT(*) WHERE is_missing = 1");

        auto ctx = makeContext ("Count");
        auto* h  = ctx.db->getDbHandle();

        insertTrack (h, "/mft/c_a.mp3", "A", 0);
        insertTrack (h, "/mft/c_b.mp3", "B", 1);
        insertTrack (h, "/mft/c_c.mp3", "C", 1);
        insertTrack (h, "/mft/c_d.mp3", "D", 0);
        insertTrack (h, "/mft/c_e.mp3", "E", 1);

        std::atomic<bool> received { false };
        int observed = -1;
        ctx.thread->countMissingTracks ([&] (int count)
        {
            observed = count;
            received.store (true, std::memory_order_release);
        });

        expect (waitForFlag (received), "Count callback timed out");
        expectEquals (observed, 3, "Expected three rows with is_missing = 1");

        destroyContext (ctx);
    }
};

static MissingFileTests g_missingFileTests;
