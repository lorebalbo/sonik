#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"

class TransportTests : public juce::UnitTest
{
public:
    TransportTests() : juce::UnitTest ("Transport System", "Sonik") {}

    void runTest() override
    {
        // Transport Commands
        testPlayFromStopped();
        testPauseDuringPlayback();
        testStopResetsPlayhead();
        testPlayAfterPauseResumes();

        // Seek
        testSeekWhileStopped();
        testSeekWhilePlaying();

        // Fade Ramps
        testFadeInOnPlay();
        testFadeOutOnPause();

        // End of Track
        testEndOfTrackStopsPlayback();

        // Playhead Precision
        testPlayheadAdvancesAtSpeed1();
        testSpeedMultiplierAffectsAdvancement();

        // Linear Interpolation
        testInterpolationAtNonUnitSpeed();

        // CUE behavior
        testCueSetInPausedState();
        testCueReturnDuringPlayback();

        // Silence
        testSilenceWhenNoBuffer();
        testSilenceWhenStopped();

        // Multiple Decks
        testIndependentTransport();

        // API
        testSendTransportCommandSetsAtomic();
        testSeekDeckSetsTargetAndCommand();
    }

private:
    // Helper: Engine context with ValueTree
    struct EngineContext
    {
        juce::ValueTree rootState { IDs::SonikState };
        std::unique_ptr<AudioEngine> engine;

        EngineContext()
        {
            engine = std::make_unique<AudioEngine> (rootState);
        }

        ~EngineContext()
        {
            engine.reset();
        }
    };

    // Helper: create a stereo AudioBufferHolder with constant values
    AudioBufferHolder::Ptr makeConstBuffer (float leftVal, float rightVal, int numFrames, double sampleRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (2, numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            buf.setSample (0, i, leftVal);
            buf.setSample (1, i, rightVal);
        }
        return new AudioBufferHolder (std::move (buf), sampleRate, static_cast<int64_t> (numFrames));
    }

    // Helper: create a stereo AudioBufferHolder with ramp data (0..1)
    AudioBufferHolder::Ptr makeRampBuffer (int numFrames, double sampleRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (2, numFrames);
        float invN = 1.0f / static_cast<float> (numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            float v = static_cast<float> (i) * invN;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        return new AudioBufferHolder (std::move (buf), sampleRate, static_cast<int64_t> (numFrames));
    }

    // Helper: run one processBlock and return output in the provided arrays
    void runBlock (AudioEngine& engine, float* outL, float* outR, int numSamples)
    {
        float* outputs[2] = { outL, outR };
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, numSamples, {});
    }

    // -----------------------------------------------------------------------
    // Transport Commands
    // -----------------------------------------------------------------------

    void testPlayFromStopped()
    {
        beginTest ("Play from stopped - output is non-silent, playhead advances");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 4096);
        ctx.engine->setDeckBuffer ("A", holder);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Playhead should have advanced
        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (playhead > 0, "Playhead should advance after Play");

        // Output should be non-silent (at least some samples non-zero due to fade-in)
        bool hasNonZero = false;
        for (int i = 0; i < blockSize; ++i)
        {
            if (outL[i] != 0.0f || outR[i] != 0.0f)
            {
                hasNonZero = true;
                break;
            }
        }
        expect (hasNonZero, "Output should be non-silent after Play");
    }

    void testPauseDuringPlayback()
    {
        beginTest ("Pause during playback - playhead stops advancing after fade-out");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Play and run a block to get past fade-in
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Now pause
        ctx.engine->sendTransportCommand ("A", TransportCommand::Pause);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playheadAfterPause = audioState.playheadPosition.load (std::memory_order_relaxed);

        // Run another block — playhead should NOT advance further
        runBlock (*ctx.engine, outL, outR, blockSize);
        auto playheadAfterExtra = audioState.playheadPosition.load (std::memory_order_relaxed);

        expectEquals (playheadAfterExtra, playheadAfterPause);
    }

    void testStopResetsPlayhead()
    {
        beginTest ("Stop resets playhead to 0");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Play and advance several blocks
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        for (int b = 0; b < 5; ++b)
            runBlock (*ctx.engine, outL, outR, blockSize);

        auto playheadBefore = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (playheadBefore > 0, "Playhead should have advanced");

        // Stop — triggers fade-out, then deferred stop
        ctx.engine->sendTransportCommand ("A", TransportCommand::Stop);
        // Run enough blocks for fade-out to complete (64 samples) and deferred action to execute
        runBlock (*ctx.engine, outL, outR, blockSize);
        // Run another block so the reset playhead value is published
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playheadAfter = audioState.playheadPosition.load (std::memory_order_relaxed);
        // After deferred Stop, the playhead resets to 0 but may advance by 1 sample
        // on the same iteration due to the sample-level advance in processBlock
        expect (playheadAfter <= 1, "Playhead should be at or very near 0 after Stop");

        auto status = static_cast<PlaybackStatusCode> (
            audioState.playbackStatus.load (std::memory_order_relaxed));
        expect (status == PlaybackStatusCode::stopped, "Status should be stopped");
    }

    void testPlayAfterPauseResumes()
    {
        beginTest ("Play after pause resumes from paused position (not 0)");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Play and advance
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        for (int b = 0; b < 3; ++b)
            runBlock (*ctx.engine, outL, outR, blockSize);

        // Pause and let fade-out complete
        ctx.engine->sendTransportCommand ("A", TransportCommand::Pause);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto pausedPlayhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (pausedPlayhead > 0, "Playhead should be > 0 after play+pause");

        // Play again — should resume from paused position
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto resumedPlayhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (resumedPlayhead > pausedPlayhead, "Playhead should resume advancing from paused position");
    }

    // -----------------------------------------------------------------------
    // Seek
    // -----------------------------------------------------------------------

    void testSeekWhileStopped()
    {
        beginTest ("Seek while stopped - playhead moves to target");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->seekDeck ("A", 1000);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expectEquals (playhead, static_cast<int64_t> (1000));
    }

    void testSeekWhilePlaying()
    {
        beginTest ("Seek while playing - playhead jumps near target and continues advancing");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Play and advance
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Seek to 5000 (will do fade-out → seek → fade-in)
        ctx.engine->seekDeck ("A", 5000);
        // Run two blocks to allow fade-out + fade-in to complete
        runBlock (*ctx.engine, outL, outR, blockSize);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        // Playhead should be near 5000 (5000 + some advancement from fade-in block)
        expect (playhead >= 5000, "Playhead should be at or past seek target");
        expect (playhead < 5500, "Playhead should be near seek target");
    }

    // -----------------------------------------------------------------------
    // Fade Ramps
    // -----------------------------------------------------------------------

    void testFadeInOnPlay()
    {
        beginTest ("Fade-in on play - first 64 samples ramp from 0 to 1");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (1.0f, 1.0f, 4096);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Sample 0 should be near 0 (start of fade-in: fadeGain = 1 - 64/64 = 0)
        expectWithinAbsoluteError (outL[0], 0.0f, 0.02f);

        // Sample 63 should be near 1.0 (end of fade-in: fadeGain ≈ 63/64)
        expectWithinAbsoluteError (outL[63], 63.0f / 64.0f, 0.02f);

        // Sample 64 should be exactly 1.0 (past fade-in)
        expectWithinAbsoluteError (outL[64], 1.0f, 0.01f);
    }

    void testFadeOutOnPause()
    {
        beginTest ("Fade-out on pause - first 64 samples ramp from 1 to 0, rest is silence");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (1.0f, 1.0f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Play and run enough blocks to get past fade-in
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        // Run 2 blocks (256 samples, well past 64-sample fade-in)
        runBlock (*ctx.engine, outL, outR, blockSize);
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Now pause — should trigger fade-out in next block
        ctx.engine->sendTransportCommand ("A", TransportCommand::Pause);
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Sample 0 should be near 1.0 (start of fade-out: fadeGain = 64/64 = 1.0)
        expectWithinAbsoluteError (outL[0], 1.0f, 0.02f);

        // Sample 63 should be near 0 (end of fade-out: fadeGain = 1/64 ≈ 0.016)
        expectWithinAbsoluteError (outL[63], 1.0f / 64.0f, 0.02f);

        // Samples past fade-out should be silence (status transitions to paused)
        for (int i = 64; i < blockSize; ++i)
            expectEquals (outL[i], 0.0f);
    }

    // -----------------------------------------------------------------------
    // End of Track
    // -----------------------------------------------------------------------

    void testEndOfTrackStopsPlayback()
    {
        beginTest ("End of track stops playback");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        // Short buffer: 256 samples
        auto holder = makeConstBuffer (0.5f, 0.25f, 256);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};

        // Run enough blocks to exhaust the buffer (256 samples + 64 fade-out)
        for (int b = 0; b < 5; ++b)
            runBlock (*ctx.engine, outL, outR, blockSize);

        auto status = static_cast<PlaybackStatusCode> (
            audioState.playbackStatus.load (std::memory_order_relaxed));
        expect (status == PlaybackStatusCode::stopped,
                "Status should be stopped after end of track");
    }

    // -----------------------------------------------------------------------
    // Playhead Precision
    // -----------------------------------------------------------------------

    void testPlayheadAdvancesAtSpeed1()
    {
        beginTest ("Playhead advances correctly at speed 1.0");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expectEquals (playhead, static_cast<int64_t> (blockSize));
    }

    void testSpeedMultiplierAffectsAdvancement()
    {
        beginTest ("Speed multiplier 2.0 doubles playhead advancement");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        audioState.speedMultiplier.store (2.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expectEquals (playhead, static_cast<int64_t> (blockSize * 2));
    }

    // -----------------------------------------------------------------------
    // Linear Interpolation
    // -----------------------------------------------------------------------

    void testInterpolationAtNonUnitSpeed()
    {
        beginTest ("Interpolation at speed 0.5 - output values are interpolated");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        audioState.speedMultiplier.store (0.5f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        // Ramp buffer: sample[i] = i / N
        const int bufFrames = 4096;
        auto holder = makeRampBuffer (bufFrames);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // At speed 0.5, after 128 output samples the playhead should be at 64
        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expectEquals (playhead, static_cast<int64_t> (64));

        // Check interpolated values past the fade-in region (sample 64+)
        // At output sample 64, playhead should be at position 32.0
        // source sample[32] = 32/4096 ≈ 0.0078125
        float invN = 1.0f / static_cast<float> (bufFrames);
        float expectedAt64 = 32.0f * invN;
        expectWithinAbsoluteError (outL[64], expectedAt64, 0.01f);
    }

    // -----------------------------------------------------------------------
    // CUE behavior
    // -----------------------------------------------------------------------

    void testCueSetInPausedState()
    {
        beginTest ("CueSet in paused state sets tempCuePosition");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Seek to 1000, then pause
        ctx.engine->seekDeck ("A", 1000);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize); // process Seek command

        // Set status to paused
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::paused), std::memory_order_relaxed);

        // Send CueSet
        ctx.engine->sendTransportCommand ("A", TransportCommand::CueSet);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto cuePos = audioState.tempCuePosition.load (std::memory_order_relaxed);
        expectEquals (cuePos, static_cast<int64_t> (1000));
    }

    void testCueReturnDuringPlayback()
    {
        beginTest ("CueReturn during playback returns to cue position and pauses");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        // Set temp cue AFTER setDeckBuffer (which resets it to 0)
        audioState.tempCuePosition.store (500, std::memory_order_relaxed);

        // Play and advance
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);
        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        for (int b = 0; b < 5; ++b)
            runBlock (*ctx.engine, outL, outR, blockSize);

        auto playheadBefore = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (playheadBefore > 500, "Playhead should have advanced past cue");

        // CueReturn — fade-out then return to cue
        ctx.engine->sendTransportCommand ("A", TransportCommand::CueReturn);
        // Run enough blocks for fade-out to complete
        runBlock (*ctx.engine, outL, outR, blockSize);
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playheadAfter = audioState.playheadPosition.load (std::memory_order_relaxed);
        // CueReturn jumps to cue position; slight advancement possible during fade-out completion sample
        expect (std::abs (playheadAfter - 500) <= 1, "Playhead should be at or very near cue position 500");

        auto status = static_cast<PlaybackStatusCode> (
            audioState.playbackStatus.load (std::memory_order_relaxed));
        expect (status == PlaybackStatusCode::paused, "Status should be paused after CueReturn");
    }

    // -----------------------------------------------------------------------
    // Silence
    // -----------------------------------------------------------------------

    void testSilenceWhenNoBuffer()
    {
        beginTest ("Silence when no buffer loaded");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);
        // No buffer set

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            expectEquals (outL[i], 0.0f);
            expectEquals (outR[i], 0.0f);
        }
    }

    void testSilenceWhenStopped()
    {
        beginTest ("Silence when stopped with buffer loaded");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 4096);
        ctx.engine->setDeckBuffer ("A", holder);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            expectEquals (outL[i], 0.0f);
            expectEquals (outR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    // Multiple Decks
    // -----------------------------------------------------------------------

    void testIndependentTransport()
    {
        beginTest ("Independent transport - play deck A, deck B remains silent");
        EngineContext ctx;
        DeckAudioState stateA, stateB;
        stateA.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        stateB.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &stateA);
        ctx.engine->registerDeck ("B", &stateB);

        auto holderA = makeConstBuffer (0.5f, 0.5f, 44100);
        auto holderB = makeConstBuffer (0.3f, 0.3f, 44100);
        ctx.engine->setDeckBuffer ("A", holderA);
        ctx.engine->setDeckBuffer ("B", holderB);

        // Play only deck A
        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 128;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        // Deck A should have advanced, deck B should not
        auto playheadA = stateA.playheadPosition.load (std::memory_order_relaxed);
        auto playheadB = stateB.playheadPosition.load (std::memory_order_relaxed);
        expect (playheadA > 0, "Deck A playhead should advance");
        expectEquals (playheadB, static_cast<int64_t> (0));

        // Output should contain only deck A's contribution (with fade-in)
        // After sample 64, output should be 0.5 (only from deck A)
        expectWithinAbsoluteError (outL[65], 0.5f, 0.01f);

        // Deck B status should still be stopped
        auto statusB = static_cast<PlaybackStatusCode> (
            stateB.playbackStatus.load (std::memory_order_relaxed));
        expect (statusB == PlaybackStatusCode::stopped, "Deck B should remain stopped");
    }

    // -----------------------------------------------------------------------
    // sendTransportCommand / seekDeck API
    // -----------------------------------------------------------------------

    void testSendTransportCommandSetsAtomic()
    {
        beginTest ("sendTransportCommand sets pendingCommand atomic");
        EngineContext ctx;
        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        // Access the pending command — need to read through the deckSlots
        // We verify indirectly: running processBlock should transition state
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);

        auto holder = makeConstBuffer (0.5f, 0.25f, 1024);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->sendTransportCommand ("A", TransportCommand::Play);

        constexpr int blockSize = 64;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto status = static_cast<PlaybackStatusCode> (
            audioState.playbackStatus.load (std::memory_order_relaxed));
        expect (status == PlaybackStatusCode::playing,
                "processBlock should consume the Play command and set status to playing");
    }

    void testSeekDeckSetsTargetAndCommand()
    {
        beginTest ("seekDeck sets seekTarget and pendingCommand to Seek");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped), std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.25f, 44100);
        ctx.engine->setDeckBuffer ("A", holder);

        ctx.engine->seekDeck ("A", 5000);

        // Run processBlock — should execute the Seek command
        constexpr int blockSize = 64;
        float outL[blockSize] = {};
        float outR[blockSize] = {};
        runBlock (*ctx.engine, outL, outR, blockSize);

        auto playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        expectEquals (playhead, static_cast<int64_t> (5000));
    }
};

static TransportTests transportTests;
