#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/Sync/SyncEngine.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockSnapshot.h"
#include "Features/Sync/MasterClockManager.h"
#include "Features/Sync/UI/SyncButton.h"
#include "Features/Sync/UI/MasterButton.h"
#include "Features/Deck/AudioThreadState.h"   // DeckAudioState + AudioStateSync
#include "Features/Deck/DeckIdentifiers.h"

class SyncEngineTests : public juce::UnitTest
{
public:
    SyncEngineTests() : juce::UnitTest ("Sync Engine", "Sonik") {}

    void runTest() override
    {
        // SyncEngine logic
        testNoOpWhenNotSynced();
        testNoOpWhenMasterNotPlaying();
        testNoOpWhenMasterBpmZero();
        testNoOpWhenDeckBpmZero();
        testNormalSync();
        testSameBpmNoChange();

        // BPM 2:1 normalisation
        testDoubleTimePrevention();
        testHalfTimePrevention();
        testFoldUpperBoundary();
        testFoldJustAboveUpperBoundary();
        testFoldLowerBoundary();
        testFoldJustBelowLowerBoundary();

        // AudioStateSync / atomic propagation
        testIsSyncedPropagates();
        testDeckBpmPropagates();

        // UI buttons
        testSyncButtonConstructsWithoutCrash();
        testSyncButtonPaintsWithoutCrash();
        testSyncButtonReflectsIsSyncedState();
        testMasterButtonConstructsWithoutCrash();
        testMasterButtonPaintsWithoutCrash();
        testMasterButtonReflectsIsMasterState();
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    static MasterClockSnapshot makeSnapshot (double bpm, bool isPlaying, int64_t phase = 0)
    {
        MasterClockSnapshot snap;
        snap.masterBPM               = bpm;
        snap.masterNativeBPM         = bpm;
        snap.masterIsPlaying         = isPlaying;
        snap.masterPhaseOriginSample = phase;
        return snap;
    }

    /// Minimal root SonikState with numDecks Deck children (mirrors MasterClockTests pattern).
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
            beatGrid.setProperty (IDs::bpm,          0.0,          nullptr);
            beatGrid.setProperty (IDs::anchorSample, (int64_t) 0,  nullptr);
            deck.addChild (beatGrid, -1, nullptr);

            decks.addChild (deck, -1, nullptr);
        }
        root.addChild (decks, -1, nullptr);
        return root;
    }

    static juce::ValueTree getDeck (const juce::ValueTree& root, int index)
    {
        return root.getChildWithName (IDs::Decks).getChild (index);
    }

    // -----------------------------------------------------------------------
    // SyncEngine Logic Tests
    // -----------------------------------------------------------------------

    void testNoOpWhenNotSynced()
    {
        beginTest ("SyncEngine - isSynced=false: speedMultiplier unchanged");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (false,   std::memory_order_relaxed);
        state.deckBPM.store         (130.0,   std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,    std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testNoOpWhenMasterNotPlaying()
    {
        beginTest ("SyncEngine - masterIsPlaying=false: speedMultiplier unchanged");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, false));  // masterIsPlaying = false
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (130.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testNoOpWhenMasterBpmZero()
    {
        beginTest ("SyncEngine - masterBPM=0.0: no crash, speedMultiplier unchanged");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (0.0, true));  // masterBPM = 0.0
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (130.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testNoOpWhenDeckBpmZero()
    {
        beginTest ("SyncEngine - deckBPM=0.0: no crash, speedMultiplier unchanged");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true, std::memory_order_relaxed);
        state.deckBPM.store         (0.0,  std::memory_order_relaxed);  // deckBPM = 0
        state.speedMultiplier.store (1.0f, std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testNormalSync()
    {
        beginTest ("SyncEngine - Normal sync: masterBPM=128 deckBPM=130 -> ~0.9846");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (130.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        const float expected = static_cast<float> (128.0 / 130.0);
        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   expected, 0.0001f);
    }

    void testSameBpmNoChange()
    {
        beginTest ("SyncEngine - Exact same BPM: speedMultiplier = 1.0");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (128.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    // -----------------------------------------------------------------------
    // BPM Normalisation Tests  (2:1 fold into [0.667, 1.5])
    // -----------------------------------------------------------------------

    void testDoubleTimePrevention()
    {
        // masterBPM=128, deckBPM=64  → ratio=2.0  → fold: 2.0*0.5=1.0
        beginTest ("SyncEngine BPM fold - Double-time: ratio=2.0 → 1.0");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (128.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true, std::memory_order_relaxed);
        state.deckBPM.store         (64.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f, std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testHalfTimePrevention()
    {
        // masterBPM=64, deckBPM=128  → ratio=0.5  → fold: 0.5*2.0=1.0
        beginTest ("SyncEngine BPM fold - Half-time: ratio=0.5 → 1.0");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (64.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (128.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.0f, 0.0001f);
    }

    void testFoldUpperBoundary()
    {
        // masterBPM=150, deckBPM=100  → ratio=1.5  → exactly on boundary, no fold
        beginTest ("SyncEngine BPM fold - Upper boundary: ratio=1.5 → 1.5 (no fold)");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (150.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (100.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   1.5f, 0.0001f);
    }

    void testFoldJustAboveUpperBoundary()
    {
        // masterBPM=151, deckBPM=100  → ratio=1.51  → fold: 1.51*0.5=0.755
        beginTest ("SyncEngine BPM fold - Just above 1.5: ratio=1.51 → 0.755");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (151.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (100.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        const float expected = static_cast<float> (1.51 * 0.5);  // 0.755
        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   expected, 0.0001f);
    }

    void testFoldLowerBoundary()
    {
        // masterBPM=67, deckBPM=100  → ratio=0.67  → 0.67 >= 0.667, no fold
        beginTest ("SyncEngine BPM fold - Lower boundary: ratio=0.67 → 0.67 (no fold)");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (67.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (100.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        const float expected = static_cast<float> (67.0 / 100.0);  // 0.67
        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   expected, 0.0001f);
    }

    void testFoldJustBelowLowerBoundary()
    {
        // masterBPM=66, deckBPM=100  → ratio=0.66  → fold: 0.66*2.0=1.32
        beginTest ("SyncEngine BPM fold - Just below 0.667: ratio=0.66 → 1.32");

        MasterClockPublisher pub;
        pub.publish (makeSnapshot (66.0, true));
        SyncEngine engine (pub);

        DeckAudioState state;
        state.isSynced.store        (true,  std::memory_order_relaxed);
        state.deckBPM.store         (100.0, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f,  std::memory_order_relaxed);

        engine.process (state);

        const float expected = static_cast<float> (0.66 * 2.0);  // 1.32
        expectWithinAbsoluteError (state.speedMultiplier.load (std::memory_order_relaxed),
                                   expected, 0.0001f);
    }

    // -----------------------------------------------------------------------
    // AudioStateSync / Atomic Propagation Tests
    // -----------------------------------------------------------------------

    void testIsSyncedPropagates()
    {
        beginTest ("AudioStateSync - isSynced propagates to DeckAudioState atomic");

        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::isSynced, false, nullptr);

        DeckAudioState state;
        AudioStateSync sync (deckTree, state);

        expect (! state.isSynced.load (std::memory_order_relaxed));

        // Setting the property fires valueTreePropertyChanged synchronously
        deckTree.setProperty (IDs::isSynced, true, nullptr);

        expect (state.isSynced.load (std::memory_order_relaxed));
    }

    void testDeckBpmPropagates()
    {
        beginTest ("AudioStateSync - deckBPM propagates from BeatGrid subtree");

        juce::ValueTree deckTree (IDs::Deck);
        juce::ValueTree beatGrid (IDs::BeatGrid);
        beatGrid.setProperty (IDs::bpm, 0.0, nullptr);
        deckTree.addChild (beatGrid, -1, nullptr);

        DeckAudioState state;
        AudioStateSync sync (deckTree, state);

        expectWithinAbsoluteError (state.deckBPM.load (std::memory_order_relaxed), 0.0, 0.0001);

        // Listener registered on deckTree covers the entire subtree
        beatGrid.setProperty (IDs::bpm, 130.0, nullptr);

        expectWithinAbsoluteError (state.deckBPM.load (std::memory_order_relaxed), 130.0, 0.0001);
    }

    // -----------------------------------------------------------------------
    // UI Button Tests
    // -----------------------------------------------------------------------

    void testSyncButtonConstructsWithoutCrash()
    {
        beginTest ("SyncButton - Constructs without crash");

        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::isSynced, false, nullptr);
        deckTree.setProperty (IDs::isMaster, false, nullptr);

        SyncButton btn (deckTree);
        expect (true);
    }

    void testSyncButtonPaintsWithoutCrash()
    {
        beginTest ("SyncButton - paintButton() does not crash");

        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::isSynced, false, nullptr);
        deckTree.setProperty (IDs::isMaster, false, nullptr);

        SyncButton btn (deckTree);
        btn.setBounds (0, 0, 60, 20);

        juce::Image    img (juce::Image::ARGB, 60, 20, true);
        juce::Graphics g (img);
        btn.paintButton (g, false, false);

        expect (true);
    }

    void testSyncButtonReflectsIsSyncedState()
    {
        beginTest ("SyncButton - isSynced=true → active fill (#2d2d2d), isSynced=false → inactive fill (#F9F9F9)");

        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::isSynced, false, nullptr);
        deckTree.setProperty (IDs::isMaster, false, nullptr);

        SyncButton btn (deckTree);
        btn.setBounds (0, 0, 60, 20);

        // Inactive state: fill should be #F9F9F9 — check interior pixel (1,1)
        {
            juce::Image    img (juce::Image::ARGB, 60, 20, true);
            juce::Graphics g (img);
            btn.paintButton (g, false, false);

            const auto px = img.getPixelAt (4, 4); // inside the 2-px ink border
            expect (px.getRed()   == (juce::uint8) 0xF9);
            expect (px.getGreen() == (juce::uint8) 0xF9);
            expect (px.getBlue()  == (juce::uint8) 0xF9);
        }

        // Setting isSynced fires valueTreePropertyChanged synchronously → isSynced_ updated
        deckTree.setProperty (IDs::isSynced, true, nullptr);

        // Active state: fill should be #000000
        {
            juce::Image    img (juce::Image::ARGB, 60, 20, true);
            juce::Graphics g (img);
            btn.paintButton (g, false, false);

            const auto px = img.getPixelAt (4, 4); // inside the 2-px ink border
            expect (px.getRed()   == (juce::uint8) 0x2D);
            expect (px.getGreen() == (juce::uint8) 0x2D);
            expect (px.getBlue()  == (juce::uint8) 0x2D);
        }
    }

    void testMasterButtonConstructsWithoutCrash()
    {
        beginTest ("MasterButton - Constructs without crash");

        auto root = createRootState (1);
        MasterClockPublisher pub;
        MasterClockManager manager (root, pub);

        auto deckTree = getDeck (root, 0);
        MasterButton btn (deckTree, manager, 0);
        expect (true);
    }

    void testMasterButtonPaintsWithoutCrash()
    {
        beginTest ("MasterButton - paintButton() does not crash");

        auto root = createRootState (1);
        MasterClockPublisher pub;
        MasterClockManager manager (root, pub);

        auto deckTree = getDeck (root, 0);
        MasterButton btn (deckTree, manager, 0);
        btn.setBounds (0, 0, 80, 20);

        juce::Image    img (juce::Image::ARGB, 80, 20, true);
        juce::Graphics g (img);
        btn.paintButton (g, false, false);

        expect (true);
    }

    void testMasterButtonReflectsIsMasterState()
    {
        beginTest ("MasterButton - isMaster=true → active fill (#2d2d2d), isMaster=false → inactive fill (#F9F9F9)");

        auto root = createRootState (1);
        MasterClockPublisher pub;
        MasterClockManager manager (root, pub);

        auto deckTree = getDeck (root, 0);
        MasterButton btn (deckTree, manager, 0);
        btn.setBounds (0, 0, 80, 20);

        // Inactive: fill should be #F9F9F9
        {
            juce::Image    img (juce::Image::ARGB, 80, 20, true);
            juce::Graphics g (img);
            btn.paintButton (g, false, false);

            const auto px = img.getPixelAt (4, 4); // inside the 2-px ink border
            expect (px.getRed()   == (juce::uint8) 0xF9);
            expect (px.getGreen() == (juce::uint8) 0xF9);
            expect (px.getBlue()  == (juce::uint8) 0xF9);
        }

        // Setting isMaster fires valueTreePropertyChanged synchronously → isMaster_ updated
        deckTree.setProperty (IDs::isMaster, true, nullptr);

        // Active: fill should be #000000
        {
            juce::Image    img (juce::Image::ARGB, 80, 20, true);
            juce::Graphics g (img);
            btn.paintButton (g, false, false);

            const auto px = img.getPixelAt (4, 4); // inside the 2-px ink border
            expect (px.getRed()   == (juce::uint8) 0x2D);
            expect (px.getGreen() == (juce::uint8) 0x2D);
            expect (px.getBlue()  == (juce::uint8) 0x2D);
        }
    }
};

static SyncEngineTests syncEngineTests;
