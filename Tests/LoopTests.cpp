#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Loop/LoopEngine.h"
#include "Features/Loop/LoopControlComponent.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/BeatGrid/BeatGridData.h"
#include "Features/Quantize/QuantizeService.h"

// =============================================================================
// Helper: build a full deck ValueTree matching DeckStateManager::createDeckTree
// =============================================================================
static juce::ValueTree createLoopTestDeckTree (const juce::String& deckId)
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
    trackMeta.setProperty (IDs::contentHash,  "looptesthash",    nullptr);
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
    loop.setProperty (IDs::loopIn,   (int64_t) -1, nullptr);
    loop.setProperty (IDs::loopOut,  (int64_t) -1, nullptr);
    loop.setProperty (IDs::active,   false,         nullptr);
    loop.setProperty (IDs::loopMode, 0,             nullptr);
    deck.addChild (loop, -1, nullptr);

    juce::ValueTree waveform (IDs::Waveform);
    waveform.setProperty (IDs::analysisStatus,   "idle", nullptr);
    waveform.setProperty (IDs::analysisProgress, 0.0f,   nullptr);
    deck.addChild (waveform, -1, nullptr);

    return deck;
}

static void setLoopDeckLoaded (juce::ValueTree& deck)
{
    deck.setProperty (IDs::playbackStatus, "stopped", nullptr);
}

static void setLoopPlayhead (juce::ValueTree& deck, int64_t pos)
{
    deck.getChildWithName (IDs::Playhead).setProperty (IDs::position, pos, nullptr);
}

// =============================================================================
// Test context: creates a LoopEngine with its dependencies
// =============================================================================
struct LoopTestContext
{
    juce::File dbFile;
    std::unique_ptr<TrackDatabase> db;
    juce::ValueTree rootState;
    std::unique_ptr<AudioEngine> engine;
    juce::ValueTree deckTree;
    DeckAudioState audioState;
    std::unique_ptr<LoopEngine> loopEngine;

    LoopTestContext()
    {
        dbFile    = juce::File::createTempFile ("loop_test.db");
        db        = std::make_unique<TrackDatabase> (dbFile);
        rootState = juce::ValueTree (IDs::SonikState);
        engine    = std::make_unique<AudioEngine> (rootState);
        deckTree  = createLoopTestDeckTree ("A");
        setLoopDeckLoaded (deckTree);
        setLoopPlayhead (deckTree, 500000);
        loopEngine = std::make_unique<LoopEngine> (deckTree, *engine, "A", *db);
        loopEngine->setAudioState (&audioState);

        // Sync playhead to audioState so readPlayhead() returns the right value
        audioState.playheadPosition.store (500000, std::memory_order_relaxed);
    }

    ~LoopTestContext()
    {
        loopEngine.reset();
        engine.reset();
        db.reset();
        dbFile.deleteFile();
    }

    void setBeatGrid (double bpm)
    {
        double sr = 44100.0;
        double interval = (bpm > 0.0) ? (sr * 60.0 / bpm) : 0.0;

        auto bg = deckTree.getChildWithName (IDs::BeatGrid);
        bg.setProperty (IDs::bpm,                 bpm,          nullptr);
        bg.setProperty (IDs::anchorSample,        (int64_t) 0,  nullptr);
        bg.setProperty (IDs::beatIntervalSamples, interval,     nullptr);

        auto bgData   = new BeatGridData();
        bgData->bpm   = bpm;
        bgData->anchorSample        = 0;
        bgData->beatIntervalSamples = interval;
        bgData->analysisSampleRate  = sr;
        loopEngine->setBeatGridData (bgData);

        audioState.beatgridAnchor.store (0, std::memory_order_relaxed);
        audioState.beatgridInterval.store (interval, std::memory_order_relaxed);
    }

    juce::ValueTree loopNode() const { return deckTree.getChildWithName (IDs::Loop); }
};

// =============================================================================
// 1. LoopEngine Unit Tests
// =============================================================================
class LoopEngineTests : public juce::UnitTest
{
public:
    LoopEngineTests() : juce::UnitTest ("Loop Engine", "Sonik") {}

    void runTest() override
    {
        testAutoLoop4Beats();
        testAutoLoopNoBeatgridFallback();
        testAutoLoopToggleOff();
        testAutoLoopChangeSize();
        testManualLoop();
        testManualLoopSwap();
        testManualLoopTooShort();
        testLoopHalve();
        testLoopDouble();
        testLoopHalveMinimum();
        testLoopDoubleMaximum();
        testLoopExit();
        testReLoop();
        testReLoopNoOp();
        testQuantizeAutoLoop();
        testQuantizeManualLoop();
        testLoopStateInValueTree();
    }

private:
    // -----------------------------------------------------------------------
    // 1. Auto-loop 4 beats with beatgrid
    // -----------------------------------------------------------------------
    void testAutoLoop4Beats()
    {
        beginTest ("Auto-loop 4 beats sets correct loopIn/loopOut with beatgrid");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        // 128 BPM @ 44100: beat interval = 44100 * 60 / 128 = 20671.875
        double interval = 44100.0 * 60.0 / 128.0;
        int64_t expected4beats = static_cast<int64_t> (4.0 * interval);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();

        expect (info.active, "Loop should be active");
        expect (info.inSamples >= 0, "loopIn should be set");
        expectEquals (info.outSamples - info.inSamples, expected4beats);
        expectEquals (info.mode, 1); // auto mode
    }

    // -----------------------------------------------------------------------
    // 2. Auto-loop with no beatgrid uses 120 BPM fallback
    // -----------------------------------------------------------------------
    void testAutoLoopNoBeatgridFallback()
    {
        beginTest ("Auto-loop with no beatgrid uses 120 BPM fallback");
        LoopTestContext ctx;
        // No beatgrid set → BPM = 0.0

        // Fallback: 120 BPM → interval = 44100 * 60 / 120 = 22050
        int64_t expectedLen = static_cast<int64_t> (4.0 * 22050.0);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();

        expect (info.active, "Loop should be active");
        expectEquals (info.outSamples - info.inSamples, expectedLen);
    }

    // -----------------------------------------------------------------------
    // 3. Auto-loop toggle: pressing same size deactivates loop
    // -----------------------------------------------------------------------
    void testAutoLoopToggleOff()
    {
        beginTest ("Auto-loop toggle: pressing same size deactivates loop");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        ctx.loopEngine->autoLoop (4.0f);
        expect (ctx.loopEngine->getLoopInfo().active, "Loop should be active after first press");

        ctx.loopEngine->autoLoop (4.0f);
        expect (! ctx.loopEngine->getLoopInfo().active, "Loop should be inactive after second press");
    }

    // -----------------------------------------------------------------------
    // 4. Auto-loop change: pressing different size changes loopOut, keeps loopIn
    // -----------------------------------------------------------------------
    void testAutoLoopChangeSize()
    {
        beginTest ("Auto-loop change: pressing different size changes loopOut, keeps loopIn");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        double interval = 44100.0 * 60.0 / 128.0;

        ctx.loopEngine->autoLoop (4.0f);
        auto info4 = ctx.loopEngine->getLoopInfo();
        int64_t originalIn = info4.inSamples;

        ctx.loopEngine->autoLoop (8.0f);
        auto info8 = ctx.loopEngine->getLoopInfo();

        expect (info8.active, "Loop should remain active");
        expectEquals (info8.inSamples, originalIn);
        expectEquals (info8.outSamples - info8.inSamples,
                      static_cast<int64_t> (8.0 * interval));
    }

    // -----------------------------------------------------------------------
    // 5. Manual loop: setLoopIn + setLoopOut activates loop
    // -----------------------------------------------------------------------
    void testManualLoop()
    {
        beginTest ("Manual loop: setLoopIn + setLoopOut activates loop");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        // Set playhead for IN
        ctx.audioState.playheadPosition.store (100000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();

        auto info1 = ctx.loopEngine->getLoopInfo();
        expect (info1.pendingIn, "Should have pending loop-in");
        expect (! info1.active, "Loop should not be active yet");

        // Move playhead for OUT
        ctx.audioState.playheadPosition.store (200000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info2 = ctx.loopEngine->getLoopInfo();
        expect (info2.active, "Loop should be active");
        expect (info2.inSamples >= 0, "loopIn should be set");
        expect (info2.outSamples > info2.inSamples, "loopOut > loopIn");
        expectEquals (info2.mode, 2); // manual mode
    }

    // -----------------------------------------------------------------------
    // 6. Manual loop: out before in → points swap
    // -----------------------------------------------------------------------
    void testManualLoopSwap()
    {
        beginTest ("Manual loop: out before in → points swap");
        LoopTestContext ctx;

        // Set IN at a later position
        ctx.audioState.playheadPosition.store (200000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();

        // Set OUT at an earlier position
        ctx.audioState.playheadPosition.store (100000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info = ctx.loopEngine->getLoopInfo();
        expect (info.active, "Loop should be active after swap");
        expect (info.inSamples < info.outSamples, "Points should be swapped: in < out");
        expectEquals (info.inSamples, (int64_t) 100000);
        expectEquals (info.outSamples, (int64_t) 200000);
    }

    // -----------------------------------------------------------------------
    // 7. Manual loop: too short (< 128 samples) is rejected
    // -----------------------------------------------------------------------
    void testManualLoopTooShort()
    {
        beginTest ("Manual loop: too short (< 128 samples) is rejected");
        LoopTestContext ctx;

        ctx.audioState.playheadPosition.store (100000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();

        // Set OUT only 50 samples later
        ctx.audioState.playheadPosition.store (100050, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info = ctx.loopEngine->getLoopInfo();
        expect (! info.active, "Loop should NOT activate for too-short distance");
    }

    // -----------------------------------------------------------------------
    // 8. Loop halve: halves the loop length from loopIn
    // -----------------------------------------------------------------------
    void testLoopHalve()
    {
        beginTest ("Loop halve: halves the loop length from loopIn");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();
        int64_t originalLen = info.outSamples - info.inSamples;
        int64_t originalIn  = info.inSamples;

        ctx.loopEngine->loopHalve();
        auto info2 = ctx.loopEngine->getLoopInfo();

        expectEquals (info2.inSamples, originalIn);
        expectEquals (info2.outSamples - info2.inSamples, originalLen / 2);
    }

    // -----------------------------------------------------------------------
    // 9. Loop double: doubles the loop length from loopIn
    // -----------------------------------------------------------------------
    void testLoopDouble()
    {
        beginTest ("Loop double: doubles the loop length from loopIn");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();
        int64_t originalLen = info.outSamples - info.inSamples;
        int64_t originalIn  = info.inSamples;

        ctx.loopEngine->loopDouble();
        auto info2 = ctx.loopEngine->getLoopInfo();

        expectEquals (info2.inSamples, originalIn);
        expectEquals (info2.outSamples - info2.inSamples, originalLen * 2);
    }

    // -----------------------------------------------------------------------
    // 10. Loop halve minimum: won't go below 128 samples
    // -----------------------------------------------------------------------
    void testLoopHalveMinimum()
    {
        beginTest ("Loop halve minimum: won't go below 128 samples");
        LoopTestContext ctx;
        // Use high BPM so that interval/32 < 128 → floor is 128
        // 1000 BPM → interval = 44100*60/1000 = 2646, interval/32 ≈ 82 < 128
        ctx.setBeatGrid (1000.0);

        // Create a manual loop of 256 samples
        ctx.audioState.playheadPosition.store (100000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();
        ctx.audioState.playheadPosition.store (100256, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info1 = ctx.loopEngine->getLoopInfo();
        expect (info1.active);
        expectEquals (info1.outSamples - info1.inSamples, (int64_t) 256);

        // First halve: 256 → 128
        ctx.loopEngine->loopHalve();
        auto info2 = ctx.loopEngine->getLoopInfo();
        expectEquals (info2.outSamples - info2.inSamples, (int64_t) 128);

        // Second halve: should be no-op (128 / 2 = 64 < 128)
        ctx.loopEngine->loopHalve();
        auto info3 = ctx.loopEngine->getLoopInfo();
        expectEquals (info3.outSamples - info3.inSamples, (int64_t) 128);
    }

    // -----------------------------------------------------------------------
    // 11. Loop double maximum: won't exceed track length
    // -----------------------------------------------------------------------
    void testLoopDoubleMaximum()
    {
        beginTest ("Loop double maximum: won't exceed track length");
        LoopTestContext ctx;
        // totalSamples = 7938000, make loop near end of track
        int64_t nearEnd = 7938000 - 500;
        ctx.audioState.playheadPosition.store (nearEnd, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();
        ctx.audioState.playheadPosition.store (nearEnd + 256, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info = ctx.loopEngine->getLoopInfo();
        expect (info.active);

        // loopDouble would produce 512 samples, but loopIn + 512 > totalSamples
        // maxLen = totalSamples - loopIn = 500
        // newLen = 512 > 500 → no-op
        ctx.loopEngine->loopDouble();
        auto info2 = ctx.loopEngine->getLoopInfo();
        expectEquals (info2.outSamples - info2.inSamples, (int64_t) 256);
    }

    // -----------------------------------------------------------------------
    // 12. Loop exit: toggleLoop deactivates, retains boundaries
    // -----------------------------------------------------------------------
    void testLoopExit()
    {
        beginTest ("Loop exit: toggleLoop deactivates, retains boundaries");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();
        int64_t savedIn  = info.inSamples;
        int64_t savedOut = info.outSamples;
        expect (info.active);

        ctx.loopEngine->toggleLoop();
        auto info2 = ctx.loopEngine->getLoopInfo();
        expect (! info2.active, "Loop should be deactivated");
        expectEquals (info2.inSamples, savedIn);
        expectEquals (info2.outSamples, savedOut);
    }

    // -----------------------------------------------------------------------
    // 13. Re-loop: reactivates stored loop
    // -----------------------------------------------------------------------
    void testReLoop()
    {
        beginTest ("Re-loop: reactivates stored loop");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        ctx.loopEngine->autoLoop (4.0f);
        int64_t savedIn  = ctx.loopEngine->getLoopInfo().inSamples;
        int64_t savedOut = ctx.loopEngine->getLoopInfo().outSamples;

        ctx.loopEngine->toggleLoop();
        expect (! ctx.loopEngine->getLoopInfo().active);

        ctx.loopEngine->reLoop();
        auto info = ctx.loopEngine->getLoopInfo();
        expect (info.active, "Loop should be reactivated");
        expectEquals (info.inSamples, savedIn);
        expectEquals (info.outSamples, savedOut);
    }

    // -----------------------------------------------------------------------
    // 14. Re-loop with no stored loop: no-op
    // -----------------------------------------------------------------------
    void testReLoopNoOp()
    {
        beginTest ("Re-loop with no stored loop: no-op");
        LoopTestContext ctx;

        ctx.loopEngine->reLoop();
        auto info = ctx.loopEngine->getLoopInfo();
        expect (! info.active, "Loop should remain inactive");
        expectEquals (info.inSamples, (int64_t) -1);
        expectEquals (info.outSamples, (int64_t) -1);
    }

    // -----------------------------------------------------------------------
    // 15. Quantize integration: auto-loop snaps to beats when quantize enabled
    // -----------------------------------------------------------------------
    void testQuantizeAutoLoop()
    {
        beginTest ("Quantize integration: auto-loop snaps to beats");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);
        ctx.deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);

        // Playhead at 500000, not on a beat. With quantize, loopIn should snap
        // to the previous beat before 500000.
        double interval = 44100.0 * 60.0 / 128.0; // 20671.875
        int64_t expectedIn = QuantizeService::getPreviousBeatBefore (500000, 0, interval);

        ctx.loopEngine->autoLoop (4.0f);
        auto info = ctx.loopEngine->getLoopInfo();

        expect (info.active);
        expectEquals (info.inSamples, expectedIn);
    }

    // -----------------------------------------------------------------------
    // 16. Quantize integration: manual loop snaps when quantize enabled
    // -----------------------------------------------------------------------
    void testQuantizeManualLoop()
    {
        beginTest ("Quantize integration: manual loop snaps when quantize enabled");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);
        ctx.deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);

        double interval = 44100.0 * 60.0 / 128.0;

        // Set loop in at playhead 100000 (not on a beat)
        ctx.audioState.playheadPosition.store (100000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopIn();

        // Set loop out at playhead 200000
        ctx.audioState.playheadPosition.store (200000, std::memory_order_relaxed);
        ctx.loopEngine->setLoopOut();

        auto info = ctx.loopEngine->getLoopInfo();
        expect (info.active);

        // When quantize is enabled, the BeatGridData::getNearestBeat is used for manual loop
        BeatGridData bgTemp;
        bgTemp.anchorSample = 0;
        bgTemp.beatIntervalSamples = interval;
        int64_t snappedIn  = bgTemp.getNearestBeat (100000);
        int64_t snappedOut = bgTemp.getNearestBeat (200000);

        expectEquals (info.inSamples, snappedIn);
        expectEquals (info.outSamples, snappedOut);
    }

    // -----------------------------------------------------------------------
    // 17. Loop state in ValueTree: loopInSamples, loopOutSamples, loopActive
    // -----------------------------------------------------------------------
    void testLoopStateInValueTree()
    {
        beginTest ("Loop state in ValueTree: properties update correctly");
        LoopTestContext ctx;
        ctx.setBeatGrid (128.0);

        auto loopNode = ctx.loopNode();
        expectEquals (static_cast<int64_t> (loopNode.getProperty (IDs::loopIn)), (int64_t) -1);
        expectEquals (static_cast<int64_t> (loopNode.getProperty (IDs::loopOut)), (int64_t) -1);
        expect (! static_cast<bool> (loopNode.getProperty (IDs::active)));

        ctx.loopEngine->autoLoop (4.0f);

        expect (static_cast<bool> (loopNode.getProperty (IDs::active)));
        expect (static_cast<int64_t> (loopNode.getProperty (IDs::loopIn)) >= 0);
        expect (static_cast<int64_t> (loopNode.getProperty (IDs::loopOut)) >
                static_cast<int64_t> (loopNode.getProperty (IDs::loopIn)));
    }
};
static LoopEngineTests loopEngineTestsInstance;

// =============================================================================
// 2. LoopControlComponent Tests
// =============================================================================
class LoopControlComponentTests : public juce::UnitTest
{
public:
    LoopControlComponentTests() : juce::UnitTest ("Loop Control Component", "Sonik") {}

    void runTest() override
    {
        testConstructsWithoutCrash();
        testPaintsWithoutCrash();
        testControlsGrayedWhenEmpty();
        testAutoLoopButtonHighlight();
    }

private:
    // -----------------------------------------------------------------------
    // 18. Component constructs without crashing
    // -----------------------------------------------------------------------
    void testConstructsWithoutCrash()
    {
        beginTest ("LoopControlComponent constructs without crashing");
        auto deck = createLoopTestDeckTree ("A");
        LoopControlComponent comp (deck);
        expect (true, "Construction succeeded");
    }

    // -----------------------------------------------------------------------
    // 19. Component renders/paints without crashing
    // -----------------------------------------------------------------------
    void testPaintsWithoutCrash()
    {
        beginTest ("LoopControlComponent renders/paints without crashing");
        auto deck = createLoopTestDeckTree ("A");
        LoopControlComponent comp (deck);
        comp.setSize (600, 30);

        juce::Image img (juce::Image::ARGB, 600, 30, true);
        juce::Graphics g (img);
        comp.paint (g);
        expect (true, "Paint succeeded without crash");
    }

    // -----------------------------------------------------------------------
    // 20. All buttons grayed out when deck is Empty
    // -----------------------------------------------------------------------
    void testControlsGrayedWhenEmpty()
    {
        beginTest ("All buttons grayed out when deck is Empty");
        auto deck = createLoopTestDeckTree ("A");
        // Deck is "empty" by default
        LoopControlComponent comp (deck);
        comp.setSize (600, 30);

        // Verify paint renders the grayed-out state without crash
        juce::Image img (juce::Image::ARGB, 600, 30, true);
        juce::Graphics g (img);
        comp.paint (g);

        // Verify the LoopEngine also ignores commands on empty deck
        juce::File dbFile = juce::File::createTempFile ("loop_empty_test.db");
        TrackDatabase db (dbFile);
        juce::ValueTree rootState (IDs::SonikState);
        AudioEngine engine (rootState);
        LoopEngine le (deck, engine, "A", db);
        le.autoLoop (4.0f);
        auto info = le.getLoopInfo();
        expect (! info.active, "Auto-loop should not activate on empty deck");
        dbFile.deleteFile();
    }

    // -----------------------------------------------------------------------
    // 21. Auto-loop button highlight when active
    // -----------------------------------------------------------------------
    void testAutoLoopButtonHighlight()
    {
        beginTest ("Auto-loop button highlight when active");
        auto deck = createLoopTestDeckTree ("A");
        setLoopDeckLoaded (deck);
        LoopControlComponent comp (deck);
        comp.setSize (600, 30);

        // Simulate loop activation via ValueTree
        auto loopNode = deck.getChildWithName (IDs::Loop);
        loopNode.setProperty (IDs::active, true, nullptr);
        loopNode.setProperty (IDs::loopIn, (int64_t) 100000, nullptr);
        loopNode.setProperty (IDs::loopOut, (int64_t) 200000, nullptr);

        comp.setActiveAutoLoopBeats (4.0f);

        // Paint again — should not crash and should render the highlight
        juce::Image img (juce::Image::ARGB, 600, 30, true);
        juce::Graphics g (img);
        comp.paint (g);
        expect (true, "Paint with active auto-loop highlight succeeded");
    }
};
static LoopControlComponentTests loopControlComponentTestsInstance;

// =============================================================================
// 3. AudioStateSync Loop Tests
// =============================================================================
class AudioStateSyncLoopTests : public juce::UnitTest
{
public:
    AudioStateSyncLoopTests() : juce::UnitTest ("AudioStateSync Loop", "Sonik") {}

    void runTest() override
    {
        testLoopInSync();
        testLoopOutSync();
        testLoopActiveSync();
    }

private:
    // -----------------------------------------------------------------------
    // 22. loopInSamples syncs from ValueTree to atomic
    // -----------------------------------------------------------------------
    void testLoopInSync()
    {
        beginTest ("loopInSamples syncs from ValueTree to atomic");
        auto deck = createLoopTestDeckTree ("A");
        DeckAudioState state;
        AudioStateSync sync (deck, state);

        // Initial: -1
        expectEquals (state.loopInSamples.load (std::memory_order_relaxed), (int64_t) -1);

        // Change in ValueTree
        auto loopNode = deck.getChildWithName (IDs::Loop);
        loopNode.setProperty (IDs::loopIn, (int64_t) 44100, nullptr);
        expectEquals (state.loopInSamples.load (std::memory_order_relaxed), (int64_t) 44100);
    }

    // -----------------------------------------------------------------------
    // 23. loopOutSamples syncs from ValueTree to atomic
    // -----------------------------------------------------------------------
    void testLoopOutSync()
    {
        beginTest ("loopOutSamples syncs from ValueTree to atomic");
        auto deck = createLoopTestDeckTree ("A");
        DeckAudioState state;
        AudioStateSync sync (deck, state);

        expectEquals (state.loopOutSamples.load (std::memory_order_relaxed), (int64_t) -1);

        auto loopNode = deck.getChildWithName (IDs::Loop);
        loopNode.setProperty (IDs::loopOut, (int64_t) 88200, nullptr);
        expectEquals (state.loopOutSamples.load (std::memory_order_relaxed), (int64_t) 88200);
    }

    // -----------------------------------------------------------------------
    // 24. loopActive syncs from ValueTree to atomic
    // -----------------------------------------------------------------------
    void testLoopActiveSync()
    {
        beginTest ("loopActive syncs from ValueTree to atomic");
        auto deck = createLoopTestDeckTree ("A");
        DeckAudioState state;
        AudioStateSync sync (deck, state);

        expect (! state.loopActive.load (std::memory_order_relaxed));

        auto loopNode = deck.getChildWithName (IDs::Loop);
        loopNode.setProperty (IDs::active, true, nullptr);
        expect (state.loopActive.load (std::memory_order_relaxed));
    }
};
static AudioStateSyncLoopTests audioStateSyncLoopTestsInstance;

// =============================================================================
// 4. Waveform Loop Overlay Tests (via DeckAudioState atomics)
// =============================================================================
class WaveformLoopOverlayTests : public juce::UnitTest
{
public:
    WaveformLoopOverlayTests() : juce::UnitTest ("Waveform Loop Overlay", "Sonik") {}

    void runTest() override
    {
        testDetailWaveformReadsLoopState();
        testOverviewWaveformReadsLoopState();
    }

private:
    // -----------------------------------------------------------------------
    // 25. DeckAudioState loop atomics store data for DetailWaveform
    // -----------------------------------------------------------------------
    void testDetailWaveformReadsLoopState()
    {
        beginTest ("DeckAudioState stores loop data for DetailWaveform");
        DeckAudioState state;

        state.loopInSamples.store (100000, std::memory_order_relaxed);
        state.loopOutSamples.store (200000, std::memory_order_relaxed);
        state.loopActive.store (true, std::memory_order_relaxed);

        expectEquals (state.loopInSamples.load (std::memory_order_relaxed), (int64_t) 100000);
        expectEquals (state.loopOutSamples.load (std::memory_order_relaxed), (int64_t) 200000);
        expect (state.loopActive.load (std::memory_order_relaxed));
    }

    // -----------------------------------------------------------------------
    // 26. DeckAudioState loop atomics store data for OverviewWaveform
    // -----------------------------------------------------------------------
    void testOverviewWaveformReadsLoopState()
    {
        beginTest ("DeckAudioState stores loop data for OverviewWaveform");
        DeckAudioState state;

        // Default: no loop
        expectEquals (state.loopInSamples.load (std::memory_order_relaxed), (int64_t) -1);
        expectEquals (state.loopOutSamples.load (std::memory_order_relaxed), (int64_t) -1);
        expect (! state.loopActive.load (std::memory_order_relaxed));

        // Set loop region
        state.loopInSamples.store (50000, std::memory_order_relaxed);
        state.loopOutSamples.store (150000, std::memory_order_relaxed);
        state.loopActive.store (false, std::memory_order_relaxed);

        expectEquals (state.loopInSamples.load (std::memory_order_relaxed), (int64_t) 50000);
        expectEquals (state.loopOutSamples.load (std::memory_order_relaxed), (int64_t) 150000);
        expect (! state.loopActive.load (std::memory_order_relaxed));
    }
};
static WaveformLoopOverlayTests waveformLoopOverlayTestsInstance;
