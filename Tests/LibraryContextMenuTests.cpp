// =============================================================================
// LibraryContextMenuTests
//
// Covers PRD-0038 acceptance criteria related to:
//   - Status column lifecycle strings (AC-24..AC-28)
//   - Force re-analyze clearing cached analysis state (AC-8)
//   - Stem failure isolation between consecutive jobs (AC-28)
//
// User-triggered queue priority (AC-23) is already covered by
// LibraryAnalysisQueueTests::testUserTriggeredJumpsQueue.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <sqlite3.h>

#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Library/LibraryAnalysisQueue.h"
#include "Features/Library/UI/LibraryStatusText.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class LibraryContextMenuTests final : public juce::UnitTest
{
public:
    LibraryContextMenuTests() : juce::UnitTest ("Library Context Menu", "Sonik") {}

    void runTest() override
    {
        testStatusTextForUpdate();
        testForceReanalyzeClearsCachedAnalysis();
        testStemFailureDoesNotBlockNextJob();
    }

private:
    using JobKind     = LibraryAnalysisQueue::JobKind;
    using JobStatus   = LibraryAnalysisQueue::JobStatus;
    using StatusUpdate = LibraryAnalysisQueue::StatusUpdate;
    using JobContext  = LibraryAnalysisQueue::JobContext;
    using JobExecutor = LibraryAnalysisQueue::JobExecutor;

    static StatusUpdate makeStatus (JobKind kind, JobStatus status, int percent = 0)
    {
        StatusUpdate u;
        u.kind = kind;
        u.status = status;
        u.percent = percent;
        return u;
    }

    static bool waitUntil (const std::function<bool()>& predicate, int timeoutMs = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);

        while (! predicate())
        {
            if (juce::Time::getMillisecondCounter() >= deadline)
                return false;

            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // A. statusTextForUpdate correctness (AC-24..AC-28)
    // -------------------------------------------------------------------------
    void testStatusTextForUpdate()
    {
        beginTest ("statusTextForUpdate maps every (kind, status, percent) combination");

        using namespace SonikLibraryUi;

        expectEquals (statusTextForUpdate (makeStatus (JobKind::Analysis,       JobStatus::Queued)),
                      juce::String ("Queued"));
        expectEquals (statusTextForUpdate (makeStatus (JobKind::StemSeparation, JobStatus::Queued)),
                      juce::String ("Queued (Stems)"));

        expectEquals (statusTextForUpdate (makeStatus (JobKind::Analysis,       JobStatus::Running, 0)),
                      juce::String ("Analyzing 0%"));
        expectEquals (statusTextForUpdate (makeStatus (JobKind::Analysis,       JobStatus::Running, 50)),
                      juce::String ("Analyzing 50%"));
        expectEquals (statusTextForUpdate (makeStatus (JobKind::StemSeparation, JobStatus::Running, 25)),
                      juce::String ("Separating Stems 25%"));

        expectEquals (statusTextForUpdate (makeStatus (JobKind::Analysis,       JobStatus::Complete)),
                      juce::String ("Complete"));
        expectEquals (statusTextForUpdate (makeStatus (JobKind::StemSeparation, JobStatus::Complete)),
                      juce::String ("Stem Complete"));

        expectEquals (statusTextForUpdate (makeStatus (JobKind::Analysis,       JobStatus::Failed)),
                      juce::String ("Failed"));
        expectEquals (statusTextForUpdate (makeStatus (JobKind::StemSeparation, JobStatus::Failed)),
                      juce::String ("Stem Failed"));
    }

    // -------------------------------------------------------------------------
    // B. Force re-analyze clears cached analysis state (AC-8)
    // -------------------------------------------------------------------------
    struct DbContext
    {
        juce::File                     tmpDir;
        juce::File                     dbFile;
        std::unique_ptr<TrackDatabase> db;
    };

    DbContext makeDbContext (const juce::String& name)
    {
        DbContext ctx;
        ctx.tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("SonikLibCtx_" + name + "_"
                                        + juce::String (juce::Time::currentTimeMillis()));
        ctx.tmpDir.createDirectory();
        ctx.dbFile = ctx.tmpDir.getChildFile ("test.db");
        ctx.db = std::make_unique<TrackDatabase> (ctx.dbFile);
        return ctx;
    }

    void destroyDbContext (DbContext& ctx)
    {
        ctx.db.reset();
        ctx.tmpDir.deleteRecursively();
    }

    static void insertAnalyzedTrackRow (TrackDatabase& db,
                                        const juce::String& filePath,
                                        const juce::String& contentHash,
                                        int keyIndex)
    {
        auto* handle = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        const auto rc = sqlite3_prepare_v2 (handle,
            "INSERT INTO track_data (file_path, content_hash, beatgrid_json, key_index, key_confidence) "
            "VALUES (?, ?, ?, ?, ?);",
            -1, &stmt, nullptr);

        if (rc == SQLITE_OK)
        {
            sqlite3_bind_text   (stmt, 1, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text   (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text   (stmt, 3, "{\"bpm\":120.0}",       -1, SQLITE_TRANSIENT);
            sqlite3_bind_int    (stmt, 4, keyIndex);
            sqlite3_bind_double (stmt, 5, 0.9);
            sqlite3_step (stmt);
            sqlite3_finalize (stmt);
        }
    }

    static bool rowExists (TrackDatabase& db,
                           const juce::String& filePath,
                           const juce::String& contentHash)
    {
        auto* handle = db.getDbHandle();
        sqlite3_stmt* stmt = nullptr;
        bool exists = false;

        if (sqlite3_prepare_v2 (handle,
                                "SELECT 1 FROM track_data WHERE file_path=? AND content_hash=?",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
            exists = (sqlite3_step (stmt) == SQLITE_ROW);
            sqlite3_finalize (stmt);
        }
        return exists;
    }

    void testForceReanalyzeClearsCachedAnalysis()
    {
        beginTest ("Force re-analyze clears cached analysis state for a track");

        auto ctx = makeDbContext ("Force");

        const juce::String filePath    = "/library/track-force-001.wav";
        const juce::String contentHash = "hash-force-001";

        insertAnalyzedTrackRow (*ctx.db, filePath, contentHash, /*keyIndex=*/ 7);

        expect (rowExists (*ctx.db, filePath, contentHash),
                "Pre-condition: analyzed row should be present before re-analyze");

        SonikLibraryUi::clearAnalysisCache (*ctx.db, filePath, contentHash);

        expect (! rowExists (*ctx.db, filePath, contentHash),
                "After force re-analyze, the cached track_data row should be removed");

        // And clearing again on a non-existent row must be a safe no-op.
        SonikLibraryUi::clearAnalysisCache (*ctx.db, filePath, contentHash);
        expect (! rowExists (*ctx.db, filePath, contentHash),
                "Clearing an already-cleared row remains a no-op");

        destroyDbContext (ctx);
    }

    // -------------------------------------------------------------------------
    // D. Stem failure isolation (AC-28)
    // -------------------------------------------------------------------------
    void testStemFailureDoesNotBlockNextJob()
    {
        beginTest ("A failed stem job does not prevent the next stem job from running and completing");

        std::atomic<int> stemCallCount { 0 };

        auto stemExecutor = [&stemCallCount] (const JobContext& ctx)
        {
            const auto call = stemCallCount.fetch_add (1, std::memory_order_acq_rel) + 1;
            juce::ignoreUnused (ctx);
            return call > 1; // first call fails, subsequent calls succeed
        };

        auto immediateAnalysis = [] (const JobContext&) { return true; };

        LibraryAnalysisQueue queue (immediateAnalysis, stemExecutor);

        std::vector<StatusUpdate> updates;
        std::mutex updatesMutex;

        queue.setStatusCallback ([&] (const StatusUpdate& u)
        {
            std::lock_guard<std::mutex> lock (updatesMutex);
            updates.push_back (u);
        });

        queue.enqueueStemSeparation (1, "/stem/fail.wav", "h-fail",    false);
        queue.enqueueStemSeparation (2, "/stem/ok.wav",   "h-success", false);

        const bool sawBothTerminal = waitUntil ([&]
        {
            std::lock_guard<std::mutex> lock (updatesMutex);
            const bool sawFailed1 = std::any_of (updates.begin(), updates.end(), [] (const StatusUpdate& u)
            {
                return u.trackId == 1
                    && u.kind == JobKind::StemSeparation
                    && u.status == JobStatus::Failed;
            });
            const bool sawComplete2 = std::any_of (updates.begin(), updates.end(), [] (const StatusUpdate& u)
            {
                return u.trackId == 2
                    && u.kind == JobKind::StemSeparation
                    && u.status == JobStatus::Complete;
            });
            return sawFailed1 && sawComplete2;
        });

        expect (sawBothTerminal,
                "Track 1 must emit Failed and track 2 must emit Complete despite the earlier failure");
        expectEquals (stemCallCount.load(), 2,
                      "Stem executor must be invoked for both jobs (failure must not skip later jobs)");
    }
};

static LibraryContextMenuTests libraryContextMenuTests;
