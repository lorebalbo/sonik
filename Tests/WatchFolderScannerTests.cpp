#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_cryptography/juce_cryptography.h>
#include "Features/Library/WatchFolderScanner.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include <sqlite3.h>

class WatchFolderScannerTests : public juce::UnitTest
{
public:
    WatchFolderScannerTests() : juce::UnitTest ("Watch Folder Scanner", "Sonik") {}

    void runTest() override
    {
        testSupportedExtensionsIngested();
        testUnsupportedExtensionsIgnored();
        testNoOpOnUnchangedFile();
        testUpsertOnChangedHash();
        testTitleFallback();
        testMissingFileReconciliation();
        testRestoredFileReconciliation();
        testRecursiveDirectoryWalk();
        testLastScannedAtOnCancellation();
        testGetWatchedFolders();
        testRemoveWatchedFolder();
    }

private:
    // =========================================================================
    // Helpers
    // =========================================================================

    /// RAII temp directory + isolated database for each test.
    struct TestContext
    {
        juce::File tmpDir;
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;

        TestContext()
        {
            tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("sonik_wfs_test_"
                                        + juce::String (juce::Random::getSystemRandom().nextInt()));
            tmpDir.createDirectory();
            dbFile = tmpDir.getChildFile ("test.db");
            db     = std::make_unique<TrackDatabase> (dbFile);
        }

        ~TestContext()
        {
            db.reset();
            tmpDir.deleteRecursively();
        }
    };

    /// Listener that records scan completion.
    struct TestListener : WatchFolderScanner::Listener
    {
        std::atomic<bool> completed { false };

        void scanProgressUpdate (int, int, const juce::String&) override {}
        void scanCompleted() override { completed = true; }
    };

    /// Pump the JUCE message loop while waiting for the completion flag, or until
    /// timeoutMs elapses.  Returns true iff the flag was set before timeout.
    bool waitForCompletion (std::atomic<bool>& flag, int timeoutMs = 8000)
    {
        const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
        while (!flag.load() && juce::Time::currentTimeMillis() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        return flag.load();
    }

    /// Insert a row into watched_folders WITHOUT triggering a scan.
    void addWatchedFolderDirect (TrackDatabase& db, const juce::String& path)
    {
        auto* h = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT OR IGNORE INTO watched_folders (folder_path, last_scanned_at)"
            " VALUES (?, NULL);";
        if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, path.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_step (stmt);
            sqlite3_finalize (stmt);
        }
    }

    /// Total rows in library_tracks.
    int countAllTracks (TrackDatabase& db)
    {
        auto* h = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        int count = 0;
        if (sqlite3_prepare_v2 (h, "SELECT COUNT(*) FROM library_tracks;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                count = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return count;
    }

    /// Rows in library_tracks matching the given file_path.
    int countTracksWithPath (TrackDatabase& db, const juce::String& path)
    {
        auto* h = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        int count = 0;
        if (sqlite3_prepare_v2 (h,
                                "SELECT COUNT(*) FROM library_tracks WHERE file_path = ?;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, path.toRawUTF8(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step (stmt) == SQLITE_ROW)
                count = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return count;
    }

    /// Query a TEXT column for a given file_path.
    juce::String queryTextForPath (TrackDatabase& db,
                                    const juce::String& filePath,
                                    const char* column)
    {
        auto* h = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        juce::String result;
        const juce::String sql =
            juce::String ("SELECT ") + column
            + " FROM library_tracks WHERE file_path = ?;";
        if (sqlite3_prepare_v2 (h, sql.toRawUTF8(), -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step (stmt) == SQLITE_ROW)
            {
                const auto* text =
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
                if (text != nullptr)
                    result = juce::String::fromUTF8 (text);
            }
            sqlite3_finalize (stmt);
        }
        return result;
    }

    /// Query an INTEGER column for a given file_path.  Returns -1 if no row.
    int queryIntForPath (TrackDatabase& db,
                         const juce::String& filePath,
                         const char* column)
    {
        auto* h = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        int result = -1;
        const juce::String sql =
            juce::String ("SELECT ") + column
            + " FROM library_tracks WHERE file_path = ?;";
        if (sqlite3_prepare_v2 (h, sql.toRawUTF8(), -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step (stmt) == SQLITE_ROW)
                result = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return result;
    }

    /// Write unique text content to a file so its SHA-256 is distinct from others.
    static void writeUniqueContent (const juce::File& file, int seed)
    {
        juce::FileOutputStream out (file);
        out.writeText ("sonik_test_content_seed=" + juce::String (seed)
                       + "_padding_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                       false, false, nullptr);
    }

    // =========================================================================
    // Test cases
    // =========================================================================

    void testSupportedExtensionsIngested()
    {
        beginTest ("Supported audio extensions are ingested into library_tracks");

        TestContext ctx;

        // Create one audio file per supported extension with UNIQUE content
        // so their SHA-256 hashes are distinct (avoids duplicate-content guard).
        const juce::StringArray audioExts { ".mp3", ".flac", ".wav",
                                            ".aiff", ".aif", ".ogg", ".m4a" };
        int seed = 0;
        for (const auto& ext : audioExts)
        {
            auto f = ctx.tmpDir.getChildFile ("track" + ext);
            f.create();
            writeUniqueContent (f, seed++);
        }

        // Non-audio files — must NOT be ingested.
        for (const auto& ext : juce::StringArray { ".txt", ".jpg", ".pdf", ".png" })
            ctx.tmpDir.getChildFile ("file" + ext).create();

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");
        expectEquals (countAllTracks (*ctx.db), audioExts.size());
    }

    void testUnsupportedExtensionsIgnored()
    {
        beginTest ("Non-audio files are silently ignored by the scanner");

        TestContext ctx;

        ctx.tmpDir.getChildFile ("readme.txt").create();
        ctx.tmpDir.getChildFile ("cover.jpg").create();
        ctx.tmpDir.getChildFile ("notes.pdf").create();
        ctx.tmpDir.getChildFile ("image.png").create();

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");
        expectEquals (countAllTracks (*ctx.db), 0);
    }

    void testNoOpOnUnchangedFile()
    {
        beginTest ("No database write when file_path and content_hash are unchanged");

        TestContext ctx;

        auto trackFile = ctx.tmpDir.getChildFile ("unchanged.wav");
        trackFile.create();
        writeUniqueContent (trackFile, 42);

        // Compute the SAME content hash the scanner will compute (engine-canonical
        // MD5 heuristic), so the pre-inserted row is genuinely "unchanged" from
        // the scanner's perspective and the no-op path is actually exercised.
        const juce::String hash = WatchFolderScanner::computeContentHash (trackFile);

        // Pre-insert the row with a known date_added sentinel value.
        constexpr int64_t kSentinelDateAdded = 1000000;
        {
            auto* h = ctx.db->getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "INSERT INTO library_tracks"
                " (file_path, content_hash, title, date_added, last_seen, is_missing)"
                " VALUES (?, ?, 'Pre-Inserted Track', ?, ?, 0);";
            if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text  (stmt, 1, trackFile.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text  (stmt, 2, hash.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64 (stmt, 3, kSentinelDateAdded);
                sqlite3_bind_int64 (stmt, 4, kSentinelDateAdded);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }
        }

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");

        // Exactly one row must remain.
        expectEquals (countTracksWithPath (*ctx.db, trackFile.getFullPathName()), 1);

        // date_added must be the original sentinel — no re-insert / overwrite occurred.
        expectEquals (queryIntForPath (*ctx.db, trackFile.getFullPathName(), "date_added"),
                      static_cast<int> (kSentinelDateAdded));
    }

    void testUpsertOnChangedHash()
    {
        beginTest ("content_hash is updated in-place when file content changes");

        TestContext ctx;

        auto trackFile = ctx.tmpDir.getChildFile ("changing.wav");
        trackFile.create();
        writeUniqueContent (trackFile, 100);

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        // --- First scan: inserts the track ---
        {
            TestListener listener;
            WatchFolderScanner scanner (*ctx.db);
            scanner.addListener (&listener);
            scanner.startScan();
            expect (waitForCompletion (listener.completed), "First scan did not complete");
        }

        const juce::String hashV1 =
            queryTextForPath (*ctx.db, trackFile.getFullPathName(), "content_hash");
        expect (hashV1.isNotEmpty(), "Hash must be set after first scan");

        // Modify file content.
        writeUniqueContent (trackFile, 999);

        // Force a full (non-incremental) rescan by nullifying last_scanned_at.
        sqlite3_exec (ctx.db->getDbHandle(),
                      "UPDATE watched_folders SET last_scanned_at = NULL;",
                      nullptr, nullptr, nullptr);

        // --- Second scan: should update hash ---
        {
            TestListener listener;
            WatchFolderScanner scanner (*ctx.db);
            scanner.addListener (&listener);
            scanner.startScan();
            expect (waitForCompletion (listener.completed), "Second scan did not complete");
        }

        const juce::String hashV2 =
            queryTextForPath (*ctx.db, trackFile.getFullPathName(), "content_hash");
        expect (hashV2.isNotEmpty(), "Hash must be set after second scan");
        expect (hashV1 != hashV2, "Hash must differ after content changed");

        // Must still be exactly one row (no duplicate inserted).
        expectEquals (countTracksWithPath (*ctx.db, trackFile.getFullPathName()), 1);
    }

    void testTitleFallback()
    {
        beginTest ("title falls back to filename-without-extension when no tags present");

        TestContext ctx;

        // An empty file has no metadata tags at all.
        auto trackFile = ctx.tmpDir.getChildFile ("my_awesome_track.wav");
        trackFile.create();

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");

        const juce::String title =
            queryTextForPath (*ctx.db, trackFile.getFullPathName(), "title");
        expectEquals (title, juce::String ("my_awesome_track"));
    }

    void testMissingFileReconciliation()
    {
        beginTest ("Reconciliation sets is_missing=1 for tracks whose file is gone");

        TestContext ctx;

        // Insert a fake track with a path that does NOT exist on disk.
        const juce::String fakePath =
            ctx.tmpDir.getChildFile ("ghost_track.wav").getFullPathName();
        {
            auto* h = ctx.db->getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "INSERT INTO library_tracks"
                " (file_path, content_hash, title, date_added, last_seen, is_missing)"
                " VALUES (?, 'deadbeef', 'Ghost Track', 1000000, 1000000, 0);";
            if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1, fakePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }
        }

        // The scanner needs at least one watched folder to run (and reach the
        // reconciliation pass).  Use an empty subdirectory.
        auto emptyDir = ctx.tmpDir.getChildFile ("watched_empty");
        emptyDir.createDirectory();
        addWatchedFolderDirect (*ctx.db, emptyDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");

        expectEquals (queryIntForPath (*ctx.db, fakePath, "is_missing"), 1);
    }

    void testRestoredFileReconciliation()
    {
        beginTest ("Reconciliation resets is_missing=0 for tracks whose file returns");

        TestContext ctx;

        // A real file that exists on disk — but the DB row says is_missing=1.
        auto realFile = ctx.tmpDir.getChildFile ("restored_track.wav");
        realFile.create();
        writeUniqueContent (realFile, 77);

        {
            auto* h = ctx.db->getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "INSERT INTO library_tracks"
                " (file_path, content_hash, title, date_added, last_seen, is_missing)"
                " VALUES (?, 'abc123', 'Restored Track', 1000000, 1000000, 1);";
            if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1, realFile.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }
        }

        // Empty watched folder so the scanner runs through to the reconciliation pass.
        auto emptyDir = ctx.tmpDir.getChildFile ("watched_empty");
        emptyDir.createDirectory();
        addWatchedFolderDirect (*ctx.db, emptyDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");

        expectEquals (queryIntForPath (*ctx.db, realFile.getFullPathName(), "is_missing"), 0);
    }

    void testRecursiveDirectoryWalk()
    {
        beginTest ("Scanner walks nested subdirectories recursively");

        TestContext ctx;

        // Create: tmpDir/sub1/sub2/nested_track.wav
        auto sub2 = ctx.tmpDir.getChildFile ("sub1").getChildFile ("sub2");
        sub2.createDirectory();
        auto nestedFile = sub2.getChildFile ("nested_track.wav");
        nestedFile.create();
        writeUniqueContent (nestedFile, 55);

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);
        scanner.startScan();

        expect (waitForCompletion (listener.completed), "Scan did not complete in time");

        expectEquals (countTracksWithPath (*ctx.db, nestedFile.getFullPathName()), 1);
    }

    void testLastScannedAtOnCancellation()
    {
        // This test verifies the invariant:
        //   - If scan completes:  last_scanned_at IS set (non-NULL).
        //   - If scan is cancelled: last_scanned_at is NOT set (NULL).
        // Both branches are valid, and we assert the correct outcome depending
        // on which one the OS scheduler produces.
        beginTest ("last_scanned_at invariant: NULL when cancelled, non-NULL when complete");

        TestContext ctx;

        // Create several files.  The unique content ensures distinct hashes.
        for (int i = 0; i < 5; ++i)
        {
            auto f = ctx.tmpDir.getChildFile ("track" + juce::String (i) + ".wav");
            f.create();
            writeUniqueContent (f, i);
        }

        addWatchedFolderDirect (*ctx.db, ctx.tmpDir.getFullPathName());

        TestListener listener;
        WatchFolderScanner scanner (*ctx.db);
        scanner.addListener (&listener);

        scanner.startScan();
        scanner.cancelScan(); // signals + blocks until thread exits (≤ 2 s)

        // Drain any callAsync messages that were already posted before the thread stopped.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        const bool scanDidComplete = listener.completed.load();

        // Query last_scanned_at for the watched folder.
        bool lastScannedAtIsNull = true;
        {
            auto* h = ctx.db->getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT last_scanned_at FROM watched_folders WHERE folder_path = ?;";
            if (sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1,
                                   ctx.tmpDir.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step (stmt) == SQLITE_ROW)
                    lastScannedAtIsNull = (sqlite3_column_type (stmt, 0) == SQLITE_NULL);
                sqlite3_finalize (stmt);
            }
        }

        if (scanDidComplete)
        {
            // Scan finished before the cancellation signal was observed.
            // last_scanned_at MUST be set (non-NULL).
            expect (!lastScannedAtIsNull,
                    "last_scanned_at must be set after a completed scan");
        }
        else
        {
            // Scan was actually cancelled — last_scanned_at MUST remain NULL.
            expect (lastScannedAtIsNull,
                    "last_scanned_at must remain NULL when scan is cancelled");
        }
    }

    void testGetWatchedFolders()
    {
        beginTest ("getWatchedFolders returns all registered folders");

        TestContext ctx;

        auto folder1 = ctx.tmpDir.getChildFile ("watch1");
        auto folder2 = ctx.tmpDir.getChildFile ("watch2");
        folder1.createDirectory();
        folder2.createDirectory();

        addWatchedFolderDirect (*ctx.db, folder1.getFullPathName());
        addWatchedFolderDirect (*ctx.db, folder2.getFullPathName());

        WatchFolderScanner scanner (*ctx.db);
        const auto folders = scanner.getWatchedFolders();

        expectEquals (folders.size(), 2);
        expect (folders.contains (folder1.getFullPathName()),
                "folder1 must appear in getWatchedFolders");
        expect (folders.contains (folder2.getFullPathName()),
                "folder2 must appear in getWatchedFolders");
    }

    void testRemoveWatchedFolder()
    {
        beginTest ("removeWatchedFolder removes exactly the specified folder");

        TestContext ctx;

        auto folder1 = ctx.tmpDir.getChildFile ("watch1");
        auto folder2 = ctx.tmpDir.getChildFile ("watch2");
        folder1.createDirectory();
        folder2.createDirectory();

        addWatchedFolderDirect (*ctx.db, folder1.getFullPathName());
        addWatchedFolderDirect (*ctx.db, folder2.getFullPathName());

        WatchFolderScanner scanner (*ctx.db);
        scanner.removeWatchedFolder (folder1);

        const auto folders = scanner.getWatchedFolders();

        expectEquals (folders.size(), 1);
        expect (!folders.contains (folder1.getFullPathName()),
                "folder1 must be absent after removal");
        expect (folders.contains (folder2.getFullPathName()),
                "folder2 must still be present after removing folder1");
    }
};

static WatchFolderScannerTests watchFolderScannerTestsInstance;
