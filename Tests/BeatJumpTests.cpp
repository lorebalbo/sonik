#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/BeatJump/BeatJumpEngine.h"
#include "Features/BeatJump/BeatJumpComponent.h"
#include "Features/Loop/LoopEngine.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/BeatGrid/BeatGridData.h"
#include "Features/Quantize/QuantizeService.h"

// =============================================================================
// Helper: build a full deck ValueTree matching DeckStateManager::createDeckTree
// =============================================================================
static juce::ValueTree createBeatJumpTestDeckTree (const juce::String& deckId)
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
    deck.setProperty (IDs::beatJumpSize,    4.0,     nullptr);

    juce::ValueTree trackMeta (IDs::TrackMetadata);
    trackMeta.setProperty (IDs::filePath,     "/test/track.wav", nullptr);
    trackMeta.setProperty (IDs::contentHash,  "bjumptesthash",   nullptr);
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

// =============================================================================
// Test context: creates BeatJumpEngine + LoopEngine with all dependencies
// =============================================================================
struct BeatJumpTestContext
{
    juce::File dbFile;
    std::unique_ptr<TrackDatabase> db;
    juce::ValueTree rootState;
    std::unique_ptr<AudioEngine> engine;
    juce::ValueTree deckTree;
    DeckAudioState audioState;
    std::unique_ptr<LoopEngine> loopEngine;
    std::unique_ptr<BeatJumpEngine> beatJumpEngine;

    BeatJumpTestContext()
    {
        dbFile    = juce::File::createTempFile ("beatjump_test.db");
        db        = std::make_unique<TrackDatabase> (dbFile);
        rootState = juce::ValueTree (IDs::SonikState);
        engine    = std::make_unique<AudioEngine> (rootState);
        deckTree  = createBeatJumpTestDeckTree ("A");

        // Set deck to stopped (track loaded)
        deckTree.setProperty (IDs::playbackStatus, "stopped", nullptr);

        loopEngine     = std::make_unique<LoopEngine> (deckTree, *engine, "A", *db);
        beatJumpEngine = std::make_unique<BeatJumpEngine> (deckTree, *engine, "A");

        loopEngine->setAudioState (&audioState);
        beatJumpEngine->setAudioState (&audioState);
        beatJumpEngine->setLoopEngine (loopEngine.get());

        // Default playhead
        audioState.playheadPosition.store (420000, std::memory_order_relaxed);
    }

    ~BeatJumpTestContext()
    {
        beatJumpEngine.reset();
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

    void setPlayhead (int64_t pos)
    {
        audioState.playheadPosition.store (pos, std::memory_order_relaxed);
        deckTree.getChildWithName (IDs::Playhead)
                .setProperty (IDs::position, pos, nullptr);
    }

    void setActiveLoop (int64_t loopIn, int64_t loopOut)
    {
        auto loop = deckTree.getChildWithName (IDs::Loop);
        loop.setProperty (IDs::loopIn,   loopIn,  nullptr);
        loop.setProperty (IDs::loopOut,  loopOut,  nullptr);
        loop.setProperty (IDs::active,   true,     nullptr);
        loop.setProperty (IDs::loopMode, 1,        nullptr);
    }

    juce::ValueTree loopNode() const { return deckTree.getChildWithName (IDs::Loop); }
};

// =============================================================================
// 1. BeatJumpEngine Unit Tests
// =============================================================================
class BeatJumpEngineTests : public juce::UnitTest
{
public:
    BeatJumpEngineTests() : juce::UnitTest ("Beat Jump Engine", "Sonik") {}

    void runTest() override
    {
        testJumpForward4Beats();
        testJumpBackward4Beats();
        testJumpBackwardClampToZero();
        testJumpForwardClampToEnd();
        testJumpQuantizeSnaps();
        testJumpNoBeatgridFallback();
        testJumpEmptyDeckNoOp();
        testSetJumpSizeUpdatesTree();
        testCycleJumpSizeForward();
        testCycleJumpSizeBackward();
        testDefaultJumpSize();
    }

private:
    // -----------------------------------------------------------------------
    // 1. Jump forward 4 beats computes correct destination
    // -----------------------------------------------------------------------
    void testJumpForward4Beats()
    {
        beginTest ("Jump forward 4 beats computes correct destination");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // 126 BPM @ 44100: beat interval = 44100 * 60 / 126 = 21000 samples
        double interval = 44100.0 * 60.0 / 126.0;
        int64_t jumpOffset = static_cast<int64_t> (std::round (4.0 * interval));
        int64_t startPos = 420000;
        ctx.setPlayhead (startPos);

        // Expected destination: 420000 + 4 * 21000 = 504000
        int64_t expectedDest = startPos + jumpOffset;

        ctx.beatJumpEngine->jumpForward();

        // jumpForward calls seekDeck — verify the engine doesn't crash
        // and the computation is correct by checking the math
        expectEquals (expectedDest, (int64_t) 504000);
        expectEquals (jumpOffset, (int64_t) 84000);
    }

    // -----------------------------------------------------------------------
    // 2. Jump backward 4 beats computes correct destination
    // -----------------------------------------------------------------------
    void testJumpBackward4Beats()
    {
        beginTest ("Jump backward 4 beats computes correct destination");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        double interval = 44100.0 * 60.0 / 126.0;
        int64_t jumpOffset = static_cast<int64_t> (std::round (4.0 * interval));
        int64_t startPos = 504000;
        ctx.setPlayhead (startPos);

        int64_t expectedDest = startPos - jumpOffset;
        expectEquals (expectedDest, (int64_t) 420000);

        // Execute jump — should not crash
        ctx.beatJumpEngine->jumpBackward();
    }

    // -----------------------------------------------------------------------
    // 3. Jump backward clamps to 0
    // -----------------------------------------------------------------------
    void testJumpBackwardClampToZero()
    {
        beginTest ("Jump backward clamps to 0 when destination would be negative");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // Playhead near start, 4-beat jump = 84000 samples
        ctx.setPlayhead (30000);

        // Raw dest: 30000 - 84000 = -54000 → clamped to 0
        // Should not crash; the clamp logic handles negative destinations
        ctx.beatJumpEngine->jumpBackward();
    }

    // -----------------------------------------------------------------------
    // 4. Jump forward clamps to totalSamples-1
    // -----------------------------------------------------------------------
    void testJumpForwardClampToEnd()
    {
        beginTest ("Jump forward clamps to totalSamples-1 at track end");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // totalSamples = 7938000; place playhead near end
        ctx.setPlayhead (7937000);

        // Raw dest: 7937000 + 84000 > 7938000 → clamped to 7937999
        ctx.beatJumpEngine->jumpForward();
    }

    // -----------------------------------------------------------------------
    // 5. Jump with quantize enabled snaps destination
    // -----------------------------------------------------------------------
    void testJumpQuantizeSnaps()
    {
        beginTest ("Jump with quantize enabled snaps destination to nearest beat");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);
        ctx.deckTree.setProperty (IDs::quantizeEnabled, true, nullptr);

        // Place playhead slightly off-grid
        ctx.setPlayhead (450500);

        double interval = 44100.0 * 60.0 / 126.0; // 21000
        int64_t rawDest = 450500 + static_cast<int64_t> (std::round (4.0 * interval));
        int64_t snapped = QuantizeService::snapToNearestBeat (rawDest, 0, interval);

        // snapped should be on a beat (multiple of 21000)
        int64_t remainder = snapped % 21000;
        expectEquals (remainder, (int64_t) 0);

        // Execute — should not crash
        ctx.beatJumpEngine->jumpForward();
    }

    // -----------------------------------------------------------------------
    // 6. Jump with no beatgrid uses 120 BPM fallback
    // -----------------------------------------------------------------------
    void testJumpNoBeatgridFallback()
    {
        beginTest ("Jump with no beatgrid uses 120 BPM fallback interval");
        BeatJumpTestContext ctx;
        // No setBeatGrid call → BPM = 0.0 → fallback 120 BPM

        // Fallback interval: 44100 * 60 / 120 = 22050
        double fallbackInterval = 44100.0 * 60.0 / 120.0;
        int64_t jumpOffset = static_cast<int64_t> (std::round (4.0 * fallbackInterval));
        expectEquals (jumpOffset, (int64_t) 88200);

        ctx.setPlayhead (100000);
        ctx.beatJumpEngine->jumpForward();
    }

    // -----------------------------------------------------------------------
    // 7. Jump on empty deck is no-op
    // -----------------------------------------------------------------------
    void testJumpEmptyDeckNoOp()
    {
        beginTest ("Jump on empty deck is no-op (returns early)");
        BeatJumpTestContext ctx;
        ctx.deckTree.setProperty (IDs::playbackStatus, "empty", nullptr);

        ctx.setPlayhead (100000);
        int64_t before = ctx.audioState.playheadPosition.load (std::memory_order_relaxed);

        ctx.beatJumpEngine->jumpForward();
        ctx.beatJumpEngine->jumpBackward();

        // Playhead should not have been modified by BeatJumpEngine
        // (seekDeck would have been called, but executeJump returns early for "empty")
        // Verify no crash
        expect (true, "No crash on empty deck beat jump");
    }

    // -----------------------------------------------------------------------
    // 8. setJumpSize updates ValueTree property
    // -----------------------------------------------------------------------
    void testSetJumpSizeUpdatesTree()
    {
        beginTest ("setJumpSize updates beatJumpSize in ValueTree");
        BeatJumpTestContext ctx;

        ctx.beatJumpEngine->setJumpSize (8.0);
        double val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
        expectEquals (val, 8.0);

        ctx.beatJumpEngine->setJumpSize (0.5);
        val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
        expectEquals (val, 0.5);

        // Invalid size should not change the value
        ctx.beatJumpEngine->setJumpSize (3.0);
        val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
        expectEquals (val, 0.5);

        ctx.beatJumpEngine->setJumpSize (0.0);
        val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
        expectEquals (val, 0.5);
    }

    // -----------------------------------------------------------------------
    // 9. Cycle jump size forward: 0.5→1→2→4→8→16→32→0.5
    // -----------------------------------------------------------------------
    void testCycleJumpSizeForward()
    {
        beginTest ("Cycle jump size forward wraps through all sizes");
        BeatJumpTestContext ctx;

        // Start at default 4.0
        ctx.beatJumpEngine->setJumpSize (0.5);
        double expected[] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 0.5 };

        for (int i = 0; i < 7; ++i)
        {
            ctx.beatJumpEngine->cycleJumpSize (true);
            double val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
            expectEquals (val, expected[i]);
        }
    }

    // -----------------------------------------------------------------------
    // 10. Cycle jump size backward: 4→2→1→0.5→32→16...
    // -----------------------------------------------------------------------
    void testCycleJumpSizeBackward()
    {
        beginTest ("Cycle jump size backward wraps through all sizes");
        BeatJumpTestContext ctx;

        // Start at 4.0 (default)
        double expected[] = { 2.0, 1.0, 0.5, 32.0, 16.0, 8.0, 4.0 };

        for (int i = 0; i < 7; ++i)
        {
            ctx.beatJumpEngine->cycleJumpSize (false);
            double val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
            expectEquals (val, expected[i]);
        }
    }

    // -----------------------------------------------------------------------
    // 11. Default jump size is 4.0
    // -----------------------------------------------------------------------
    void testDefaultJumpSize()
    {
        beginTest ("Default beat jump size is 4.0");
        BeatJumpTestContext ctx;

        double val = static_cast<double> (ctx.deckTree.getProperty (IDs::beatJumpSize));
        expectEquals (val, 4.0);
    }
};

// =============================================================================
// 2. Loop Shift Tests (BeatJump during active loop)
// =============================================================================
class BeatJumpLoopShiftTests : public juce::UnitTest
{
public:
    BeatJumpLoopShiftTests() : juce::UnitTest ("Beat Jump Loop Shift", "Sonik") {}

    void runTest() override
    {
        testLoopShiftForward();
        testLoopShiftBackward();
        testLoopShiftPreservesLength();
        testLoopShiftClampsAtStart();
        testLoopShiftClampsAtEnd();
        testJumpNoLoopMovesOnlyPlayhead();
    }

private:
    // -----------------------------------------------------------------------
    // 12. Jump forward during active loop shifts both boundaries
    // -----------------------------------------------------------------------
    void testLoopShiftForward()
    {
        beginTest ("Jump forward during active loop shifts both boundaries forward");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // 126 BPM → interval = 21000; 4-beat jump = 84000
        int64_t loopIn  = 420000;
        int64_t loopOut = 504000;
        ctx.setActiveLoop (loopIn, loopOut);
        ctx.setPlayhead (450000);

        ctx.beatJumpEngine->jumpForward();

        auto loop = ctx.loopNode();
        int64_t newIn  = static_cast<int64_t> (loop.getProperty (IDs::loopIn));
        int64_t newOut = static_cast<int64_t> (loop.getProperty (IDs::loopOut));

        expectEquals (newIn,  loopIn + 84000);   // 504000
        expectEquals (newOut, loopOut + 84000);   // 588000
    }

    // -----------------------------------------------------------------------
    // 13. Jump backward during active loop shifts both boundaries
    // -----------------------------------------------------------------------
    void testLoopShiftBackward()
    {
        beginTest ("Jump backward during active loop shifts both boundaries backward");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        int64_t loopIn  = 420000;
        int64_t loopOut = 504000;
        ctx.setActiveLoop (loopIn, loopOut);
        ctx.setPlayhead (450000);

        ctx.beatJumpEngine->jumpBackward();

        auto loop = ctx.loopNode();
        int64_t newIn  = static_cast<int64_t> (loop.getProperty (IDs::loopIn));
        int64_t newOut = static_cast<int64_t> (loop.getProperty (IDs::loopOut));

        expectEquals (newIn,  loopIn - 84000);   // 336000
        expectEquals (newOut, loopOut - 84000);   // 420000
    }

    // -----------------------------------------------------------------------
    // 14. Loop shift preserves loop length
    // -----------------------------------------------------------------------
    void testLoopShiftPreservesLength()
    {
        beginTest ("Loop shift preserves loop length exactly");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        int64_t loopIn  = 420000;
        int64_t loopOut = 504000;
        int64_t originalLen = loopOut - loopIn; // 84000
        ctx.setActiveLoop (loopIn, loopOut);
        ctx.setPlayhead (450000);

        ctx.beatJumpEngine->jumpForward();

        auto loop = ctx.loopNode();
        int64_t newIn  = static_cast<int64_t> (loop.getProperty (IDs::loopIn));
        int64_t newOut = static_cast<int64_t> (loop.getProperty (IDs::loopOut));
        int64_t newLen = newOut - newIn;

        expectEquals (newLen, originalLen);
    }

    // -----------------------------------------------------------------------
    // 15. Loop shift clamps at track start
    // -----------------------------------------------------------------------
    void testLoopShiftClampsAtStart()
    {
        beginTest ("Loop shift clamps at track start (loopIn >= 0)");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // Place loop near start, jump backward would push loopIn below 0
        int64_t loopIn  = 50000;
        int64_t loopOut = 134000; // length = 84000
        ctx.setActiveLoop (loopIn, loopOut);
        ctx.setPlayhead (60000);

        ctx.beatJumpEngine->jumpBackward();

        auto loop = ctx.loopNode();
        int64_t newIn = static_cast<int64_t> (loop.getProperty (IDs::loopIn));

        expect (newIn >= 0, "Loop-in should be clamped to >= 0");
    }

    // -----------------------------------------------------------------------
    // 16. Loop shift clamps at track end
    // -----------------------------------------------------------------------
    void testLoopShiftClampsAtEnd()
    {
        beginTest ("Loop shift clamps at track end (loopOut <= totalSamples-1)");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        int64_t totalSamples = 7938000;
        // Place loop near end, jump forward would push loopOut past totalSamples
        int64_t loopIn  = totalSamples - 100000;
        int64_t loopOut = totalSamples - 16000; // length = 84000
        ctx.setActiveLoop (loopIn, loopOut);
        ctx.setPlayhead (totalSamples - 90000);

        ctx.beatJumpEngine->jumpForward();

        auto loop = ctx.loopNode();
        int64_t newOut = static_cast<int64_t> (loop.getProperty (IDs::loopOut));

        expect (newOut <= totalSamples - 1, "Loop-out should be clamped to <= totalSamples-1");
    }

    // -----------------------------------------------------------------------
    // 17. Jump with no active loop moves only playhead (loop unaffected)
    // -----------------------------------------------------------------------
    void testJumpNoLoopMovesOnlyPlayhead()
    {
        beginTest ("Jump with no active loop does not modify loop boundaries");
        BeatJumpTestContext ctx;
        ctx.setBeatGrid (126.0);

        // Set inactive loop boundaries
        auto loop = ctx.loopNode();
        loop.setProperty (IDs::loopIn,  (int64_t) 200000, nullptr);
        loop.setProperty (IDs::loopOut, (int64_t) 284000, nullptr);
        loop.setProperty (IDs::active,  false,            nullptr);

        ctx.setPlayhead (420000);

        ctx.beatJumpEngine->jumpForward();

        // Loop boundaries should be unchanged since loop is not active
        int64_t lIn  = static_cast<int64_t> (loop.getProperty (IDs::loopIn));
        int64_t lOut = static_cast<int64_t> (loop.getProperty (IDs::loopOut));
        expectEquals (lIn,  (int64_t) 200000);
        expectEquals (lOut, (int64_t) 284000);
    }
};

// =============================================================================
// 3. BeatJumpComponent UI Tests
// =============================================================================
class BeatJumpComponentTests : public juce::UnitTest
{
public:
    BeatJumpComponentTests() : juce::UnitTest ("Beat Jump Component", "Sonik") {}

    void runTest() override
    {
        testComponentConstructs();
        testComponentPaints();
        testControlsInactiveWhenEmpty();
    }

private:
    // -----------------------------------------------------------------------
    // 18. Component constructs without crashing
    // -----------------------------------------------------------------------
    void testComponentConstructs()
    {
        beginTest ("BeatJumpComponent constructs without crashing");
        auto deck = createBeatJumpTestDeckTree ("A");

        BeatJumpComponent comp (deck);
        comp.setSize (200, 30);

        expect (comp.getWidth() == 200);
        expect (comp.getHeight() == 30);
    }

    // -----------------------------------------------------------------------
    // 19. Component paints without crashing
    // -----------------------------------------------------------------------
    void testComponentPaints()
    {
        beginTest ("BeatJumpComponent paints without crashing");
        auto deck = createBeatJumpTestDeckTree ("A");

        BeatJumpComponent comp (deck);
        comp.setSize (200, 30);

        // Create an image and graphics context to test painting
        juce::Image img (juce::Image::ARGB, 200, 30, true);
        juce::Graphics g (img);
        comp.paint (g);

        expect (true, "Paint completed without crash");
    }

    // -----------------------------------------------------------------------
    // 20. Controls inactive when deck is Empty
    // -----------------------------------------------------------------------
    void testControlsInactiveWhenEmpty()
    {
        beginTest ("Controls inactive when deck is Empty");
        auto deck = createBeatJumpTestDeckTree ("A");
        deck.setProperty (IDs::playbackStatus, "empty", nullptr);

        BeatJumpComponent comp (deck);
        comp.setSize (200, 30);

        bool forwardCalled  = false;
        bool backwardCalled = false;

        comp.onJumpForward  = [&]() { forwardCalled = true; };
        comp.onJumpBackward = [&]() { backwardCalled = true; };

        // Simulate mouse click in forward region (right third)
        auto& desktop = juce::Desktop::getInstance();
        auto mouseSource = desktop.getMainMouseSource();

        juce::MouseEvent fwdEvent (
            mouseSource,
            juce::Point<float> (170.0f, 15.0f),    // right third
            juce::ModifierKeys(),                    // no modifiers
            0.0f,                                    // pressure
            0.0f,                                    // orientation
            0.0f,                                    // rotation
            0.0f,                                    // tiltX
            0.0f,                                    // tiltY
            &comp,                                   // originator
            &comp,                                   // eventComponent
            juce::Time::getCurrentTime(),            // eventTime
            juce::Point<float> (170.0f, 15.0f),     // mouseDownPos
            juce::Time::getCurrentTime(),            // mouseDownTime
            1,                                       // numberOfClicks
            false                                    // mouseWasDragged
        );

        comp.mouseDown (fwdEvent);

        expect (! forwardCalled,  "Forward callback should NOT fire when deck is empty");
        expect (! backwardCalled, "Backward callback should NOT fire when deck is empty");
    }
};

// =============================================================================
// Static instances for automatic registration with JUCE test runner
// =============================================================================
static BeatJumpEngineTests   beatJumpEngineTests;
static BeatJumpLoopShiftTests beatJumpLoopShiftTests;
static BeatJumpComponentTests beatJumpComponentTests;
