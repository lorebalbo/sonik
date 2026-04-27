#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Sync/MasterClockSnapshot.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockManager.h"
#include "Features/Deck/DeckIdentifiers.h"

class MasterClockTests : public juce::UnitTest
{
public:
    MasterClockTests() : juce::UnitTest ("Master Clock Core", "Sonik") {}

    void runTest() override
    {
        testPublisherConstruction();
        testPublisherDefaultRead();
        testPublisherSinglePublish();
        testPublisherMultiplePublishes();
        testPublisherCoherentSnapshot();
        testIDisMasterDefined();
        testIDisSyncedDefined();
        testIDmasterDeckIndexDefined();
        testDeckDefaultIsMasterAndIsSynced();
        testRootDefaultMasterDeckIndex();
        testAutoPromoteOnStopped();
        testAutoPromoteToLowestPlayingOnMasterStop();
        testDormantStateWhenNoPlayingDeck();
        testPauseMasterPublishesIsPlayingFalse();
        testSetMasterDemotesAndPromotes();
        testSetMasterNoOpWhenAlreadyPlayingMaster();
        testAtMostOneMasterAtAnyTime();
        testSetMasterClearsSyncedFlag();
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Build a minimal SonikState ValueTree with `numDecks` Deck children.
    /// Each deck has playbackStatus="empty", isMaster=false, isSynced=false,
    /// and a BeatGrid child with bpm=0 and anchorSample=0.
    static juce::ValueTree createRootState (int numDecks)
    {
        juce::ValueTree root (IDs::SonikState);
        root.setProperty (IDs::masterDeckIndex, -1, nullptr);

        juce::ValueTree decks (IDs::Decks);
        for (int i = 0; i < numDecks; ++i)
        {
            juce::ValueTree deck (IDs::Deck);
            deck.setProperty (IDs::playbackStatus, "empty", nullptr);
            deck.setProperty (IDs::isMaster,       false,   nullptr);
            deck.setProperty (IDs::isSynced,       false,   nullptr);

            juce::ValueTree beatGrid (IDs::BeatGrid);
            beatGrid.setProperty (IDs::bpm,          0.0,         nullptr);
            beatGrid.setProperty (IDs::anchorSample, (int64_t) 0, nullptr);
            deck.addChild (beatGrid, -1, nullptr);

            decks.addChild (deck, -1, nullptr);
        }
        root.addChild (decks, -1, nullptr);
        return root;
    }

    /// Return the deck ValueTree at the given index inside the root's Decks child.
    static juce::ValueTree getDeck (const juce::ValueTree& root, int index)
    {
        return root.getChildWithName (IDs::Decks).getChild (index);
    }

    /// Count how many deck children currently have isMaster=true.
    static int countMasterDecks (const juce::ValueTree& root)
    {
        auto decks = root.getChildWithName (IDs::Decks);
        int count = 0;
        for (int i = 0; i < decks.getNumChildren(); ++i)
            if (static_cast<bool> (decks.getChild (i).getProperty (IDs::isMaster)))
                ++count;
        return count;
    }

    // -----------------------------------------------------------------------
    // SeqLock Tests
    // -----------------------------------------------------------------------

    void testPublisherConstruction()
    {
        beginTest ("SeqLock - Publisher constructs without crash");
        MasterClockPublisher pub;
        expect (true); // reaching here means construction succeeded
    }

    void testPublisherDefaultRead()
    {
        beginTest ("SeqLock - Fresh publisher read() returns default snapshot");
        MasterClockPublisher pub;
        auto snap = pub.read();
        expectEquals (snap.masterBPM,               0.0);
        expectEquals (snap.masterPhaseOriginSample, (int64_t) 0);
        expect       (! snap.masterIsPlaying);
    }

    void testPublisherSinglePublish()
    {
        beginTest ("SeqLock - publish({128.0, 1000, true}) reflected by read()");
        MasterClockPublisher pub;
        MasterClockSnapshot snap;
        snap.masterBPM               = 128.0;
        snap.masterPhaseOriginSample = 1000;
        snap.masterIsPlaying         = true;
        pub.publish (snap);

        auto result = pub.read();
        expectEquals (result.masterBPM,               128.0);
        expectEquals (result.masterPhaseOriginSample, (int64_t) 1000);
        expect       (result.masterIsPlaying);
    }

    void testPublisherMultiplePublishes()
    {
        beginTest ("SeqLock - Multiple successive publishes: read() always returns latest");
        MasterClockPublisher pub;

        const double bpms[] = { 90.0, 120.0, 140.0, 175.0 };
        for (double bpm : bpms)
        {
            MasterClockSnapshot snap;
            snap.masterBPM               = bpm;
            snap.masterPhaseOriginSample = static_cast<int64_t> (bpm * 100.0);
            snap.masterIsPlaying         = (bpm > 100.0);
            pub.publish (snap);

            auto result = pub.read();
            expectEquals (result.masterBPM,               bpm);
            expectEquals (result.masterPhaseOriginSample, static_cast<int64_t> (bpm * 100.0));
            expect       (result.masterIsPlaying == (bpm > 100.0));
        }
    }

    void testPublisherCoherentSnapshot()
    {
        beginTest ("SeqLock - read() returns coherent snapshot: no mixing of fields from different publishes");
        MasterClockPublisher pub;

        // Publish snapshot A
        MasterClockSnapshot snapA;
        snapA.masterBPM               = 128.0;
        snapA.masterPhaseOriginSample = 44100;
        snapA.masterIsPlaying         = true;
        pub.publish (snapA);

        // Publish snapshot B immediately after
        MasterClockSnapshot snapB;
        snapB.masterBPM               = 96.0;
        snapB.masterPhaseOriginSample = 88200;
        snapB.masterIsPlaying         = false;
        pub.publish (snapB);

        // Must return exactly B — no field from A should appear
        auto result = pub.read();
        expectEquals (result.masterBPM,               96.0);
        expectEquals (result.masterPhaseOriginSample, (int64_t) 88200);
        expect       (! result.masterIsPlaying);
    }

    // -----------------------------------------------------------------------
    // ValueTree ID Tests
    // -----------------------------------------------------------------------

    void testIDisMasterDefined()
    {
        beginTest ("ValueTree IDs - IDs::isMaster is defined and non-empty");
        juce::Identifier id = IDs::isMaster;
        expect       (id.toString().isNotEmpty());
        expectEquals (id.toString(), juce::String ("isMaster"));
    }

    void testIDisSyncedDefined()
    {
        beginTest ("ValueTree IDs - IDs::isSynced is defined and non-empty");
        juce::Identifier id = IDs::isSynced;
        expect       (id.toString().isNotEmpty());
        expectEquals (id.toString(), juce::String ("isSynced"));
    }

    void testIDmasterDeckIndexDefined()
    {
        beginTest ("ValueTree IDs - IDs::masterDeckIndex is defined and non-empty");
        juce::Identifier id = IDs::masterDeckIndex;
        expect       (id.toString().isNotEmpty());
        expectEquals (id.toString(), juce::String ("masterDeckIndex"));
    }

    void testDeckDefaultIsMasterAndIsSynced()
    {
        beginTest ("ValueTree IDs - Fresh deck ValueTree has isMaster=false and isSynced=false");
        auto root = createRootState (1);
        auto deck = getDeck (root, 0);
        // Use a "default true" fallback to catch a missing property as a failure
        expect (! static_cast<bool> (deck.getProperty (IDs::isMaster, true)));
        expect (! static_cast<bool> (deck.getProperty (IDs::isSynced, true)));
    }

    void testRootDefaultMasterDeckIndex()
    {
        beginTest ("ValueTree IDs - Root state has masterDeckIndex=-1 by default");
        auto root = createRootState (2);
        expectEquals (static_cast<int> (root.getProperty (IDs::masterDeckIndex, 0)), -1);
    }

    // -----------------------------------------------------------------------
    // MasterClockManager State Machine Tests
    //
    // Note: MasterClockManager registers a juce::ValueTree::Listener. JUCE fires
    // ValueTree listener callbacks synchronously on the thread that calls setProperty,
    // so no message-thread pumping is required here.
    // -----------------------------------------------------------------------

    void testAutoPromoteOnStopped()
    {
        beginTest ("MasterClockManager - No master assigned: deck transitioning to stopped is auto-promoted");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        expectEquals (mgr.getMasterDeckIndex(), -1);

        // Deck 0 transitions to "stopped" — triggers auto-promotion (no master yet)
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);

        expectEquals (mgr.getMasterDeckIndex(), 0);
        expect       (static_cast<bool>  (getDeck (root, 0).getProperty (IDs::isMaster)));
        expect       (! static_cast<bool> (getDeck (root, 1).getProperty (IDs::isMaster)));
    }

    void testAutoPromoteToLowestPlayingOnMasterStop()
    {
        beginTest ("MasterClockManager - Master stops while another deck is playing: lowest-index playing deck is promoted");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Promote deck 0 via auto-promotion, then put it in "playing" state
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);  // auto-promotes
        getDeck (root, 0).setProperty (IDs::playbackStatus, "playing", nullptr);  // master publishes isPlaying=true
        // Deck 1 starts playing (MasterClockManager ignores this — deck 0 is master)
        getDeck (root, 1).setProperty (IDs::playbackStatus, "playing", nullptr);

        expectEquals (mgr.getMasterDeckIndex(), 0);

        // Master (deck 0) stops — deck 1 is playing, so it should be promoted
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);

        expectEquals (mgr.getMasterDeckIndex(), 1);
        expect       (! static_cast<bool> (getDeck (root, 0).getProperty (IDs::isMaster)));
        expect       (static_cast<bool>   (getDeck (root, 1).getProperty (IDs::isMaster)));
    }

    void testDormantStateWhenNoPlayingDeck()
    {
        beginTest ("MasterClockManager - Master stops and no other deck is playing: enters dormant state");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Promote deck 0, make it play
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);
        getDeck (root, 0).setProperty (IDs::playbackStatus, "playing", nullptr);

        // Deck 0 stops — no other deck is playing
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);

        expectEquals (mgr.getMasterDeckIndex(), -1);
        expect (! pub.read().masterIsPlaying);
    }

    void testPauseMasterPublishesIsPlayingFalse()
    {
        beginTest ("MasterClockManager - Master deck paused: publishes masterIsPlaying=false with BPM retained");
        auto root = createRootState (1);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Promote deck 0 and start playing
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);
        getDeck (root, 0).setProperty (IDs::playbackStatus, "playing", nullptr);

        // Set BPM on the BeatGrid so it is published and retained
        getDeck (root, 0).getChildWithName (IDs::BeatGrid)
                         .setProperty (IDs::bpm, 128.0, nullptr);

        // Pause the master deck
        getDeck (root, 0).setProperty (IDs::playbackStatus, "paused", nullptr);

        auto snap = pub.read();
        expect       (! snap.masterIsPlaying);
        expectEquals (snap.masterBPM, 128.0);
    }

    void testSetMasterDemotesAndPromotes()
    {
        beginTest ("MasterClockManager - setMaster(1) demotes current master (deck 0) and promotes deck 1");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Promote deck 0 via auto-promotion
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);
        expectEquals (mgr.getMasterDeckIndex(), 0);
        expect (static_cast<bool> (getDeck (root, 0).getProperty (IDs::isMaster)));

        // Manually promote deck 1
        mgr.setMaster (1);

        expectEquals (mgr.getMasterDeckIndex(), 1);
        expect (! static_cast<bool> (getDeck (root, 0).getProperty (IDs::isMaster)));
        expect (static_cast<bool>   (getDeck (root, 1).getProperty (IDs::isMaster)));
    }

    void testSetMasterNoOpWhenAlreadyPlayingMaster()
    {
        beginTest ("MasterClockManager - setMaster(0) is no-op when deck 0 is already the playing master");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Promote deck 0 and set it playing
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);
        getDeck (root, 0).setProperty (IDs::playbackStatus, "playing", nullptr);
        expectEquals (mgr.getMasterDeckIndex(), 0);

        // Call setMaster on the already-playing master — must be a no-op
        mgr.setMaster (0);

        // State must be unchanged
        expectEquals (mgr.getMasterDeckIndex(), 0);
        expect (static_cast<bool>  (getDeck (root, 0).getProperty (IDs::isMaster)));
        expect (! static_cast<bool> (getDeck (root, 1).getProperty (IDs::isMaster)));
    }

    void testAtMostOneMasterAtAnyTime()
    {
        beginTest ("MasterClockManager - At most one deck has isMaster=true after any transition");
        auto root = createRootState (3);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Before any activity: no masters
        expectEquals (countMasterDecks (root), 0);

        // Auto-promote deck 0
        getDeck (root, 0).setProperty (IDs::playbackStatus, "stopped", nullptr);
        expectEquals (countMasterDecks (root), 1);
        expectEquals (mgr.getMasterDeckIndex(), 0);

        // Manual promotion to deck 1
        mgr.setMaster (1);
        expectEquals (countMasterDecks (root), 1);
        expectEquals (mgr.getMasterDeckIndex(), 1);

        // Manual promotion to deck 2
        mgr.setMaster (2);
        expectEquals (countMasterDecks (root), 1);
        expectEquals (mgr.getMasterDeckIndex(), 2);

        // Deck 2 (master) stops → dormant (no other deck playing)
        getDeck (root, 2).setProperty (IDs::playbackStatus, "stopped", nullptr);
        // Deck 2 was set to master but its playbackStatus was "empty" (never set to playing).
        // Wait — deck 2's status was never changed, so when we call setProperty("stopped"),
        // the callback fires. But deck 2 has isMaster=true at this point (from setMaster(2)).
        // status=="stopped" → demote deck 2, findLowestPlayingDeckIndex()=-1 → publishDormant().
        expectEquals (countMasterDecks (root), 0);
        expectEquals (mgr.getMasterDeckIndex(), -1);
    }

    void testSetMasterClearsSyncedFlag()
    {
        beginTest ("MasterClockManager - setMaster on synced deck clears isSynced before promoting");
        auto root = createRootState (2);
        MasterClockPublisher pub;
        MasterClockManager mgr (root, pub);

        // Mark deck 1 as synced
        getDeck (root, 1).setProperty (IDs::isSynced, true, nullptr);
        expect (static_cast<bool> (getDeck (root, 1).getProperty (IDs::isSynced)));

        // Promote deck 1 as master
        mgr.setMaster (1);

        // isSynced must be cleared before/during promotion
        expect (! static_cast<bool> (getDeck (root, 1).getProperty (IDs::isSynced)));
        // Deck 1 must now be master
        expect (static_cast<bool>   (getDeck (root, 1).getProperty (IDs::isMaster)));
        expectEquals (mgr.getMasterDeckIndex(), 1);
    }
};

static MasterClockTests masterClockTests;
