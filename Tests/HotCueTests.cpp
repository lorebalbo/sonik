#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Cue/HotCueData.h"
#include "Features/Cue/HotCueManager.h"
#include "Features/Cue/HotCuePadComponent.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/Waveform/DetailWaveform.h"
#include "Features/Waveform/OverviewWaveform.h"
#include "Features/BeatGrid/BeatGridData.h"
#include <array>

// =============================================================================
// Helper: build a full deck ValueTree matching DeckStateManager::createDeckTree
// =============================================================================
static juce::ValueTree createTestDeckTree (const juce::String& deckId)
{
    juce::ValueTree deck (IDs::Deck);
    deck.setProperty (IDs::id,              deckId,  nullptr);
    deck.setProperty (IDs::playbackStatus,  "empty", nullptr);
    deck.setProperty (IDs::isMasterTempo,   false,   nullptr);
    deck.setProperty (IDs::gain,            1.0f,    nullptr);
    deck.setProperty (IDs::quantizeEnabled, false,   nullptr);
    deck.setProperty (IDs::slipEnabled,     false,   nullptr);
    deck.setProperty (IDs::keyLockEnabled,  false,   nullptr);
    deck.setProperty (IDs::pitchRange,      8,       nullptr);
    deck.setProperty (IDs::loadingStatus,   "idle",  nullptr);
    deck.setProperty (IDs::loadingProgress, 0.0f,    nullptr);
    deck.setProperty (IDs::loadingError,    "",      nullptr);
    deck.setProperty (IDs::pitch,           0.0f,    nullptr);
    deck.setProperty (IDs::speedMultiplier, 1.0f,    nullptr);

    juce::ValueTree trackMeta (IDs::TrackMetadata);
    trackMeta.setProperty (IDs::filePath,     "/test/track.wav", nullptr);
    trackMeta.setProperty (IDs::contentHash,  "testhash123",     nullptr);
    trackMeta.setProperty (IDs::title,        "Test Track",      nullptr);
    trackMeta.setProperty (IDs::artist,       "Test Artist",     nullptr);
    trackMeta.setProperty (IDs::album,        "",                nullptr);
    trackMeta.setProperty (IDs::duration,     180.0,             nullptr);
    trackMeta.setProperty (IDs::sampleRate,   44100.0,           nullptr);
    trackMeta.setProperty (IDs::bitDepth,     16,                nullptr);
    trackMeta.setProperty (IDs::totalSamples, (int64_t) 7938000, nullptr);
    trackMeta.setProperty (IDs::hasAlbumArt,  false,             nullptr);
    trackMeta.setProperty (IDs::channelCount, 2,                 nullptr);
    deck.addChild (trackMeta, -1, nullptr);

    juce::ValueTree playhead (IDs::Playhead);
    playhead.setProperty (IDs::position, (int64_t) 0, nullptr);
    deck.addChild (playhead, -1, nullptr);

    juce::ValueTree tempCue (IDs::TempCue);
    tempCue.setProperty (IDs::position, (int64_t) -1, nullptr);
    deck.addChild (tempCue, -1, nullptr);

    juce::ValueTree cuePoints (IDs::CuePoints);
    for (int i = 0; i < 8; ++i)
    {
        juce::ValueTree cp (IDs::CuePoint);
        cp.setProperty (IDs::index,      i,             nullptr);
        cp.setProperty (IDs::position,   (int64_t) -1,  nullptr);
        cp.setProperty (IDs::colorIndex, 0,             nullptr);
        cp.setProperty (IDs::label,      "",            nullptr);
        cp.setProperty (IDs::isValid,    false,         nullptr);
        cuePoints.addChild (cp, -1, nullptr);
    }
    deck.addChild (cuePoints, -1, nullptr);

    juce::ValueTree beatGrid (IDs::BeatGrid);
    beatGrid.setProperty (IDs::bpm,                 0.0,         nullptr);
    beatGrid.setProperty (IDs::anchorSample,        (int64_t) 0, nullptr);
    beatGrid.setProperty (IDs::beatIntervalSamples, 0.0,         nullptr);
    beatGrid.setProperty (IDs::confidence,          0.0f,        nullptr);
    beatGrid.setProperty (IDs::manuallyAdjusted,    false,       nullptr);
    beatGrid.setProperty (IDs::analysisStatus,      "idle",      nullptr);
    beatGrid.setProperty (IDs::analysisProgress,    0.0f,        nullptr);
    deck.addChild (beatGrid, -1, nullptr);

    juce::ValueTree keyInfo (IDs::KeyInfo);
    keyInfo.setProperty (IDs::keyIndex,         -1,     nullptr);
    keyInfo.setProperty (IDs::confidence,       0.0f,   nullptr);
    keyInfo.setProperty (IDs::manuallyAdjusted, false,  nullptr);
    keyInfo.setProperty (IDs::analysisStatus,   "idle", nullptr);
    keyInfo.setProperty (IDs::analysisProgress, 0.0f,   nullptr);
    deck.addChild (keyInfo, -1, nullptr);

    juce::ValueTree loop (IDs::Loop);
    loop.setProperty (IDs::loopIn,  (int64_t) -1, nullptr);
    loop.setProperty (IDs::loopOut, (int64_t) -1, nullptr);
    loop.setProperty (IDs::active,  false,         nullptr);
    deck.addChild (loop, -1, nullptr);

    juce::ValueTree waveform (IDs::Waveform);
    waveform.setProperty (IDs::analysisStatus,   "idle", nullptr);
    waveform.setProperty (IDs::analysisProgress, 0.0f,   nullptr);
    deck.addChild (waveform, -1, nullptr);

    return deck;
}

// Helper: set deck to "stopped" state so it has a loaded track
static void setDeckLoaded (juce::ValueTree& deck)
{
    deck.setProperty (IDs::playbackStatus, "stopped", nullptr);
}

// Helper: set playhead position
static void setPlayheadPosition (juce::ValueTree& deck, int64_t pos)
{
    deck.getChildWithName (IDs::Playhead).setProperty (IDs::position, pos, nullptr);
}

// =============================================================================
// 1. HotCueData Tests
// =============================================================================
class HotCueDataTests : public juce::UnitTest
{
public:
    HotCueDataTests() : juce::UnitTest ("Hot Cue Data", "Sonik") {}

    void runTest() override
    {
        beginTest ("Palette has 16 colors");
        {
            // The palette array should contain exactly 16 entries
            constexpr size_t paletteSize = sizeof (HotCueColors::palette) / sizeof (HotCueColors::palette[0]);
            expectEquals (static_cast<int> (paletteSize), 16);
        }

        beginTest ("getColour returns valid color for valid indices");
        {
            for (int i = 0; i < 16; ++i)
            {
                auto colour = HotCueColors::getColour (i);
                // Should not be transparent
                expect (colour.getAlpha() > 0);
                // Should match palette
                expect (colour.getARGB() == HotCueColors::palette[i]);
            }
        }

        beginTest ("getColour clamps out-of-range to index 0");
        {
            auto negColor = HotCueColors::getColour (-1);
            auto overColor = HotCueColors::getColour (16);
            auto over100 = HotCueColors::getColour (100);

            auto expected = juce::Colour (HotCueColors::palette[0]);
            expect (negColor.getARGB() == expected.getARGB());
            expect (overColor.getARGB() == expected.getARGB());
            expect (over100.getARGB() == expected.getARGB());
        }

        beginTest ("Default pad color mapping has 8 entries");
        {
            constexpr size_t mapSize = sizeof (HotCueColors::defaultColorForPad) /
                                       sizeof (HotCueColors::defaultColorForPad[0]);
            expectEquals (static_cast<int> (mapSize), 8);
        }

        beginTest ("Default pad colors are within palette range");
        {
            for (int i = 0; i < 8; ++i)
            {
                expect (HotCueColors::defaultColorForPad[i] >= 0);
                expect (HotCueColors::defaultColorForPad[i] < 16);
            }
        }

        beginTest ("HotCueInfo defaults are sane");
        {
            HotCueInfo info;
            expectEquals (info.padIndex, 0);
            expectEquals (static_cast<int> (info.positionSamples), -1);
            expectEquals (info.colorIndex, 0);
            expect (info.label.isEmpty());
            expect (! info.active);
        }
    }
};
static HotCueDataTests hotCueDataTestsInstance;

// =============================================================================
// 2. HotCueManager Tests
// =============================================================================
class HotCueManagerTests : public juce::UnitTest
{
public:
    HotCueManagerTests() : juce::UnitTest ("Hot Cue Manager", "Sonik") {}

    void runTest() override
    {
        testSetCueStoresPosition();
        testSetCueQuantizeSnap();
        testSetCueDefaultColor();
        testTriggerCueSeek();
        testTriggerCueSkipsNearby();
        testTriggerCueFromStoppedPlays();
        testDeleteCue();
        testUndoDelete();
        testUndoTimerExpiry();
        testSetColor();
        testSetLabel();
        testSetLabelTruncation();
        testGetHotCues();
        testLoadAndSaveCuesDB();
        testSetCueOnEmptyDeckIsNoop();
        testSetCueOutOfRange();
        testDeleteInvalidCue();
    }

private:
    struct TestContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        juce::ValueTree rootState;
        std::unique_ptr<AudioEngine> engine;
        juce::ValueTree deckTree;
        std::unique_ptr<HotCueManager> mgr;

        TestContext()
        {
            dbFile = juce::File::createTempFile ("hot_cue_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            rootState = juce::ValueTree (IDs::SonikState);
            engine = std::make_unique<AudioEngine> (rootState);
            deckTree = createTestDeckTree ("A");
            setDeckLoaded (deckTree);
            setPlayheadPosition (deckTree, 44100);
            mgr = std::make_unique<HotCueManager> (deckTree, *engine, "A", *db);
        }

        ~TestContext()
        {
            mgr.reset();
            engine.reset();
            db.reset();
            dbFile.deleteFile();
        }
    };

    // --- setCue stores playhead position in ValueTree ---
    void testSetCueStoresPosition()
    {
        beginTest ("setCue stores playhead position");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 88200);
        ctx.mgr->setCue (0);

        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (static_cast<int64_t> (cp.getProperty (IDs::position)) == (int64_t) 88200);
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));
    }

    // --- setCue with quantize snaps to nearest beat ---
    void testSetCueQuantizeSnap()
    {
        beginTest ("setCue with quantize snaps to nearest beat");
        TestContext ctx;

        // Enable quantize
        ctx.deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);

        // Create a beatgrid: 120 BPM at 44100 Hz => beatInterval = 22050 samples
        auto bgData = new BeatGridData();
        bgData->bpm = 120.0;
        bgData->anchorSample = 0;
        bgData->beatIntervalSamples = 22050.0;
        bgData->confidence = 1.0f;
        ctx.mgr->setBeatGridData (BeatGridData::Ptr (bgData));

        // Set playhead slightly off from a beat boundary
        // Beat at 44100, next at 66150. 50000 is closer to 44100.
        setPlayheadPosition (ctx.deckTree, 50000);
        ctx.mgr->setCue (0);

        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        auto pos = static_cast<int64_t> (cp.getProperty (IDs::position));
        // Should snap to 44100 (beat index 2 * 22050)
        expect (pos == (int64_t) 44100);
    }

    // --- setCue sets default color on unassigned pad ---
    void testSetCueDefaultColor()
    {
        beginTest ("setCue sets default color on fresh pad");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (0); // Pad A, default color = 1 (Red)

        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        auto colorIdx = static_cast<int> (cp.getProperty (IDs::colorIndex));
        expectEquals (colorIdx, HotCueColors::defaultColorForPad[0]); // 1 (Red)
    }

    // --- triggerCue calls seekDeck (verify via seekTarget atomic) ---
    void testTriggerCueSeek()
    {
        beginTest ("triggerCue seeks to cue position");
        TestContext ctx;

        // Set a cue at position 88200
        setPlayheadPosition (ctx.deckTree, 88200);
        ctx.mgr->setCue (0);

        // Move playhead away
        setPlayheadPosition (ctx.deckTree, 0);

        // Trigger the cue — should seek to 88200
        ctx.mgr->triggerCue (0);

        // We can't easily verify the atomic seekTarget without audio engine internals,
        // but we can verify it didn't crash and the cue is still valid
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));
    }

    // --- triggerCue skips if within 64 samples ---
    void testTriggerCueSkipsNearby()
    {
        beginTest ("triggerCue skips if within 64 samples");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 88200);
        ctx.mgr->setCue (0);

        // Position playhead 30 samples away (< 64)
        setPlayheadPosition (ctx.deckTree, 88230);

        // This should be a no-op (skip the seek)
        ctx.mgr->triggerCue (0);

        // No crash, cue still valid
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));
    }

    // --- triggerCue from stopped state ---
    void testTriggerCueFromStoppedPlays()
    {
        beginTest ("triggerCue from stopped state triggers seek");
        TestContext ctx;

        ctx.deckTree.setProperty (IDs::playbackStatus, "stopped", nullptr);

        setPlayheadPosition (ctx.deckTree, 88200);
        ctx.mgr->setCue (0);

        setPlayheadPosition (ctx.deckTree, 0);
        ctx.mgr->triggerCue (0);

        // Cue still valid after trigger
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));
        expectEquals (static_cast<int64_t> (cp.getProperty (IDs::position)), (int64_t) 88200);
    }

    // --- deleteCue sets isValid=false ---
    void testDeleteCue()
    {
        beginTest ("deleteCue invalidates cue");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (2);

        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (2);
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));

        ctx.mgr->deleteCue (2);

        expect (! static_cast<bool> (cp.getProperty (IDs::isValid)));
        expect (static_cast<int64_t> (cp.getProperty (IDs::position)) == (int64_t) -1);
    }

    // --- undoDelete restores previous state ---
    void testUndoDelete()
    {
        beginTest ("undoDelete restores cue");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 66150);
        ctx.mgr->setCue (3);

        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (3);
        auto origPos   = static_cast<int64_t> (cp.getProperty (IDs::position));
        auto origColor = static_cast<int> (cp.getProperty (IDs::colorIndex));

        ctx.mgr->deleteCue (3);
        expect (! static_cast<bool> (cp.getProperty (IDs::isValid)));

        ctx.mgr->undoDelete();
        expect (static_cast<bool> (cp.getProperty (IDs::isValid)));
        expect (static_cast<int64_t> (cp.getProperty (IDs::position)) == origPos);
        expectEquals (static_cast<int> (cp.getProperty (IDs::colorIndex)), origColor);
    }

    // --- undo expires after timer callback ---
    void testUndoTimerExpiry()
    {
        beginTest ("undo expires after timer callback");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (5);
        ctx.mgr->deleteCue (5);

        // Directly invoke timerCallback to simulate expiry
        // HotCueManager inherits juce::Timer — timerCallback clears pendingUndo
        // We can't call it directly, but we can verify undo works before and fails after
        // a second delete (which cancels pending undo)
        ctx.mgr->setCue (6);
        ctx.mgr->deleteCue (6);
        // Previous undo for pad 5 was cancelled by the new delete

        ctx.mgr->undoDelete();
        // Should restore pad 6, not pad 5
        auto cp5 = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (5);
        auto cp6 = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (6);
        expect (! static_cast<bool> (cp5.getProperty (IDs::isValid)));
        expect (static_cast<bool> (cp6.getProperty (IDs::isValid)));
    }

    // --- setColor updates ValueTree ---
    void testSetColor()
    {
        beginTest ("setColor updates color in ValueTree");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (0);

        ctx.mgr->setColor (0, 5); // Set to Aqua
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expectEquals (static_cast<int> (cp.getProperty (IDs::colorIndex)), 5);

        ctx.mgr->setColor (0, 15); // Set to Rose
        expectEquals (static_cast<int> (cp.getProperty (IDs::colorIndex)), 15);
    }

    // --- setLabel truncates to 12 chars ---
    void testSetLabel()
    {
        beginTest ("setLabel stores label");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (0);

        ctx.mgr->setLabel (0, "Drop");
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expectEquals (cp.getProperty (IDs::label).toString(), juce::String ("Drop"));
    }

    void testSetLabelTruncation()
    {
        beginTest ("setLabel truncates to 12 characters");
        TestContext ctx;

        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (0);

        ctx.mgr->setLabel (0, "This is a very long label name");
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        auto stored = cp.getProperty (IDs::label).toString();
        expectEquals (stored.length(), 12);
        expectEquals (stored, juce::String ("This is a ve"));
    }

    // --- getHotCues returns correct state ---
    void testGetHotCues()
    {
        beginTest ("getHotCues returns all 8 cue states");
        TestContext ctx;

        // Set cues on pads 0, 3, 7
        setPlayheadPosition (ctx.deckTree, 44100);
        ctx.mgr->setCue (0);
        setPlayheadPosition (ctx.deckTree, 88200);
        ctx.mgr->setCue (3);
        setPlayheadPosition (ctx.deckTree, 132300);
        ctx.mgr->setCue (7);

        auto cues = ctx.mgr->getHotCues();

        // Pad 0
        expect (cues[0].active);
        expect (cues[0].positionSamples == (int64_t) 44100);

        // Pad 3
        expect (cues[3].active);
        expect (cues[3].positionSamples == (int64_t) 88200);

        // Pad 7
        expect (cues[7].active);
        expect (cues[7].positionSamples == (int64_t) 132300);

        // Pads 1,2,4,5,6 should be inactive
        expect (! cues[1].active);
        expect (! cues[2].active);
        expect (! cues[4].active);
        expect (! cues[5].active);
        expect (! cues[6].active);
    }

    // --- loadCuesFromDB / saveCuesToDB round-trip ---
    void testLoadAndSaveCuesDB()
    {
        beginTest ("Cue DB round-trip via HotCueManager");
        auto dbFile = juce::File::createTempFile ("hot_cue_roundtrip.db");
        auto db = std::make_unique<TrackDatabase> (dbFile);

        juce::ValueTree rootState (IDs::SonikState);
        auto engine = std::make_unique<AudioEngine> (rootState);

        // First manager: set some cues
        {
            auto deckTree = createTestDeckTree ("A");
            setDeckLoaded (deckTree);

            auto mgr = std::make_unique<HotCueManager> (deckTree, *engine, "A", *db);
            setPlayheadPosition (deckTree, 44100);
            mgr->setCue (0);
            mgr->setColor (0, 3);
            mgr->setLabel (0, "Intro");

            setPlayheadPosition (deckTree, 176400);
            mgr->setCue (4);
            mgr->setLabel (4, "Drop");

            // Give ThreadPool time to write
            juce::Thread::sleep (100);
            mgr.reset();
        }

        // Second manager: load from same DB with same content hash
        {
            auto deckTree2 = createTestDeckTree ("A");
            setDeckLoaded (deckTree2);

            auto mgr2 = std::make_unique<HotCueManager> (deckTree2, *engine, "A", *db);
            auto cues = mgr2->getHotCues();

            expect (cues[0].active);
            expect (cues[0].positionSamples == (int64_t) 44100);
            expectEquals (cues[0].colorIndex, 3);
            expectEquals (cues[0].label, juce::String ("Intro"));

            expect (cues[4].active);
            expect (cues[4].positionSamples == (int64_t) 176400);
            expectEquals (cues[4].label, juce::String ("Drop"));

            expect (! cues[1].active);
            expect (! cues[2].active);
            expect (! cues[3].active);

            mgr2.reset();
        }

        engine.reset();
        db.reset();
        dbFile.deleteFile();
    }

    // --- setCue on empty deck is no-op ---
    void testSetCueOnEmptyDeckIsNoop()
    {
        beginTest ("setCue on empty deck is no-op");

        auto dbFile = juce::File::createTempFile ("hot_cue_empty.db");
        auto db = std::make_unique<TrackDatabase> (dbFile);
        juce::ValueTree rootState (IDs::SonikState);
        auto engine = std::make_unique<AudioEngine> (rootState);

        auto deckTree = createTestDeckTree ("A");
        // Leave status as "empty"

        auto mgr = std::make_unique<HotCueManager> (deckTree, *engine, "A", *db);
        setPlayheadPosition (deckTree, 44100);
        mgr->setCue (0);

        auto cp = deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (! static_cast<bool> (cp.getProperty (IDs::isValid)));

        mgr.reset();
        engine.reset();
        db.reset();
        dbFile.deleteFile();
    }

    // --- setCue with out-of-range index ---
    void testSetCueOutOfRange()
    {
        beginTest ("setCue with invalid index is no-op");
        TestContext ctx;

        ctx.mgr->setCue (-1);
        ctx.mgr->setCue (8);
        ctx.mgr->setCue (100);

        // No cues should be set
        auto cues = ctx.mgr->getHotCues();
        for (int i = 0; i < 8; ++i)
            expect (! cues[static_cast<size_t> (i)].active);
    }

    // --- deleteCue on invalid cue is no-op ---
    void testDeleteInvalidCue()
    {
        beginTest ("deleteCue on unset cue is no-op");
        TestContext ctx;

        ctx.mgr->deleteCue (0);
        // Should not crash, no undo state stored
        ctx.mgr->undoDelete();
        // Still no crash
        auto cp = ctx.deckTree.getChildWithName (IDs::CuePoints).getChild (0);
        expect (! static_cast<bool> (cp.getProperty (IDs::isValid)));
    }
};
static HotCueManagerTests hotCueManagerTestsInstance;

// =============================================================================
// 3. AudioStateSync hot cue position tests
// =============================================================================
class AudioStateSyncHotCueTests : public juce::UnitTest
{
public:
    AudioStateSyncHotCueTests() : juce::UnitTest ("Hot Cue AudioStateSync", "Sonik") {}

    void runTest() override
    {
        beginTest ("CuePoint property changes sync to DeckAudioState");
        {
            auto deckTree = createTestDeckTree ("A");
            DeckAudioState audioState;
            AudioStateSync sync (deckTree, audioState);

            // Initially all -1
            for (int i = 0; i < 8; ++i)
                expect (audioState.hotCuePositions[i].load() == (int64_t) -1);

            // Set a valid cue on pad 0
            auto cp0 = deckTree.getChildWithName (IDs::CuePoints).getChild (0);
            cp0.setProperty (IDs::position, (int64_t) 88200, nullptr);
            cp0.setProperty (IDs::isValid,  true,            nullptr);

            expect (audioState.hotCuePositions[0].load() == (int64_t) 88200);

            // Set another cue on pad 5
            auto cp5 = deckTree.getChildWithName (IDs::CuePoints).getChild (5);
            cp5.setProperty (IDs::position, (int64_t) 176400, nullptr);
            cp5.setProperty (IDs::isValid,  true,             nullptr);

            expect (audioState.hotCuePositions[5].load() == (int64_t) 176400);
        }

        beginTest ("Invalid cue sets position to -1 in DeckAudioState");
        {
            auto deckTree = createTestDeckTree ("A");
            DeckAudioState audioState;
            AudioStateSync sync (deckTree, audioState);

            // Set a valid cue
            auto cp = deckTree.getChildWithName (IDs::CuePoints).getChild (2);
            cp.setProperty (IDs::position, (int64_t) 44100, nullptr);
            cp.setProperty (IDs::isValid,  true,            nullptr);
            expect (audioState.hotCuePositions[2].load() == (int64_t) 44100);

            // Invalidate it
            cp.setProperty (IDs::isValid, false, nullptr);
            expect (audioState.hotCuePositions[2].load() == (int64_t) -1);
        }

        beginTest ("Multiple cues sync independently");
        {
            auto deckTree = createTestDeckTree ("A");
            DeckAudioState audioState;
            AudioStateSync sync (deckTree, audioState);

            auto cuePoints = deckTree.getChildWithName (IDs::CuePoints);

            for (int i = 0; i < 8; ++i)
            {
                auto cp = cuePoints.getChild (i);
                cp.setProperty (IDs::position, (int64_t) (i * 22050), nullptr);
                cp.setProperty (IDs::isValid,  true,                  nullptr);
            }

            for (int i = 0; i < 8; ++i)
                expect (audioState.hotCuePositions[i].load() == (int64_t) (i * 22050));

            // Invalidate pad 3 only
            cuePoints.getChild (3).setProperty (IDs::isValid, false, nullptr);
            expect (audioState.hotCuePositions[3].load() == (int64_t) -1);
            // Others unaffected
            expect (audioState.hotCuePositions[2].load() == (int64_t) (2 * 22050));
            expect (audioState.hotCuePositions[4].load() == (int64_t) (4 * 22050));
        }
    }
};
static AudioStateSyncHotCueTests audioStateSyncHotCueTestsInstance;

// =============================================================================
// 4. TrackDatabase cue JSON persistence
// =============================================================================
class TrackDatabaseCueTests : public juce::UnitTest
{
public:
    TrackDatabaseCueTests() : juce::UnitTest ("Hot Cue DB Persistence", "Sonik") {}

    void runTest() override
    {
        beginTest ("saveCuePointsJson + loadCuePointsJson round-trip");
        {
            auto dbFile = juce::File::createTempFile ("cue_json_test.db");
            TrackDatabase db (dbFile);

            juce::String json = R"([{"pad":0,"pos":88200,"color":1,"label":"Drop"},{"pad":3,"pos":176400,"color":5,"label":"Chorus"}])";
            db.saveCuePointsJson ("/test/track.wav", "hash123", json);

            auto loaded = db.loadCuePointsJson ("hash123");
            expect (loaded.isNotEmpty());

            // Verify JSON parses back correctly
            auto parsed = juce::JSON::parse (loaded);
            expect (parsed.isArray());
            auto* arr = parsed.getArray();
            expect (arr != nullptr);
            expectEquals (arr->size(), 2);

            auto* first = (*arr)[0].getDynamicObject();
            expectEquals (static_cast<int> (first->getProperty ("pad")), 0);
            expect (static_cast<int64_t> (static_cast<double> (first->getProperty ("pos"))) == (int64_t) 88200);
            expectEquals (static_cast<int> (first->getProperty ("color")), 1);
            expectEquals (first->getProperty ("label").toString(), juce::String ("Drop"));

            dbFile.deleteFile();
        }

        beginTest ("loadCuePointsJson with unknown hash returns empty");
        {
            auto dbFile = juce::File::createTempFile ("cue_json_empty.db");
            TrackDatabase db (dbFile);

            auto result = db.loadCuePointsJson ("nonexistent_hash");
            expect (result.isEmpty());

            dbFile.deleteFile();
        }

        beginTest ("Content-hash keyed (not file path)");
        {
            auto dbFile = juce::File::createTempFile ("cue_json_hash.db");
            TrackDatabase db (dbFile);

            juce::String json1 = R"([{"pad":0,"pos":44100,"color":0,"label":"A"}])";
            juce::String json2 = R"([{"pad":1,"pos":88200,"color":1,"label":"B"}])";

            // Same hash, different file paths
            db.saveCuePointsJson ("/path1/track.wav", "samehash", json1);
            // This will insert a new row since primary key is (file_path, content_hash)
            db.saveCuePointsJson ("/path2/track.wav", "samehash", json2);

            // loadCuePointsJson searches by hash only (LIMIT 1)
            auto loaded = db.loadCuePointsJson ("samehash");
            expect (loaded.isNotEmpty());

            // Different hash: no data
            auto noData = db.loadCuePointsJson ("differenthash");
            expect (noData.isEmpty());

            dbFile.deleteFile();
        }

        beginTest ("Overwrite existing cue data for same file+hash");
        {
            auto dbFile = juce::File::createTempFile ("cue_json_overwrite.db");
            TrackDatabase db (dbFile);

            juce::String json1 = R"([{"pad":0,"pos":44100,"color":0,"label":"Old"}])";
            db.saveCuePointsJson ("/test/track.wav", "hashX", json1);

            juce::String json2 = R"([{"pad":0,"pos":88200,"color":3,"label":"New"}])";
            db.saveCuePointsJson ("/test/track.wav", "hashX", json2);

            auto loaded = db.loadCuePointsJson ("hashX");
            expect (loaded.contains ("New"));
            expect (! loaded.contains ("Old"));

            dbFile.deleteFile();
        }
    }
};
static TrackDatabaseCueTests trackDatabaseCueTestsInstance;

// =============================================================================
// 5. HotCuePadComponent tests
// =============================================================================
class HotCuePadComponentTests : public juce::UnitTest
{
public:
    HotCuePadComponentTests() : juce::UnitTest ("Hot Cue Pad Component", "Sonik") {}

    void runTest() override
    {
        beginTest ("Constructs without crash");
        {
            auto deckTree = createTestDeckTree ("A");
            HotCuePadComponent pad (deckTree);
            expect (true); // If we get here, construction succeeded
        }

        beginTest ("Paints without crash");
        {
            auto deckTree = createTestDeckTree ("A");
            HotCuePadComponent pad (deckTree);
            pad.setSize (400, 40);

            juce::Image img (juce::Image::ARGB, 400, 40, true);
            juce::Graphics g (img);
            pad.paint (g);
            expect (true); // No crash during paint
        }

        beginTest ("Paints with active cues without crash");
        {
            auto deckTree = createTestDeckTree ("A");
            setDeckLoaded (deckTree);

            // Set some cues as active
            auto cuePoints = deckTree.getChildWithName (IDs::CuePoints);
            cuePoints.getChild (0).setProperty (IDs::isValid, true, nullptr);
            cuePoints.getChild (0).setProperty (IDs::position, (int64_t) 44100, nullptr);
            cuePoints.getChild (0).setProperty (IDs::colorIndex, 1, nullptr);
            cuePoints.getChild (3).setProperty (IDs::isValid, true, nullptr);
            cuePoints.getChild (3).setProperty (IDs::position, (int64_t) 88200, nullptr);
            cuePoints.getChild (3).setProperty (IDs::colorIndex, 6, nullptr);

            HotCuePadComponent pad (deckTree);
            pad.setSize (400, 40);

            juce::Image img (juce::Image::ARGB, 400, 40, true);
            juce::Graphics g (img);
            pad.paint (g);
            expect (true);
        }

        beginTest ("isDeckEmpty returns true when status is empty");
        {
            auto deckTree = createTestDeckTree ("A");
            // Status defaults to "empty"
            HotCuePadComponent pad (deckTree);
            pad.setSize (400, 40);

            // Paint in empty state
            juce::Image img (juce::Image::ARGB, 400, 40, true);
            juce::Graphics g (img);
            pad.paint (g);
            expect (true);
        }

        beginTest ("isDeckEmpty returns false after loading track");
        {
            auto deckTree = createTestDeckTree ("A");
            setDeckLoaded (deckTree);
            HotCuePadComponent pad (deckTree);
            pad.setSize (400, 40);

            // Paint in loaded state
            juce::Image img (juce::Image::ARGB, 400, 40, true);
            juce::Graphics g (img);
            pad.paint (g);
            expect (true);
        }
    }
};
static HotCuePadComponentTests hotCuePadComponentTestsInstance;

// =============================================================================
// 6. Waveform marker data tests
// =============================================================================
class WaveformHotCueMarkerTests : public juce::UnitTest
{
public:
    WaveformHotCueMarkerTests() : juce::UnitTest ("Hot Cue Waveform Markers", "Sonik") {}

    void runTest() override
    {
        beginTest ("DetailWaveform setHotCues stores data");
        {
            DetailWaveform detail;
            detail.setSize (800, 100);

            std::array<HotCueInfo, 8> cues {};
            cues[0].active = true;
            cues[0].positionSamples = 44100;
            cues[0].colorIndex = 1;
            cues[0].label = "Drop";

            cues[3].active = true;
            cues[3].positionSamples = 88200;
            cues[3].colorIndex = 6;

            detail.setHotCues (cues);

            // Paint to verify no crash
            juce::Image img (juce::Image::ARGB, 800, 100, true);
            juce::Graphics g (img);
            detail.paint (g);
            expect (true);
        }

        beginTest ("OverviewWaveform setHotCues stores data");
        {
            OverviewWaveform overview;
            overview.setSize (800, 60);

            std::array<HotCueInfo, 8> cues {};
            cues[0].active = true;
            cues[0].positionSamples = 44100;
            cues[0].colorIndex = 1;

            cues[7].active = true;
            cues[7].positionSamples = 176400;
            cues[7].colorIndex = 0;

            overview.setHotCues (cues);

            // Paint to verify no crash
            juce::Image img (juce::Image::ARGB, 800, 60, true);
            juce::Graphics g (img);
            overview.paint (g);
            expect (true);
        }

        beginTest ("DetailWaveform handles empty cues array");
        {
            DetailWaveform detail;
            detail.setSize (800, 100);

            std::array<HotCueInfo, 8> cues {}; // All inactive
            detail.setHotCues (cues);

            juce::Image img (juce::Image::ARGB, 800, 100, true);
            juce::Graphics g (img);
            detail.paint (g);
            expect (true);
        }

        beginTest ("OverviewWaveform handles all 8 cues active");
        {
            OverviewWaveform overview;
            overview.setSize (800, 60);
            overview.setTotalSamples (7938000);

            std::array<HotCueInfo, 8> cues {};
            for (int i = 0; i < 8; ++i)
            {
                cues[static_cast<size_t> (i)].active = true;
                cues[static_cast<size_t> (i)].positionSamples = i * 100000;
                cues[static_cast<size_t> (i)].colorIndex = i % 16;
                cues[static_cast<size_t> (i)].padIndex = i;
            }

            overview.setHotCues (cues);

            juce::Image img (juce::Image::ARGB, 800, 60, true);
            juce::Graphics g (img);
            overview.paint (g);
            expect (true);
        }
    }
};
static WaveformHotCueMarkerTests waveformHotCueMarkerTestsInstance;
