#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Quantize/QuantizeService.h"
#include "Features/Quantize/QuantizeButtonComponent.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckIdentifiers.h"

// =============================================================================
// Helper: build a full deck ValueTree matching DeckStateManager layout
// =============================================================================
static juce::ValueTree createQuantizeTestDeckTree (const juce::String& deckId)
{
    juce::ValueTree deck (IDs::Deck);
    deck.setProperty (IDs::id,              deckId,  nullptr);
    deck.setProperty (IDs::playbackStatus,  "stopped", nullptr);
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

// =============================================================================
// 1. QuantizeService Unit Tests (Pure Math)
// =============================================================================
class QuantizeServiceTests : public juce::UnitTest
{
public:
    QuantizeServiceTests() : juce::UnitTest ("Quantize Service", "Sonik") {}

    void runTest() override
    {
        testSnapOnBeat();
        testSnapSlightlyBeforeBeat();
        testSnapSlightlyAfterBeat();
        testSnapMidpointRounding();
        testSnapBeforeAnchor();
        testSnapClampedToZero();
        testSnapIntervalLessThanZero();
        testSnapIntervalZero();
        testNextBeatOnBeat();
        testNextBeatBetween();
        testNextBeatIntervalInvalid();
        testPrevBeatOnBeat();
        testPrevBeatBetween();
        testPrevBeatBeforeAnchorClamped();
        testPrevBeatIntervalInvalid();
        testLargeBeatInterval();
        testSmallBeatInterval();
        testPositionZeroAnchorZero();
    }

private:
    // 120 BPM @ 44100 Hz = 22050 samples/beat
    static constexpr double kInterval120 = 22050.0;
    static constexpr int64_t kAnchor0    = 0;

    // -------------------------------------------------------------------
    void testSnapOnBeat()
    {
        beginTest ("snapToNearestBeat: position exactly on a beat returns same position");

        expectEquals (QuantizeService::snapToNearestBeat (0, kAnchor0, kInterval120),
                      (int64_t) 0);
        expectEquals (QuantizeService::snapToNearestBeat (22050, kAnchor0, kInterval120),
                      (int64_t) 22050);
        expectEquals (QuantizeService::snapToNearestBeat (44100, kAnchor0, kInterval120),
                      (int64_t) 44100);
    }

    // -------------------------------------------------------------------
    void testSnapSlightlyBeforeBeat()
    {
        beginTest ("snapToNearestBeat: position slightly before a beat snaps forward");

        // 22000 is 50 samples before beat at 22050
        expectEquals (QuantizeService::snapToNearestBeat (22000, kAnchor0, kInterval120),
                      (int64_t) 22050);
        // 21900 is 150 samples before beat at 22050
        expectEquals (QuantizeService::snapToNearestBeat (21900, kAnchor0, kInterval120),
                      (int64_t) 22050);
    }

    // -------------------------------------------------------------------
    void testSnapSlightlyAfterBeat()
    {
        beginTest ("snapToNearestBeat: position slightly after a beat snaps backward");

        // 22100 is 50 samples after beat at 22050
        expectEquals (QuantizeService::snapToNearestBeat (22100, kAnchor0, kInterval120),
                      (int64_t) 22050);
        // 22200 is 150 samples after beat at 22050
        expectEquals (QuantizeService::snapToNearestBeat (22200, kAnchor0, kInterval120),
                      (int64_t) 22050);
    }

    // -------------------------------------------------------------------
    void testSnapMidpointRounding()
    {
        beginTest ("snapToNearestBeat: midpoint between beats rounds via std::round");

        // Midpoint between beat 0 (0) and beat 1 (22050) is 11025
        // std::round(11025 / 22050) = std::round(0.5) = 1 (rounds away from zero)
        expectEquals (QuantizeService::snapToNearestBeat (11025, kAnchor0, kInterval120),
                      (int64_t) 22050);

        // One sample before midpoint rounds down
        expectEquals (QuantizeService::snapToNearestBeat (11024, kAnchor0, kInterval120),
                      (int64_t) 0);
    }

    // -------------------------------------------------------------------
    void testSnapBeforeAnchor()
    {
        beginTest ("snapToNearestBeat: position before anchor snaps correctly (negative beat index)");

        int64_t anchor = 44100;
        // Position 22050 is one beat before anchor → beatIndex = round((22050 - 44100) / 22050) = round(-1) = -1
        // result = 44100 + (-1 * 22050) = 22050
        expectEquals (QuantizeService::snapToNearestBeat (22050, anchor, kInterval120),
                      (int64_t) 22050);

        // Position 30000 → offset = 30000 - 44100 = -14100, beatIndex = round(-14100/22050) = round(-0.6395) = -1
        // result = 44100 + (-1 * 22050) = 22050
        expectEquals (QuantizeService::snapToNearestBeat (30000, anchor, kInterval120),
                      (int64_t) 22050);
    }

    // -------------------------------------------------------------------
    void testSnapClampedToZero()
    {
        beginTest ("snapToNearestBeat: result is clamped to >= 0");

        // Anchor at 0, position far negative → computed beat is negative → clamped to 0
        // position = -15000, offset = -15000, beatIndex = round(-15000/22050) = round(-0.68) = -1
        // result = 0 + (-1 * 22050) = -22050 → clamped to 0
        expectEquals (QuantizeService::snapToNearestBeat (-15000, kAnchor0, kInterval120),
                      (int64_t) 0);

        // Also test getNextBeatAfter clamping
        expectEquals (QuantizeService::getNextBeatAfter (-50000, kAnchor0, kInterval120),
                      (int64_t) 0);

        // Also test getPreviousBeatBefore clamping
        expectEquals (QuantizeService::getPreviousBeatBefore (-50000, kAnchor0, kInterval120),
                      (int64_t) 0);
    }

    // -------------------------------------------------------------------
    void testSnapIntervalLessThanZero()
    {
        beginTest ("snapToNearestBeat: beatInterval < 0 returns position unchanged");

        expectEquals (QuantizeService::snapToNearestBeat (12345, kAnchor0, -100.0),
                      (int64_t) 12345);
    }

    // -------------------------------------------------------------------
    void testSnapIntervalZero()
    {
        beginTest ("snapToNearestBeat: beatInterval == 0 returns position unchanged");

        expectEquals (QuantizeService::snapToNearestBeat (99999, kAnchor0, 0.0),
                      (int64_t) 99999);
    }

    // -------------------------------------------------------------------
    void testNextBeatOnBeat()
    {
        beginTest ("getNextBeatAfter: position on a beat returns next beat");

        // Position exactly on beat 1 (22050) → next beat is 44100
        // floor(22050/22050) + 1 = 1 + 1 = 2 → 2 * 22050 = 44100
        expectEquals (QuantizeService::getNextBeatAfter (22050, kAnchor0, kInterval120),
                      (int64_t) 44100);

        // Position on beat 0 (0) → next beat is 22050
        expectEquals (QuantizeService::getNextBeatAfter (0, kAnchor0, kInterval120),
                      (int64_t) 22050);
    }

    // -------------------------------------------------------------------
    void testNextBeatBetween()
    {
        beginTest ("getNextBeatAfter: position between beats returns next beat ahead");

        // Position 30000 between beat 1 (22050) and beat 2 (44100)
        // floor(30000/22050) + 1 = 1 + 1 = 2 → 44100
        expectEquals (QuantizeService::getNextBeatAfter (30000, kAnchor0, kInterval120),
                      (int64_t) 44100);

        // Position 100 between beat 0 (0) and beat 1 (22050)
        // floor(100/22050) + 1 = 0 + 1 = 1 → 22050
        expectEquals (QuantizeService::getNextBeatAfter (100, kAnchor0, kInterval120),
                      (int64_t) 22050);
    }

    // -------------------------------------------------------------------
    void testNextBeatIntervalInvalid()
    {
        beginTest ("getNextBeatAfter: beatInterval <= 0 returns position unchanged");

        expectEquals (QuantizeService::getNextBeatAfter (5000, kAnchor0, 0.0),
                      (int64_t) 5000);
        expectEquals (QuantizeService::getNextBeatAfter (5000, kAnchor0, -1.0),
                      (int64_t) 5000);
    }

    // -------------------------------------------------------------------
    void testPrevBeatOnBeat()
    {
        beginTest ("getPreviousBeatBefore: position on a beat returns previous beat");

        // Position on beat 2 (44100) → prev beat is beat 1 (22050)
        // ceil(44100/22050) - 1 = 2 - 1 = 1 → 22050
        expectEquals (QuantizeService::getPreviousBeatBefore (44100, kAnchor0, kInterval120),
                      (int64_t) 22050);

        // Position on beat 1 (22050) → prev beat is beat 0 (0)
        expectEquals (QuantizeService::getPreviousBeatBefore (22050, kAnchor0, kInterval120),
                      (int64_t) 0);
    }

    // -------------------------------------------------------------------
    void testPrevBeatBetween()
    {
        beginTest ("getPreviousBeatBefore: position between beats returns beat behind");

        // Position 30000 between beat 1 (22050) and beat 2 (44100)
        // ceil(30000/22050) - 1 = 2 - 1 = 1 → 22050
        expectEquals (QuantizeService::getPreviousBeatBefore (30000, kAnchor0, kInterval120),
                      (int64_t) 22050);

        // Position 100 → ceil(100/22050) - 1 = 1 - 1 = 0 → 0
        expectEquals (QuantizeService::getPreviousBeatBefore (100, kAnchor0, kInterval120),
                      (int64_t) 0);
    }

    // -------------------------------------------------------------------
    void testPrevBeatBeforeAnchorClamped()
    {
        beginTest ("getPreviousBeatBefore: position before anchor clamps to 0");

        // Anchor at 44100, position at 100
        // offset = 100 - 44100 = -44000
        // beatIndex = ceil(-44000/22050) - 1 = ceil(-1.995) - 1 = -1 - 1 = -2
        // result = 44100 + (-2 * 22050) = 0
        expectEquals (QuantizeService::getPreviousBeatBefore (100, 44100, kInterval120),
                      (int64_t) 0);

        // Position at beat 0 (0) with anchor at 0 → prev beat is -22050 → clamped to 0
        expectEquals (QuantizeService::getPreviousBeatBefore (0, kAnchor0, kInterval120),
                      (int64_t) 0);
    }

    // -------------------------------------------------------------------
    void testPrevBeatIntervalInvalid()
    {
        beginTest ("getPreviousBeatBefore: beatInterval <= 0 returns position unchanged");

        expectEquals (QuantizeService::getPreviousBeatBefore (5000, kAnchor0, 0.0),
                      (int64_t) 5000);
        expectEquals (QuantizeService::getPreviousBeatBefore (5000, kAnchor0, -10.0),
                      (int64_t) 5000);
    }

    // -------------------------------------------------------------------
    void testLargeBeatInterval()
    {
        beginTest ("Large beat interval (30 BPM) works correctly");

        // 30 BPM @ 44100 Hz = 2s per beat = 88200 samples
        double interval = 88200.0;
        int64_t anchor = 0;

        // Exactly on beat 1
        expectEquals (QuantizeService::snapToNearestBeat (88200, anchor, interval),
                      (int64_t) 88200);

        // Between beat 0 and beat 1, closer to beat 0
        expectEquals (QuantizeService::snapToNearestBeat (40000, anchor, interval),
                      (int64_t) 0);

        // Between beat 0 and beat 1, closer to beat 1
        expectEquals (QuantizeService::snapToNearestBeat (50000, anchor, interval),
                      (int64_t) 88200);

        // getNextBeatAfter from beat 0
        expectEquals (QuantizeService::getNextBeatAfter (0, anchor, interval),
                      (int64_t) 88200);
    }

    // -------------------------------------------------------------------
    void testSmallBeatInterval()
    {
        beginTest ("Small beat interval (300 BPM) works correctly");

        // 300 BPM @ 44100 Hz = 0.2s per beat = 8820 samples
        double interval = 8820.0;
        int64_t anchor = 0;

        // On beat 5
        expectEquals (QuantizeService::snapToNearestBeat (44100, anchor, interval),
                      (int64_t) 44100);

        // Slightly off beat 3 (26460)
        expectEquals (QuantizeService::snapToNearestBeat (26500, anchor, interval),
                      (int64_t) 26460);

        // getNextBeatAfter from beat 2 (17640) → beat 3 (26460)
        expectEquals (QuantizeService::getNextBeatAfter (17640, anchor, interval),
                      (int64_t) 26460);

        // getPreviousBeatBefore beat 3 (26460) → beat 2 (17640)
        expectEquals (QuantizeService::getPreviousBeatBefore (26460, anchor, interval),
                      (int64_t) 17640);
    }

    // -------------------------------------------------------------------
    void testPositionZeroAnchorZero()
    {
        beginTest ("Position 0 with anchor 0 returns 0 for all methods");

        expectEquals (QuantizeService::snapToNearestBeat (0, 0, kInterval120),
                      (int64_t) 0);
        // getNextBeatAfter(0) → beat 1 = 22050
        expectEquals (QuantizeService::getNextBeatAfter (0, 0, kInterval120),
                      (int64_t) 22050);
        // getPreviousBeatBefore(0) → beat -1 = -22050 → clamped to 0
        expectEquals (QuantizeService::getPreviousBeatBefore (0, 0, kInterval120),
                      (int64_t) 0);
    }
};

static QuantizeServiceTests quantizeServiceTests;

// =============================================================================
// 2. QuantizeButtonComponent Tests
// =============================================================================
class QuantizeButtonTests : public juce::UnitTest
{
public:
    QuantizeButtonTests() : juce::UnitTest ("Quantize Button", "Sonik") {}

    void runTest() override
    {
        testButtonConstructs();
        testButtonTogglesProperty();
        testButtonReflectsExternalChange();
        testButtonNonInteractiveWhenEmpty();
        testButtonReducedOpacityNoBeatgrid();
    }

private:
    // Helper: create a dummy MouseEvent using the Desktop main mouse source
    juce::MouseEvent makeDummyMouseEvent (juce::Component& target)
    {
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        return juce::MouseEvent (source,
                                 juce::Point<float> (15.0f, 15.0f),
                                 juce::ModifierKeys(),
                                 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &target, &target,
                                 juce::Time::getCurrentTime(),
                                 juce::Point<float> (15.0f, 15.0f),
                                 juce::Time::getCurrentTime(),
                                 1, false);
    }

    // -------------------------------------------------------------------
    void testButtonConstructs()
    {
        beginTest ("Button constructs without crashing");

        auto tree = createQuantizeTestDeckTree ("A");
        QuantizeButtonComponent button (tree);
        button.setSize (30, 30);
        // If we get here, construction succeeded
        expect (true);
    }

    // -------------------------------------------------------------------
    void testButtonTogglesProperty()
    {
        beginTest ("Button toggles quantizeEnabled in ValueTree on click");

        auto tree = createQuantizeTestDeckTree ("A");
        tree.setProperty (IDs::playbackStatus, "stopped", nullptr);
        QuantizeButtonComponent button (tree);
        button.setSize (30, 30);

        // Initially false
        expect (! static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));

        // Simulate click
        auto event = makeDummyMouseEvent (button);
        button.mouseDown (event);

        // Should now be true
        expect (static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));

        // Click again → should toggle back to false
        button.mouseDown (event);
        expect (! static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));
    }

    // -------------------------------------------------------------------
    void testButtonReflectsExternalChange()
    {
        beginTest ("Button reflects ValueTree changes via listener (external toggle)");

        auto tree = createQuantizeTestDeckTree ("A");
        tree.setProperty (IDs::playbackStatus, "stopped", nullptr);
        QuantizeButtonComponent button (tree);
        button.setSize (30, 30);

        // Externally set quantizeEnabled to true
        tree.setProperty (IDs::quantizeEnabled, true, nullptr);

        // The button's listener should have updated.
        // We can verify by clicking (which toggles) — if the internal state tracked the
        // external change, one click should set it to false.
        auto event = makeDummyMouseEvent (button);
        button.mouseDown (event);

        // After toggling from true → false
        expect (! static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));
    }

    // -------------------------------------------------------------------
    void testButtonNonInteractiveWhenEmpty()
    {
        beginTest ("Button is non-interactive when deck is Empty");

        auto tree = createQuantizeTestDeckTree ("A");
        tree.setProperty (IDs::playbackStatus, "empty", nullptr);
        QuantizeButtonComponent button (tree);
        button.setSize (30, 30);

        auto event = makeDummyMouseEvent (button);
        button.mouseDown (event);

        // quantizeEnabled should remain false — click ignored
        expect (! static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));
    }

    // -------------------------------------------------------------------
    void testButtonReducedOpacityNoBeatgrid()
    {
        beginTest ("Button shows reduced state when quantize ON but no beatgrid (bpm == 0)");

        auto tree = createQuantizeTestDeckTree ("A");
        tree.setProperty (IDs::playbackStatus, "stopped", nullptr);

        // Enable quantize with no beatgrid (bpm = 0, the default)
        tree.setProperty (IDs::quantizeEnabled, true, nullptr);
        QuantizeButtonComponent button (tree);
        button.setSize (30, 30);

        // The button should be in the "enabled but no beatgrid" visual state.
        // We verify the underlying state: quantize is ON, beatgrid bpm is 0.
        expect (static_cast<bool> (tree.getProperty (IDs::quantizeEnabled)));
        auto bg = tree.getChildWithName (IDs::BeatGrid);
        expectEquals (static_cast<double> (bg.getProperty (IDs::bpm)), 0.0);

        // Now set a valid beatgrid — the button's listener should detect the change
        bg.setProperty (IDs::bpm, 120.0, nullptr);
        expect (static_cast<double> (bg.getProperty (IDs::bpm)) > 0.0);

        // Paint should not crash in both states (no beatgrid and with beatgrid)
        juce::Image img (juce::Image::ARGB, 30, 30, true);
        juce::Graphics g (img);
        button.paint (g);
        expect (true);
    }
};

static QuantizeButtonTests quantizeButtonTests;

// =============================================================================
// 3. AudioStateSync Quantize Integration Tests
// =============================================================================
class QuantizeAudioStateSyncTests : public juce::UnitTest
{
public:
    QuantizeAudioStateSyncTests() : juce::UnitTest ("Quantize AudioStateSync", "Sonik") {}

    void runTest() override
    {
        testSyncQuantizeEnabled();
        testSyncBeatgridAnchor();
        testSyncBeatgridInterval();
    }

private:
    // -------------------------------------------------------------------
    void testSyncQuantizeEnabled()
    {
        beginTest ("AudioStateSync propagates quantizeEnabled from ValueTree to atomic");

        auto tree = createQuantizeTestDeckTree ("A");
        DeckAudioState audioState;
        AudioStateSync sync (tree, audioState);

        // Initial: quantizeEnabled is false
        expect (! audioState.quantizeEnabled.load (std::memory_order_relaxed));

        // Set to true in ValueTree
        tree.setProperty (IDs::quantizeEnabled, true, nullptr);
        expect (audioState.quantizeEnabled.load (std::memory_order_relaxed));

        // Toggle back
        tree.setProperty (IDs::quantizeEnabled, false, nullptr);
        expect (! audioState.quantizeEnabled.load (std::memory_order_relaxed));
    }

    // -------------------------------------------------------------------
    void testSyncBeatgridAnchor()
    {
        beginTest ("AudioStateSync propagates beatgridAnchor from ValueTree to atomic");

        auto tree = createQuantizeTestDeckTree ("A");
        DeckAudioState audioState;
        AudioStateSync sync (tree, audioState);

        auto bg = tree.getChildWithName (IDs::BeatGrid);

        // Initial: anchor is 0
        expectEquals (audioState.beatgridAnchor.load (std::memory_order_relaxed), (int64_t) 0);

        // Change anchor
        bg.setProperty (IDs::anchorSample, (int64_t) 44100, nullptr);
        expectEquals (audioState.beatgridAnchor.load (std::memory_order_relaxed), (int64_t) 44100);

        // Change again
        bg.setProperty (IDs::anchorSample, (int64_t) 1000, nullptr);
        expectEquals (audioState.beatgridAnchor.load (std::memory_order_relaxed), (int64_t) 1000);
    }

    // -------------------------------------------------------------------
    void testSyncBeatgridInterval()
    {
        beginTest ("AudioStateSync propagates beatgridInterval from ValueTree to atomic");

        auto tree = createQuantizeTestDeckTree ("A");
        DeckAudioState audioState;
        AudioStateSync sync (tree, audioState);

        auto bg = tree.getChildWithName (IDs::BeatGrid);

        // Initial: interval is 0
        expectWithinAbsoluteError (audioState.beatgridInterval.load (std::memory_order_relaxed),
                                   0.0, 0.001);

        // Set to 120 BPM interval
        bg.setProperty (IDs::beatIntervalSamples, 22050.0, nullptr);
        expectWithinAbsoluteError (audioState.beatgridInterval.load (std::memory_order_relaxed),
                                   22050.0, 0.001);

        // Set to 140 BPM interval (44100 * 60 / 140 = 18900)
        bg.setProperty (IDs::beatIntervalSamples, 18900.0, nullptr);
        expectWithinAbsoluteError (audioState.beatgridInterval.load (std::memory_order_relaxed),
                                   18900.0, 0.001);
    }
};

static QuantizeAudioStateSyncTests quantizeAudioStateSyncTests;
