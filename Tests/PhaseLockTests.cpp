#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Sync/PhaseLockEngine.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockSnapshot.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckIdentifiers.h"

class PhaseLockTests : public juce::UnitTest
{
public:
    PhaseLockTests() : juce::UnitTest ("Phase Lock Engine", "Sonik") {}

    void runTest() override
    {
        testNoOpWhenNotSynced();
        testNoOpWhenMasterNotPlaying();
        testNoOpWhenDeckNotPlaying();
        testNoOpWhenSlaveBpmZero();
        testNoOpWhenSlipEnabled();
        testPhaseZeroNoCorrection();
        testPhaseAheadSlowsDown();
        testPhaseBehindSpeesUp();
        testUnwrappedPhaseWraps();
        testStretcherLatencyApplied();
        testRampIsGradual();
        testSyncDisengagedSnapsToOne();
        testWithinThresholdTargetsOne();
        testNoCrashAtPhaseExactlyHalf();
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Build a DeckAudioState wired to a minimal ValueTree so AudioStateSync is happy
    struct TestFixture
    {
        juce::ValueTree root;
        juce::ValueTree deckTree;
        juce::ValueTree beatGrid;
        DeckAudioState  state;
        std::unique_ptr<AudioStateSync> sync;

        TestFixture()
        {
            root = juce::ValueTree (IDs::SonikState);
            auto decks = juce::ValueTree (IDs::Decks);
            root.addChild (decks, -1, nullptr);

            deckTree = juce::ValueTree (IDs::Deck);
            deckTree.setProperty (IDs::id,              "A",       nullptr);
            deckTree.setProperty (IDs::playbackStatus,  "playing", nullptr);
            deckTree.setProperty (IDs::gain,            1.0f,      nullptr);
            deckTree.setProperty (IDs::speedMultiplier, 1.0f,      nullptr);
            deckTree.setProperty (IDs::isSynced,        true,      nullptr);
            deckTree.setProperty (IDs::isMaster,        false,     nullptr);
            deckTree.setProperty (IDs::slipEnabled,     false,     nullptr);

            beatGrid = juce::ValueTree (IDs::BeatGrid);
            beatGrid.setProperty (IDs::bpm,                 128.0, nullptr);
            beatGrid.setProperty (IDs::anchorSample,        (int64_t) 0, nullptr);
            beatGrid.setProperty (IDs::beatIntervalSamples, 0.0,   nullptr);
            beatGrid.setProperty (IDs::confidence,          1.0f,  nullptr);
            beatGrid.setProperty (IDs::manuallyAdjusted,    false, nullptr);
            beatGrid.setProperty (IDs::analysisStatus,      "idle", nullptr);
            beatGrid.setProperty (IDs::analysisProgress,    0.0f,  nullptr);
            deckTree.addChild (beatGrid, -1, nullptr);

            // Minimal required child trees for AudioStateSync
            auto playhead = juce::ValueTree (IDs::Playhead);
            playhead.setProperty (IDs::position, (int64_t) 0, nullptr);
            deckTree.addChild (playhead, -1, nullptr);

            auto tempCue = juce::ValueTree (IDs::TempCue);
            tempCue.setProperty (IDs::position, (int64_t) -1, nullptr);
            deckTree.addChild (tempCue, -1, nullptr);

            auto cuePoints = juce::ValueTree (IDs::CuePoints);
            deckTree.addChild (cuePoints, -1, nullptr);

            auto loop = juce::ValueTree (IDs::Loop);
            loop.setProperty (IDs::loopIn,   (int64_t) -1, nullptr);
            loop.setProperty (IDs::loopOut,  (int64_t) -1, nullptr);
            loop.setProperty (IDs::active,   false, nullptr);
            loop.setProperty (IDs::loopMode, 0,     nullptr);
            deckTree.addChild (loop, -1, nullptr);

            auto keyInfo = juce::ValueTree (IDs::KeyInfo);
            keyInfo.setProperty (IDs::keyIndex,         -1,     nullptr);
            keyInfo.setProperty (IDs::confidence,       0.0f,   nullptr);
            keyInfo.setProperty (IDs::manuallyAdjusted, false,  nullptr);
            keyInfo.setProperty (IDs::analysisStatus,   "idle", nullptr);
            keyInfo.setProperty (IDs::analysisProgress, 0.0f,   nullptr);
            deckTree.addChild (keyInfo, -1, nullptr);

            auto stems = juce::ValueTree (IDs::Stems);
            stems.setProperty (IDs::status,    "none", nullptr);
            stems.setProperty (IDs::progress,  0.0f,   nullptr);
            stems.setProperty (IDs::stemError, "",     nullptr);
            deckTree.addChild (stems, -1, nullptr);

            decks.addChild (deckTree, -1, nullptr);

            sync = std::make_unique<AudioStateSync> (deckTree, state);
        }

        void setBpm (double bpm)
        {
            beatGrid.setProperty (IDs::bpm, bpm, nullptr);
        }

        void setPlaying (bool playing)
        {
            deckTree.setProperty (IDs::playbackStatus,
                                  playing ? "playing" : "paused", nullptr);
        }

        void setSynced (bool synced)
        {
            deckTree.setProperty (IDs::isSynced, synced, nullptr);
        }

        void setSlip (bool slip)
        {
            deckTree.setProperty (IDs::slipEnabled, slip, nullptr);
        }

        void setSpeedMultiplier (float mul)
        {
            deckTree.setProperty (IDs::speedMultiplier, mul, nullptr);
        }
    };

    static MasterClockSnapshot makePlaying (double bpm, int64_t origin = 0)
    {
        MasterClockSnapshot s;
        s.masterBPM               = bpm;
        s.masterIsPlaying         = true;
        s.masterPhaseOriginSample = origin;
        return s;
    }

    // -----------------------------------------------------------------------
    // Tests
    // -----------------------------------------------------------------------

    void testNoOpWhenNotSynced()
    {
        beginTest ("PhaseLockEngine - isSynced=false: correctionMultiplier stays 1.0");

        MasterClockPublisher pub;
        pub.publish (makePlaying (128.0));

        TestFixture fx;
        fx.setSynced (false);

        DeckAudioSource source;
        source.playheadAccumulator = 44100.0;
        source.stretcherLatency    = 0;
        source.correctionMultiplier = 0.99; // set to non-1 to verify snap

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, 44100.0);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must snap to 1.0 when not synced");
        expect (source.phaseOffset.load() == 0.0f,
                "phaseOffset must be 0.0f when not synced");
    }

    void testNoOpWhenMasterNotPlaying()
    {
        beginTest ("PhaseLockEngine - masterIsPlaying=false: correctionMultiplier stays 1.0");

        MasterClockPublisher pub;
        MasterClockSnapshot snap;
        snap.masterBPM       = 128.0;
        snap.masterIsPlaying = false;
        pub.publish (snap);

        TestFixture fx;
        fx.setBpm (128.0);
        DeckAudioSource source;
        source.correctionMultiplier = 1.01;

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, 44100.0);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 when master is not playing");
    }

    void testNoOpWhenDeckNotPlaying()
    {
        beginTest ("PhaseLockEngine - deck paused: correctionMultiplier stays 1.0");

        MasterClockPublisher pub;
        pub.publish (makePlaying (128.0));

        TestFixture fx;
        fx.setBpm (128.0);
        fx.setPlaying (false);
        DeckAudioSource source;
        source.correctionMultiplier = 1.01;

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, 44100.0);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 when deck is not playing");
    }

    void testNoOpWhenSlaveBpmZero()
    {
        beginTest ("PhaseLockEngine - slaveBPM=0: no crash, correctionMultiplier=1.0");

        MasterClockPublisher pub;
        pub.publish (makePlaying (128.0));

        TestFixture fx;
        fx.setBpm (0.0); // deckBPM = 0 → slaveBPM = 0

        DeckAudioSource source;
        source.correctionMultiplier = 1.01;

        PhaseLockEngine engine (pub);
        // Must not crash or divide by zero
        engine.process (source, fx.state, 44100.0);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 when slaveBPM is zero");
    }

    void testNoOpWhenSlipEnabled()
    {
        beginTest ("PhaseLockEngine - slip mode active: correctionMultiplier stays 1.0");

        MasterClockPublisher pub;
        pub.publish (makePlaying (128.0));

        TestFixture fx;
        fx.setBpm (128.0);
        fx.setSlip (true);
        DeckAudioSource source;
        source.correctionMultiplier = 1.01;

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, 44100.0);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 during slip mode");
    }

    void testPhaseZeroNoCorrection()
    {
        beginTest ("PhaseLockEngine - phase offset = 0.0: correctionMultiplier converges to 1.0");

        MasterClockPublisher pub;
        // masterPhaseOriginSample = 0, deck playhead = 0 → phase = 0
        pub.publish (makePlaying (128.0, 0));

        TestFixture fx;
        fx.setBpm (128.0);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = 0.0;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, 44100.0);

        // Phase is exactly 0.0, well inside convergenceThreshold — target = 1.0
        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 at zero phase offset");
        expectWithinAbsoluteError (source.phaseOffset.load(), 0.0f, 0.001f,
                                   "phaseOffset must be ~0 at zero offset");
    }

    void testPhaseAheadSlowsDown()
    {
        beginTest ("PhaseLockEngine - phase +0.3 (slave ahead): correctionMultiplier < 1.0");

        // 128 BPM @ 44100 → beatInterval = 44100*60/128 = 20671.875 samples
        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm; // ≈ 20671.875

        // Set playhead so phase = +0.3: effectivePlayhead = 0.3 * beatInterval
        const double effectivePos = 0.3 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0)); // origin = 0

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        // Run enough blocks to move correctionMultiplier away from 1.0
        for (int i = 0; i < 10; ++i)
            engine.process (source, fx.state, sr);

        expect (source.correctionMultiplier < 1.0,
                "correctionMultiplier must be < 1.0 when slave is ahead");
        expect (source.phaseOffset.load() > 0.0f,
                "phaseOffset must be > 0 when slave is ahead");
    }

    void testPhaseBehindSpeesUp()
    {
        beginTest ("PhaseLockEngine - phase -0.3 (slave behind): correctionMultiplier > 1.0");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        // Phase = -0.3: raw phase = 0.7 (since -0.3 wraps to 0.7 in [0,1))
        // then 0.7 > 0.5 → wraps to -0.3
        const double effectivePos = 0.7 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        for (int i = 0; i < 10; ++i)
            engine.process (source, fx.state, sr);

        expect (source.correctionMultiplier > 1.0,
                "correctionMultiplier must be > 1.0 when slave is behind");
        expectWithinAbsoluteError (source.phaseOffset.load(), -0.3f, 0.01f,
                                   "phaseOffset must be ~-0.3");
    }

    void testUnwrappedPhaseWraps()
    {
        beginTest ("PhaseLockEngine - raw phase 0.8 wraps to -0.2: correctionMultiplier > 1.0");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        // raw phase 0.8 → wraps to -0.2 → slave behind → speed up
        const double effectivePos = 0.8 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        for (int i = 0; i < 10; ++i)
            engine.process (source, fx.state, sr);

        expect (source.correctionMultiplier > 1.0,
                "raw phase 0.8 wraps to -0.2: correction must be > 1.0");
        expectWithinAbsoluteError (source.phaseOffset.load(), -0.2f, 0.01f,
                                   "wrapped phaseOffset must be ~-0.2");
    }

    void testStretcherLatencyApplied()
    {
        beginTest ("PhaseLockEngine - stretcherLatency=1024: effectivePlayhead = accumulator - 1024");

        // If we place the accumulator exactly on-beat (accounting for latency),
        // the effective playhead should still be on-beat → phase ≈ 0.
        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;
        constexpr int latency   = 1024;

        // Set accumulator so that (accumulator - latency) lands exactly at 0.0
        // (one full beat, just to avoid trivially 0).
        const double accumulator = beatInterval + static_cast<double> (latency);

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = accumulator;
        source.stretcherLatency     = latency;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, sr);

        // effectivePlayhead = accumulator - latency = beatInterval → phase = 0
        expectWithinAbsoluteError (source.phaseOffset.load(), 0.0f, 0.001f,
                                   "with latency compensation phase must be ~0");
        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must be 1.0 at zero phase");
    }

    void testRampIsGradual()
    {
        beginTest ("PhaseLockEngine - correctionMultiplier ramps gradually, not instant");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        // Slave ahead by 0.3 beats
        const double effectivePos = 0.3 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        // One block — should have moved only one step
        engine.process (source, fx.state, sr);

        constexpr double expectedStep =
            PhaseLockEngine::correctionRate / PhaseLockEngine::correctionWindowBlocks;
        const double distanceFrom1 = 1.0 - source.correctionMultiplier;

        expectWithinAbsoluteError (distanceFrom1, expectedStep, 1e-10,
                                   "after one block correctionMultiplier must have moved exactly one step");
    }

    void testSyncDisengagedSnapsToOne()
    {
        beginTest ("PhaseLockEngine - sync disengaged mid-ramp: correctionMultiplier snaps to 1.0");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = 0.3 * beatInterval;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        // Run a few blocks to start the ramp
        for (int i = 0; i < 5; ++i)
            engine.process (source, fx.state, sr);

        expect (source.correctionMultiplier < 1.0, "ramp should have started");

        // Disengage sync
        fx.setSynced (false);
        engine.process (source, fx.state, sr);

        expect (source.correctionMultiplier == 1.0,
                "correctionMultiplier must snap to 1.0 immediately when sync is off");
        expect (source.phaseOffset.load() == 0.0f,
                "phaseOffset must be 0 when sync is off");
    }

    void testWithinThresholdTargetsOne()
    {
        beginTest ("PhaseLockEngine - |phase| < threshold: correctionMultiplier ramps toward 1.0");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        // Phase = 0.01 beats — within the 0.02 convergenceThreshold
        const double effectivePos = 0.01 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 0.998; // slightly off — should ramp back to 1.0

        PhaseLockEngine engine (pub);
        engine.process (source, fx.state, sr);

        // Within threshold: targetCorrection = 1.0 → ramp toward 1.0
        expect (source.correctionMultiplier > 0.998,
                "correctionMultiplier must ramp toward 1.0 within dead-band");
    }

    void testNoCrashAtPhaseExactlyHalf()
    {
        beginTest ("PhaseLockEngine - phase exactly 0.5: treated as +0.5, slowdown applied");

        constexpr double sr    = 44100.0;
        constexpr double bpm   = 128.0;
        const double beatInterval = sr * 60.0 / bpm;

        // Phase = exactly 0.5 beats
        const double effectivePos = 0.5 * beatInterval;

        MasterClockPublisher pub;
        pub.publish (makePlaying (bpm, 0));

        TestFixture fx;
        fx.setBpm (bpm);
        fx.setSpeedMultiplier (1.0f);

        DeckAudioSource source;
        source.playheadAccumulator  = effectivePos;
        source.stretcherLatency     = 0;
        source.correctionMultiplier = 1.0;

        PhaseLockEngine engine (pub);

        // Must not crash; 0.5 stays as +0.5 → slow down
        engine.process (source, fx.state, sr);

        expectWithinAbsoluteError (source.phaseOffset.load(), 0.5f, 0.001f,
                                   "phase exactly 0.5 must be treated as +0.5");
        expect (source.correctionMultiplier < 1.0,
                "0.5 beat phase must trigger slowdown");
    }
};

static PhaseLockTests phaseLockTests;
