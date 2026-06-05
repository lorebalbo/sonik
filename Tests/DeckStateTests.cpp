#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include <sqlite3.h>

class DeckStateTests : public juce::UnitTest
{
public:
    DeckStateTests() : juce::UnitTest ("Deck State Management", "Sonik") {}

    void runTest() override
    {
        testInitialization();
        testDeckAddition();
        testDeckRemoval();
        testDeckIndependence();
        testStateMachineTransitions();
        testTrackLoading();
        testTrackLoadingProjectsPersistedKey();
        testRecordedClipSourceResolvesAfterLoad();
        testTrackEjection();
        testActiveDeck();
        testMasterTempo();
        testLetterAssignment();
        testAudioStateSync();
        testSessionPersistence();
        testDatabaseOperations();
    }

private:
    // Helper to create a temp database for each test section
    struct TestContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> mgr;

        TestContext()
        {
            dbFile = juce::File::createTempFile ("sonik_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            mgr = std::make_unique<DeckStateManager> (*db);
        }

        ~TestContext()
        {
            mgr.reset();
            db.reset();
            dbFile.deleteFile();
        }
    };

    TrackMetadata makeSampleMetadata (const juce::String& title = "Test Track")
    {
        TrackMetadata meta;
        meta.filePath     = "/path/to/" + title + ".wav";
        meta.contentHash  = "hash_" + title;
        meta.title        = title;
        meta.artist       = "Test Artist";
        meta.album        = "Test Album";
        meta.duration     = 180.0;
        meta.sampleRate   = 44100.0;
        meta.bitDepth     = 16;
        meta.totalSamples = 7938000;
        meta.hasAlbumArt  = false;
        return meta;
    }

    int execSql (sqlite3* handle, const char* sql)
    {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec (handle, sql, nullptr, nullptr, &errMsg);
        if (errMsg != nullptr)
            sqlite3_free (errMsg);
        return rc;
    }

    juce::String queryText (sqlite3* handle, const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        juce::String result;
        if (sqlite3_prepare_v2 (handle, sql, -1, &stmt, nullptr) == SQLITE_OK)
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

    int queryInt (sqlite3* handle, const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        int result = -1;
        if (sqlite3_prepare_v2 (handle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step (stmt) == SQLITE_ROW)
                result = sqlite3_column_int (stmt, 0);
            sqlite3_finalize (stmt);
        }
        return result;
    }

    // -----------------------------------------------------------------------
    void testInitialization()
    {
        beginTest ("Initialization - starts with 0 decks");
        TestContext ctx;
        expectEquals (ctx.mgr->getDeckCount(), 0);

        beginTest ("Initialization - addDeck creates deck A");
        auto id = ctx.mgr->addDeck();
        expectEquals (id, juce::String ("A"));
        expectEquals (ctx.mgr->getDeckCount(), 1);

        beginTest ("Initialization - deck has correct default state");
        auto deckTree = ctx.mgr->getDeckState ("A");
        expect (deckTree.isValid());
        expectEquals (deckTree.getProperty (IDs::playbackStatus).toString(), juce::String ("empty"));
        expectEquals (static_cast<float> (deckTree.getProperty (IDs::gain)), 1.0f);
        expect (! static_cast<bool> (deckTree.getProperty (IDs::quantizeEnabled)));
        expect (! static_cast<bool> (deckTree.getProperty (IDs::slipEnabled)));
        expectEquals (static_cast<float> (deckTree.getProperty (IDs::pitch)), 0.0f);
    }

    // -----------------------------------------------------------------------
    void testDeckAddition()
    {
        beginTest ("Deck Addition - letters A, B, C, D assigned in order");
        TestContext ctx;

        auto a = ctx.mgr->addDeck();
        auto b = ctx.mgr->addDeck();
        auto c = ctx.mgr->addDeck();
        auto d = ctx.mgr->addDeck();

        expectEquals (a, juce::String ("A"));
        expectEquals (b, juce::String ("B"));
        expectEquals (c, juce::String ("C"));
        expectEquals (d, juce::String ("D"));
        expectEquals (ctx.mgr->getDeckCount(), 4);

        beginTest ("Deck Addition - 5th add returns empty string");
        auto e = ctx.mgr->addDeck();
        expect (e.isEmpty());
        expectEquals (ctx.mgr->getDeckCount(), 4);
    }

    // -----------------------------------------------------------------------
    void testDeckRemoval()
    {
        beginTest ("Deck Removal - can remove deck");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        expect (ctx.mgr->removeDeck ("B"));
        expectEquals (ctx.mgr->getDeckCount(), 1);

        beginTest ("Deck Removal - cannot remove playing deck");
        ctx.mgr->addDeck(); // B again
        ctx.mgr->loadTrack ("B", makeSampleMetadata());
        expect (ctx.mgr->setPlaybackStatus ("B", "playing"));
        expect (! ctx.mgr->canRemoveDeck ("B"));
        expect (! ctx.mgr->removeDeck ("B"));

        beginTest ("Deck Removal - cannot go below 1 deck");
        // Stop B so we can test min-deck rule on A
        expect (ctx.mgr->setPlaybackStatus ("B", "stopped"));
        // With 2 decks, A can be removed (would leave 1)
        expect (ctx.mgr->canRemoveDeck ("A"));
        // Remove B first, then try A — should fail (would go below 1)
        expect (ctx.mgr->removeDeck ("B"));
        expectEquals (ctx.mgr->getDeckCount(), 1);
        expect (! ctx.mgr->canRemoveDeck ("A"));
        expect (! ctx.mgr->removeDeck ("A"));

        beginTest ("Deck Removal - letter becomes available after removal");
        ctx.mgr->addDeck(); // Should get B
        auto newId = ctx.mgr->addDeck(); // Should get C? No, B was just re-added. Let's check.
        // After removing B then re-adding, we got B. Then adding again should give C.
        // The first addDeck after removing B fills gap => returns B
        // Actually we already called addDeck once (got B), then addDeck again => C
        expectEquals (newId, juce::String ("C"));
    }

    // -----------------------------------------------------------------------
    void testDeckIndependence()
    {
        beginTest ("Deck Independence - mutating one deck does not affect another");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        // Modify deck A
        auto deckA = ctx.mgr->getDeckState ("A");
        deckA.setProperty (IDs::gain, 0.5f, nullptr);
        deckA.setProperty (IDs::quantizeEnabled, true, nullptr);

        // Verify deck B is unaffected
        auto deckB = ctx.mgr->getDeckState ("B");
        expectEquals (static_cast<float> (deckB.getProperty (IDs::gain)), 1.0f);
        expect (! static_cast<bool> (deckB.getProperty (IDs::quantizeEnabled)));

        // Load a track on A, verify B has no track
        ctx.mgr->loadTrack ("A", makeSampleMetadata ("TrackA"));
        auto metaA = ctx.mgr->getDeckState ("A").getChildWithName (IDs::TrackMetadata);
        auto metaB = ctx.mgr->getDeckState ("B").getChildWithName (IDs::TrackMetadata);
        expectEquals (metaA.getProperty (IDs::title).toString(), juce::String ("TrackA"));
        expectEquals (metaB.getProperty (IDs::title).toString(), juce::String (""));
    }

    // -----------------------------------------------------------------------
    void testStateMachineTransitions()
    {
        beginTest ("State Machine - valid transitions");
        TestContext ctx;
        ctx.mgr->addDeck(); // A

        // empty -> stopped (via loadTrack)
        ctx.mgr->loadTrack ("A", makeSampleMetadata());
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("stopped"));

        // stopped -> playing
        expect (ctx.mgr->setPlaybackStatus ("A", "playing"));
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("playing"));

        // playing -> paused
        expect (ctx.mgr->setPlaybackStatus ("A", "paused"));
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("paused"));

        // paused -> playing
        expect (ctx.mgr->setPlaybackStatus ("A", "playing"));
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("playing"));

        // playing -> stopped
        expect (ctx.mgr->setPlaybackStatus ("A", "stopped"));
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("stopped"));

        // paused -> stopped
        expect (ctx.mgr->setPlaybackStatus ("A", "playing"));
        expect (ctx.mgr->setPlaybackStatus ("A", "paused"));
        expect (ctx.mgr->setPlaybackStatus ("A", "stopped"));
        expectEquals (ctx.mgr->getDeckState ("A").getProperty (IDs::playbackStatus).toString(),
                      juce::String ("stopped"));

        beginTest ("State Machine - invalid transitions");
        // stopped -> paused (invalid)
        expect (! ctx.mgr->setPlaybackStatus ("A", "paused"));

        // empty -> playing (invalid) -- eject first
        expect (ctx.mgr->ejectTrack ("A"));
        expect (! ctx.mgr->setPlaybackStatus ("A", "playing"));

        // empty -> paused (invalid)
        expect (! ctx.mgr->setPlaybackStatus ("A", "paused"));

        beginTest ("State Machine - transition on invalid deck returns false");
        expect (! ctx.mgr->setPlaybackStatus ("Z", "playing"));
    }

    // -----------------------------------------------------------------------
    void testTrackLoading()
    {
        beginTest ("Track Loading - populates metadata");
        TestContext ctx;
        ctx.mgr->addDeck(); // A

        auto meta = makeSampleMetadata ("My Song");
        ctx.mgr->loadTrack ("A", meta);

        auto deckTree = ctx.mgr->getDeckState ("A");
        auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
        expectEquals (trackMeta.getProperty (IDs::title).toString(), juce::String ("My Song"));
        expectEquals (trackMeta.getProperty (IDs::artist).toString(), juce::String ("Test Artist"));
        expectEquals (static_cast<double> (trackMeta.getProperty (IDs::duration)), 180.0);
        expectEquals (static_cast<double> (trackMeta.getProperty (IDs::sampleRate)), 44100.0);

        beginTest ("Track Loading - sets status to stopped");
        expectEquals (deckTree.getProperty (IDs::playbackStatus).toString(), juce::String ("stopped"));

        beginTest ("Track Loading - resets pitch to 0");
        expectEquals (static_cast<float> (deckTree.getProperty (IDs::pitch)), 0.0f);

        beginTest ("Track Loading - deck-level state persists (gain, quantize, slip)");
        // Set deck-level state first
        deckTree.setProperty (IDs::gain, 0.7f, nullptr);
        deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);
        deckTree.setProperty (IDs::slipEnabled, true, nullptr);

        // Load a new track
        ctx.mgr->loadTrack ("A", makeSampleMetadata ("New Song"));

        auto updatedDeck = ctx.mgr->getDeckState ("A");
        expectEquals (static_cast<float> (updatedDeck.getProperty (IDs::gain)), 0.7f);
        expect (static_cast<bool> (updatedDeck.getProperty (IDs::quantizeEnabled)));
        expect (static_cast<bool> (updatedDeck.getProperty (IDs::slipEnabled)));
        // But pitch should be reset
        expectEquals (static_cast<float> (updatedDeck.getProperty (IDs::pitch)), 0.0f);

        beginTest ("Track Loading - resets playhead position");
        auto playhead = updatedDeck.getChildWithName (IDs::Playhead);
        expectEquals (static_cast<int64_t> (playhead.getProperty (IDs::position)), (int64_t) 0);

        beginTest ("Track Loading - cannot load while playing");
        expect (ctx.mgr->setPlaybackStatus ("A", "playing"));
        auto oldTitle = ctx.mgr->getDeckState ("A")
                            .getChildWithName (IDs::TrackMetadata)
                            .getProperty (IDs::title).toString();
        ctx.mgr->loadTrack ("A", makeSampleMetadata ("Blocked Track"));
        auto newTitle = ctx.mgr->getDeckState ("A")
                            .getChildWithName (IDs::TrackMetadata)
                            .getProperty (IDs::title).toString();
        expectEquals (newTitle, oldTitle);
    }

    void testTrackLoadingProjectsPersistedKey()
    {
        beginTest ("Track Loading - persisted key updates library row when hashes differ");
        TestContext ctx;
        ctx.mgr->addDeck();

        auto meta = makeSampleMetadata ("CachedKeyTrack");
        meta.contentHash = "deck_loader_hash";

        auto* handle = ctx.db->getDbHandle();
        expectEquals (execSql (handle,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, artist, album, date_added) "
            "VALUES "
            "  ('/path/to/CachedKeyTrack.wav', 'scanner_sha256_hash', "
            "   'CachedKeyTrack', 'Test Artist', 'Test Album', 1000);"),
            SQLITE_OK);

        ctx.db->saveTrackData (meta.filePath, meta.contentHash,
                               {}, {}, 19, 0.9f, false);

        ctx.mgr->loadTrack ("A", meta);

        const auto key = queryText (
            handle, "SELECT key FROM library_tracks WHERE file_path='/path/to/CachedKeyTrack.wav';");
        expectEquals (key, juce::String ("8A"));

        const int keyIndex = queryInt (
            handle, "SELECT key_index FROM library_tracks WHERE file_path='/path/to/CachedKeyTrack.wav';");
        expectEquals (keyIndex, 7);
    }

    // -----------------------------------------------------------------------
    // EPIC-0010 playback: a clip recorded from a deck carries the deck's content
    // hash as its sourceFileId. ClipSourceResolver maps that back to a file via
    // TrackDatabase::getFilePathForContentHash(). Historically the watch-folder
    // scanner wrote a DIFFERENT hash (full-file SHA-256) into library_tracks than
    // the deck computes (MD5 heuristic), so the reverse lookup never matched and
    // recorded clips played silence. Loading a track must reconcile the library
    // row so the deck/clip hash resolves.
    void testRecordedClipSourceResolvesAfterLoad()
    {
        beginTest ("Track Loading - recorded clip source resolves after deck load (EPIC-0010)");
        TestContext ctx;
        ctx.mgr->addDeck();

        // A library row scanned under a stale/different hash than the deck uses.
        auto* handle = ctx.db->getDbHandle();
        expectEquals (execSql (handle,
            "INSERT INTO library_tracks "
            "  (file_path, content_hash, title, date_added) "
            "VALUES "
            "  ('/music/set_track.wav', 'stale_scanner_hash', 'Set Track', 1000);"),
            SQLITE_OK);

        // Precondition (the bug): the deck/clip hash does not resolve yet.
        expect (ctx.db->getFilePathForContentHash ("deck_clip_hash").isEmpty(),
                "deck/clip hash must not resolve before the load reconciles it");

        // Loading the track stamps the deck hash and reconciles the library row.
        auto meta = makeSampleMetadata ("Set Track");
        meta.filePath    = "/music/set_track.wav";
        meta.contentHash = "deck_clip_hash";
        ctx.mgr->loadTrack ("A", meta);

        // A clip recorded from this deck (sourceFileId == deck hash) now resolves
        // back to its source file at playback time.
        expectEquals (ctx.db->getFilePathForContentHash ("deck_clip_hash"),
                      juce::String ("/music/set_track.wav"),
                      "deck/clip hash must resolve to the source path after load");
    }

    // -----------------------------------------------------------------------
    void testTrackEjection()
    {
        beginTest ("Track Ejection - returns deck to empty");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->loadTrack ("A", makeSampleMetadata());

        expect (ctx.mgr->ejectTrack ("A"));
        auto deckTree = ctx.mgr->getDeckState ("A");
        expectEquals (deckTree.getProperty (IDs::playbackStatus).toString(), juce::String ("empty"));

        auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
        expectEquals (trackMeta.getProperty (IDs::title).toString(), juce::String (""));
        expectEquals (trackMeta.getProperty (IDs::filePath).toString(), juce::String (""));

        beginTest ("Track Ejection - cannot eject while playing");
        ctx.mgr->loadTrack ("A", makeSampleMetadata());
        expect (ctx.mgr->setPlaybackStatus ("A", "playing"));
        expect (! ctx.mgr->canEjectTrack ("A"));
        expect (! ctx.mgr->ejectTrack ("A"));

        beginTest ("Track Ejection - can eject while paused");
        expect (ctx.mgr->setPlaybackStatus ("A", "paused"));
        expect (ctx.mgr->canEjectTrack ("A"));
        expect (ctx.mgr->ejectTrack ("A"));

        beginTest ("Track Ejection - eject invalid deck returns false");
        expect (! ctx.mgr->ejectTrack ("Z"));
    }

    // -----------------------------------------------------------------------
    void testActiveDeck()
    {
        beginTest ("Active Deck - first added deck becomes active");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("A"));

        beginTest ("Active Deck - setActiveDeck works");
        ctx.mgr->addDeck(); // B
        ctx.mgr->setActiveDeck ("B");
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("B"));

        beginTest ("Active Deck - setActiveDeck with invalid id does nothing");
        ctx.mgr->setActiveDeck ("Z");
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("B"));

        beginTest ("Active Deck - removing active deck switches to first remaining");
        expect (ctx.mgr->removeDeck ("B"));
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("A"));
    }

    // -----------------------------------------------------------------------
    void testMasterTempo()
    {
        beginTest ("Master Tempo - only one deck can be master");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        ctx.mgr->setMasterTempo ("A");
        expect (static_cast<bool> (ctx.mgr->getDeckState ("A").getProperty (IDs::isMasterTempo)));
        expect (! static_cast<bool> (ctx.mgr->getDeckState ("B").getProperty (IDs::isMasterTempo)));

        beginTest ("Master Tempo - setting new master clears old one");
        ctx.mgr->setMasterTempo ("B");
        expect (! static_cast<bool> (ctx.mgr->getDeckState ("A").getProperty (IDs::isMasterTempo)));
        expect (static_cast<bool> (ctx.mgr->getDeckState ("B").getProperty (IDs::isMasterTempo)));
    }

    // -----------------------------------------------------------------------
    void testLetterAssignment()
    {
        beginTest ("Letter Assignment - fills gaps after removal");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B
        ctx.mgr->addDeck(); // C

        // Remove B
        expect (ctx.mgr->removeDeck ("B"));
        expectEquals (ctx.mgr->getDeckCount(), 2);

        // Next add should fill gap => B
        auto newId = ctx.mgr->addDeck();
        expectEquals (newId, juce::String ("B"));

        beginTest ("Letter Assignment - fills earliest gap");
        // Now we have A, B, C. Remove A.
        expect (ctx.mgr->removeDeck ("A"));
        auto fillA = ctx.mgr->addDeck();
        expectEquals (fillA, juce::String ("A"));
    }

    // -----------------------------------------------------------------------
    void testAudioStateSync()
    {
        beginTest ("AudioStateSync - gain property syncs to atomic");
        TestContext ctx;
        ctx.mgr->addDeck(); // A

        auto* audioState = ctx.mgr->getAudioState ("A");
        expect (audioState != nullptr);

        // Default gain
        expectEquals (audioState->gain.load(), 1.0f);

        // Change gain via ValueTree
        auto deckTree = ctx.mgr->getDeckState ("A");
        deckTree.setProperty (IDs::gain, 0.5f, nullptr);
        expectEquals (audioState->gain.load(), 0.5f);

        beginTest ("AudioStateSync - playbackStatus syncs to atomic");
        ctx.mgr->loadTrack ("A", makeSampleMetadata());
        expectEquals (audioState->playbackStatus.load(), static_cast<int> (PlaybackStatusCode::stopped));

        ctx.mgr->setPlaybackStatus ("A", "playing");
        expectEquals (audioState->playbackStatus.load(), static_cast<int> (PlaybackStatusCode::playing));

        ctx.mgr->setPlaybackStatus ("A", "paused");
        expectEquals (audioState->playbackStatus.load(), static_cast<int> (PlaybackStatusCode::paused));

        beginTest ("AudioStateSync - quantize/slip/keyLock sync");
        deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);
        expect (audioState->quantizeEnabled.load());

        deckTree.setProperty (IDs::slipEnabled, true, nullptr);
        expect (audioState->slipEnabled.load());

        deckTree.setProperty (IDs::keyLockEnabled, true, nullptr);
        expect (audioState->keyLockEnabled.load());

        beginTest ("AudioStateSync - playhead position syncs");
        auto playhead = deckTree.getChildWithName (IDs::Playhead);
        playhead.setProperty (IDs::position, (int64_t) 12345, nullptr);
        expectEquals (audioState->playheadPosition.load(), (int64_t) 12345);

        beginTest ("AudioStateSync - temp cue position syncs");
        auto tempCue = deckTree.getChildWithName (IDs::TempCue);
        tempCue.setProperty (IDs::position, (int64_t) 99999, nullptr);
        expectEquals (audioState->tempCuePosition.load(), (int64_t) 99999);

        beginTest ("AudioStateSync - speedMultiplier syncs");
        deckTree.setProperty (IDs::speedMultiplier, 1.5f, nullptr);
        expectEquals (audioState->speedMultiplier.load(), 1.5f);

        beginTest ("AudioStateSync - invalid deckId returns nullptr");
        expect (ctx.mgr->getAudioState ("Z") == nullptr);
    }

    // -----------------------------------------------------------------------
    void testSessionPersistence()
    {
        beginTest ("Session Persistence - save and restore round-trip");

        juce::File dbFile = juce::File::createTempFile ("sonik_session_test.db");
        {
            TrackDatabase db (dbFile);
            DeckStateManager mgr (db);

            mgr.addDeck(); // A
            mgr.addDeck(); // B
            mgr.addDeck(); // C
            mgr.setActiveDeck ("B");
            mgr.saveSession();
        }

        // Restore in a fresh manager with the same database
        {
            TrackDatabase db (dbFile);
            DeckStateManager mgr (db);

            mgr.restoreSession();
            expectEquals (mgr.getDeckCount(), 3);
            expectEquals (mgr.getActiveDeckId(), juce::String ("B"));
        }

        dbFile.deleteFile();
    }

    // -----------------------------------------------------------------------
    void testDatabaseOperations()
    {
        beginTest ("Database - save and load track data");
        juce::File dbFile = juce::File::createTempFile ("sonik_db_test.db");
        {
            TrackDatabase db (dbFile);

            db.saveTrackData ("/path/to/song.wav", "abc123",
                              "[{\"position\":1000}]", "{\"bpm\":128.0}",
                              5, 0.95f, false);

            auto result = db.loadTrackData ("/path/to/song.wav", "abc123");
            expect (result.has_value());
            expectEquals (result->keyIndex, 5);
            expect (result->keyConfidence > 0.94f && result->keyConfidence < 0.96f);
            expect (! result->keyManuallyAdjusted);
            expectEquals (result->cuePointsJson, juce::String ("[{\"position\":1000}]"));
            expectEquals (result->beatgridJson, juce::String ("{\"bpm\":128.0}"));
        }

        beginTest ("Database - load non-existent track returns nullopt");
        {
            TrackDatabase db (dbFile);
            auto result = db.loadTrackData ("/nonexistent.wav", "nohash");
            expect (! result.has_value());
        }

        beginTest ("Database - save and load session state");
        {
            TrackDatabase db (dbFile);
            db.saveSessionState (3, "B", "[{\"deckId\":\"A\"}]");

            auto session = db.loadSessionState();
            expectEquals (session.deckCount, 3);
            expectEquals (session.activeDeckId, juce::String ("B"));
            expectEquals (session.loadedTracksJson, juce::String ("[{\"deckId\":\"A\"}]"));
        }

        beginTest ("Database - update track data overwrites previous");
        {
            TrackDatabase db (dbFile);

            db.saveTrackData ("/path/to/song.wav", "abc123",
                              "[{\"position\":2000}]", "{\"bpm\":140.0}",
                              7, 0.8f, true);

            auto result = db.loadTrackData ("/path/to/song.wav", "abc123");
            expect (result.has_value());
            expectEquals (result->keyIndex, 7);
            expect (result->keyManuallyAdjusted);
            expectEquals (result->cuePointsJson, juce::String ("[{\"position\":2000}]"));
        }

        dbFile.deleteFile();
    }
};

static DeckStateTests deckStateTests;
