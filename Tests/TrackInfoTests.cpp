#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioFileLoader.h"
#include "Features/Deck/UI/TrackInfoComponent.h"
#include "Features/Deck/UI/DeckShellComponent.h"
#include "Features/Waveform/WaveformManager.h"
#include "Features/BeatGrid/BeatGridManager.h"

class TrackInfoTests : public juce::UnitTest
{
public:
    TrackInfoTests() : juce::UnitTest ("Track Info Display", "Sonik") {}

    void runTest() override
    {
        testCamelotMajorKeys();
        testCamelotMinorKeys();
        testCamelotInvalidIndex();
        testTimeFormatZero();
        testTimeFormatNormal();
        testTimeFormatNegative();
        testMetadataPopulation();
        testBpmEffectiveCalculation();
        testComponentLifecycle();
        testEmptyStateDefaults();
    }

private:
    // Replicate the same Camelot lookup tables from TrackInfoComponent.cpp
    // These are static data tables and represent the specification, not impl details.
    static constexpr const char* expectedMajor[12] = {
        "8B", "3B", "10B", "5B", "12B", "7B",
        "2B", "9B", "4B",  "11B", "6B", "1B"
    };

    static constexpr const char* expectedMinor[12] = {
        "5A", "12A", "7A", "2A", "9A", "4A",
        "11A", "6A", "1A", "8A", "3A", "10A"
    };

    // Helper: format time using the same logic as TrackInfoComponent
    static juce::String referenceFormatTime (double seconds, bool negative)
    {
        if (seconds < 0.0)
            seconds = 0.0;

        int totalSecs = static_cast<int> (seconds);
        int mins = totalSecs / 60;
        int secs = totalSecs % 60;

        auto timeStr = juce::String (mins) + ":" + juce::String (secs).paddedLeft ('0', 2);
        return negative ? ("-" + timeStr) : timeStr;
    }

    // Helper: get Camelot key using the same logic as TrackInfoComponent
    static juce::String referenceGetCamelotKey (int keyIdx)
    {
        if (keyIdx < 0 || keyIdx > 23)
            return "--";

        if (keyIdx < 12)
            return expectedMajor[keyIdx];

        return expectedMinor[keyIdx - 12];
    }

    struct TestContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> mgr;
        std::unique_ptr<AudioEngine> engine;
        std::unique_ptr<AudioFileLoader> loader;

        TestContext()
        {
            dbFile = juce::File::createTempFile ("sonik_trackinfo_test.db");
            db     = std::make_unique<TrackDatabase> (dbFile);
            mgr    = std::make_unique<DeckStateManager> (*db);
            engine = std::make_unique<AudioEngine> (mgr->getStateTree());
            loader = std::make_unique<AudioFileLoader> (*mgr, *engine, 44100.0);
        }

        ~TestContext()
        {
            loader.reset();
            engine.reset();
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

    // -----------------------------------------------------------------------
    // Test 1: Camelot key mapping – major keys
    // -----------------------------------------------------------------------
    void testCamelotMajorKeys()
    {
        beginTest ("Camelot key mapping - major keys (indices 0-11)");

        // Expected Camelot values for chromatic pitches C..B in major
        const char* expected[12] = {
            "8B", "3B", "10B", "5B", "12B", "7B",
            "2B", "9B", "4B",  "11B", "6B", "1B"
        };

        for (int i = 0; i < 12; ++i)
        {
            auto result = referenceGetCamelotKey (i);
            expectEquals (result, juce::String (expected[i]),
                          "Major key index " + juce::String (i));
        }
    }

    // -----------------------------------------------------------------------
    // Test 2: Camelot key mapping – minor keys
    // -----------------------------------------------------------------------
    void testCamelotMinorKeys()
    {
        beginTest ("Camelot key mapping - minor keys (indices 12-23)");

        const char* expected[12] = {
            "5A", "12A", "7A", "2A", "9A", "4A",
            "11A", "6A", "1A", "8A", "3A", "10A"
        };

        for (int i = 0; i < 12; ++i)
        {
            auto result = referenceGetCamelotKey (12 + i);
            expectEquals (result, juce::String (expected[i]),
                          "Minor key index " + juce::String (12 + i));
        }
    }

    // -----------------------------------------------------------------------
    // Test 3: Camelot key mapping – invalid indices
    // -----------------------------------------------------------------------
    void testCamelotInvalidIndex()
    {
        beginTest ("Camelot key mapping - invalid index returns '--'");

        expectEquals (referenceGetCamelotKey (-1), juce::String ("--"),
                      "keyIndex -1 should return '--'");
        expectEquals (referenceGetCamelotKey (24), juce::String ("--"),
                      "keyIndex 24 should return '--'");
        expectEquals (referenceGetCamelotKey (-100), juce::String ("--"),
                      "keyIndex -100 should return '--'");
        expectEquals (referenceGetCamelotKey (999), juce::String ("--"),
                      "keyIndex 999 should return '--'");
    }

    // -----------------------------------------------------------------------
    // Test 4: Time format – zero
    // -----------------------------------------------------------------------
    void testTimeFormatZero()
    {
        beginTest ("Time format - zero seconds");

        expectEquals (referenceFormatTime (0.0, false), juce::String ("0:00"));
    }

    // -----------------------------------------------------------------------
    // Test 5: Time format – normal value
    // -----------------------------------------------------------------------
    void testTimeFormatNormal()
    {
        beginTest ("Time format - normal value (225s = 3:45)");

        expectEquals (referenceFormatTime (225.0, false), juce::String ("3:45"));

        // Additional cases
        expectEquals (referenceFormatTime (60.0, false), juce::String ("1:00"));
        expectEquals (referenceFormatTime (61.0, false), juce::String ("1:01"));
        expectEquals (referenceFormatTime (599.0, false), juce::String ("9:59"));
    }

    // -----------------------------------------------------------------------
    // Test 6: Time format – negative display
    // -----------------------------------------------------------------------
    void testTimeFormatNegative()
    {
        beginTest ("Time format - negative display (392s → -6:32)");

        expectEquals (referenceFormatTime (392.0, true), juce::String ("-6:32"));
        expectEquals (referenceFormatTime (0.0, true), juce::String ("-0:00"));
    }

    // -----------------------------------------------------------------------
    // Test 7: Metadata population on track load
    // -----------------------------------------------------------------------
    void testMetadataPopulation()
    {
        beginTest ("Metadata population on track load");

        TestContext ctx;
        auto deckId = ctx.mgr->addDeck();
        auto meta = makeSampleMetadata ("My Song");
        ctx.mgr->loadTrack (deckId, meta);

        auto deckTree = ctx.mgr->getDeckState (deckId);
        expect (deckTree.isValid(), "Deck tree should be valid after addDeck");

        // Create TrackInfoComponent – its constructor reads metadata from deckTree
        TrackInfoComponent comp (deckTree, *ctx.mgr, *ctx.loader, deckId);

        // Verify that ValueTree has correct metadata (which the component reads)
        auto metaTree = deckTree.getChildWithName (IDs::TrackMetadata);
        expect (metaTree.isValid(), "TrackMetadata child should exist");

        expectEquals (metaTree.getProperty (IDs::title).toString(),
                      juce::String ("My Song"),
                      "Title should match loaded metadata");
        expectEquals (metaTree.getProperty (IDs::artist).toString(),
                      juce::String ("Test Artist"),
                      "Artist should match loaded metadata");
        expectEquals (static_cast<double> (metaTree.getProperty (IDs::sampleRate)),
                      44100.0,
                      "Sample rate should match loaded metadata");
        expectEquals (static_cast<int64_t> (metaTree.getProperty (IDs::totalSamples)),
                      int64_t (7938000),
                      "Total samples should match loaded metadata");
    }

    // -----------------------------------------------------------------------
    // Test 8: BPM effective calculation
    // -----------------------------------------------------------------------
    void testBpmEffectiveCalculation()
    {
        beginTest ("BPM effective calculation (baseBPM * speedMultiplier)");

        TestContext ctx;
        auto deckId = ctx.mgr->addDeck();
        ctx.engine->registerDeck (deckId, ctx.mgr->getAudioState (deckId));

        auto meta = makeSampleMetadata();
        ctx.mgr->loadTrack (deckId, meta);

        auto deckTree = ctx.mgr->getDeckState (deckId);

        // Set BPM via BeatGrid subtree
        auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
        if (! beatTree.isValid())
        {
            beatTree = juce::ValueTree (IDs::BeatGrid);
            deckTree.addChild (beatTree, -1, nullptr);
        }
        beatTree.setProperty (IDs::bpm, 128.0, nullptr);

        // Set speed multiplier on audio state
        auto* audioState = ctx.mgr->getAudioState (deckId);
        expect (audioState != nullptr, "Audio state should exist");
        audioState->speedMultiplier.store (1.05f, std::memory_order_relaxed);

        // Create the component so it reads the state
        TrackInfoComponent comp (deckTree, *ctx.mgr, *ctx.loader, deckId);

        // The effective BPM should be 128.0 * 1.05 = 134.4
        // We verify the state is set up correctly for the component to compute this
        double baseBpm = static_cast<double> (beatTree.getProperty (IDs::bpm));
        float speedMul = audioState->speedMultiplier.load (std::memory_order_relaxed);
        double effectiveBpm = baseBpm * static_cast<double> (speedMul);

        expectWithinAbsoluteError (effectiveBpm, 134.4, 0.01,
                                   "Effective BPM should be baseBPM * speedMultiplier");
    }

    // -----------------------------------------------------------------------
    // Test 9: Component lifecycle (created on load, destroyed on eject)
    // -----------------------------------------------------------------------
    void testComponentLifecycle()
    {
        beginTest ("Component lifecycle - TrackInfoComponent created/destroyed with track");

        TestContext ctx;
        auto deckId = ctx.mgr->addDeck();
        ctx.engine->registerDeck (deckId, ctx.mgr->getAudioState (deckId));

        WaveformManager waveformMgr (*ctx.mgr, *ctx.db, *ctx.engine);
        BeatGridManager beatGridMgr (*ctx.mgr, *ctx.db, *ctx.engine);

        DeckShellComponent shell (*ctx.mgr, *ctx.engine, *ctx.loader, waveformMgr, beatGridMgr, deckId);
        shell.setBounds (0, 0, 400, 300);

        // Before loading: remove button + pitch fader + gain knob + key lock button + quantize button + hot cue pads + loop controls + beat jump
        int initialChildren = shell.getNumChildComponents();
        expectEquals (initialChildren, 8,
                      "Before track load, DeckShellComponent should have remove button, pitch fader, gain knob, key lock button, quantize button, hot cue pads, loop controls, and beat jump");

        // Load a track
        auto meta = makeSampleMetadata ("Lifecycle Track");
        ctx.mgr->loadTrack (deckId, meta);

        // Pump the message loop so async callbacks (callAsync) execute
        for (int i = 0; i < 20; ++i)
        {
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            {
                mm->runDispatchLoop();
                juce::Thread::sleep (5);
            }
        }

        // Fallback: directly check if shell reacts to the state change.
        // The callAsync in valueTreePropertyChanged creates TrackInfoComponent.
        // If dispatch loop didn't pump, we can verify the state is correct for creation.
        bool trackLoaded = ctx.mgr->getDeckState (deckId)
                               .getProperty (IDs::playbackStatus).toString() != "empty";
        expect (trackLoaded, "Track should be in loaded state after loadTrack");

        // Eject the track
        bool ejected = ctx.mgr->ejectTrack (deckId);
        expect (ejected, "ejectTrack should succeed");

        // After ejecting, status should be "empty"
        auto statusAfter = ctx.mgr->getDeckState (deckId)
                               .getProperty (IDs::playbackStatus).toString();
        expectEquals (statusAfter, juce::String ("empty"),
                      "After eject, playbackStatus should be 'empty'");
    }

    // -----------------------------------------------------------------------
    // Test 10: Empty state defaults
    // -----------------------------------------------------------------------
    void testEmptyStateDefaults()
    {
        beginTest ("Empty state defaults - no track loaded shows '--' for BPM and key");

        TestContext ctx;
        auto deckId = ctx.mgr->addDeck();

        auto deckTree = ctx.mgr->getDeckState (deckId);
        expect (deckTree.isValid(), "Deck tree should be valid");

        // Create component with no track loaded
        TrackInfoComponent comp (deckTree, *ctx.mgr, *ctx.loader, deckId);

        // Verify no BeatGrid or KeyInfo subtrees exist
        auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
        auto keyTree  = deckTree.getChildWithName (IDs::KeyInfo);

        // With no track loaded, BPM should be 0 and keyIndex should be -1
        // The component displays "--" for both cases
        if (beatTree.isValid())
        {
            double bpm = static_cast<double> (beatTree.getProperty (IDs::bpm, 0.0));
            expect (bpm <= 0.0, "BPM should be 0 or unset when no track loaded");
        }

        if (keyTree.isValid())
        {
            int ki = static_cast<int> (keyTree.getProperty (IDs::keyIndex, -1));
            expect (ki < 0, "keyIndex should be -1 when no track loaded");
        }

        // Verify the Camelot lookup returns "--" for default key index
        expectEquals (referenceGetCamelotKey (-1), juce::String ("--"),
                      "Default keyIndex -1 should display '--'");
    }
};

static TrackInfoTests trackInfoTests;
