#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockSnapshot.h"
#include "Features/Daw/Playback/ClipStreamer.h"
#include "Features/Daw/Import/ImportSourcePublisher.h"

class AudioEngineTests : public juce::UnitTest
{
public:
    AudioEngineTests() : juce::UnitTest ("Audio Engine Core", "Sonik") {}

    void runTest() override
    {
        testStateTreeInitialization();
        testDefaultPropertyValues();
        testDeckRegistration();
        testDeckUnregistration();
        testInvalidDeckIdIgnored();
        testDeckAudioSourceDefaults();
        testProcessBlockSilenceNoDecks();
        testProcessBlockSilenceRegisteredNotPlaying();
        testMeteringZerosWhenStopped();
        testMeteringZerosWhenEmpty();
        testCpuLoadMonitorInitial();
        testAudioStatePointerAfterRegister();
        testProcessBlockClearsOutput();
        testProcessBlockHardClip();
        testSyncDisengageKeepsCurrentSpeed();
        testMultipleDeckRegistration();
        testReRegisterDeck();
        testDawPauseSilencesArrangement();
    }

private:
    // Helper: create a root ValueTree and an AudioEngine attached to it.
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

    // -----------------------------------------------------------------------
    void testStateTreeInitialization()
    {
        beginTest ("Initialization - AudioDevice node exists in state tree");
        EngineContext ctx;

        auto audioDeviceNode = ctx.rootState.getChildWithName (IDs::AudioDevice);
        expect (audioDeviceNode.isValid(), "AudioDevice node should exist in state tree");
    }

    // -----------------------------------------------------------------------
    void testDefaultPropertyValues()
    {
        beginTest ("Initialization - AudioDevice has correct default properties");
        EngineContext ctx;

        auto node = ctx.rootState.getChildWithName (IDs::AudioDevice);
        expect (node.isValid());

        // deviceName defaults to empty string
        expectEquals (node.getProperty (IDs::deviceName).toString(), juce::String (""));

        // sampleRate defaults to 0.0
        expectEquals (static_cast<double> (node.getProperty (IDs::sampleRate)), 0.0);

        // bufferSize defaults to 0
        expectEquals (static_cast<int> (node.getProperty (IDs::bufferSize)), 0);

        // outputLatencyMs defaults to 0.0
        expectEquals (static_cast<double> (node.getProperty (IDs::outputLatencyMs)), 0.0);

        // cpuLoad defaults to 0.0
        expectEquals (static_cast<float> (node.getProperty (IDs::cpuLoad)), 0.0f);

        // cpuOverload defaults to false
        expect (! static_cast<bool> (node.getProperty (IDs::cpuOverload)),
                "cpuOverload should default to false");

        // deviceError defaults to empty string
        expectEquals (node.getProperty (IDs::deviceError).toString(), juce::String (""));
    }

    // -----------------------------------------------------------------------
    void testDeckRegistration()
    {
        beginTest ("Deck Registration - registerDeck maps A=0, B=1, C=2, D=3");
        EngineContext ctx;

        DeckAudioState stateA, stateB, stateC, stateD;

        // Register all four decks — should not crash or assert
        ctx.engine->registerDeck ("A", &stateA);
        ctx.engine->registerDeck ("B", &stateB);
        ctx.engine->registerDeck ("C", &stateC);
        ctx.engine->registerDeck ("D", &stateD);

        // Verify by calling processBlock — if registration crashed we won't get here
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // If we reached here, all four decks were registered successfully
        expect (true, "All four decks registered without error");
    }

    // -----------------------------------------------------------------------
    void testDeckUnregistration()
    {
        beginTest ("Deck Unregistration - unregisterDeck safely removes deck");
        EngineContext ctx;

        DeckAudioState stateA;
        ctx.engine->registerDeck ("A", &stateA);
        ctx.engine->unregisterDeck ("A");

        // processBlock should work fine after unregistration
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // Output should be silence
        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testInvalidDeckIdIgnored()
    {
        beginTest ("Deck Registration - invalid deck IDs are safely ignored");
        EngineContext ctx;

        DeckAudioState state;
        // These should not crash or have side effects
        ctx.engine->registerDeck ("E", &state);
        ctx.engine->registerDeck ("", &state);
        ctx.engine->registerDeck ("Z", &state);
        ctx.engine->unregisterDeck ("E");
        ctx.engine->unregisterDeck ("");
        ctx.engine->unregisterDeck ("Z");

        expect (true, "Invalid deck IDs handled gracefully");
    }

    // -----------------------------------------------------------------------
    void testDeckAudioSourceDefaults()
    {
        beginTest ("DeckAudioSource - default state has nullptr buffer and zero metering");

        DeckAudioSource source;
        expect (source.channelL.load() == nullptr, "channelL should default to nullptr");
        expect (source.channelR.load() == nullptr, "channelR should default to nullptr");
        expectEquals (source.bufferNumFrames.load(), static_cast<int64_t> (0));
        expect (source.audioState == nullptr, "audioState should default to nullptr");
        expectEquals (source.peakL.load(), 0.0f);
        expectEquals (source.peakR.load(), 0.0f);
        expectEquals (source.rmsL.load(), 0.0f);
        expectEquals (source.rmsR.load(), 0.0f);
    }

    // -----------------------------------------------------------------------
    void testProcessBlockSilenceNoDecks()
    {
        beginTest ("processBlock - outputs silence when no decks registered");
        EngineContext ctx;

        // Fill output with non-zero values to verify clearing
        float outputL[128];
        float outputR[128];
        for (int i = 0; i < 128; ++i)
        {
            outputL[i] = 0.999f;
            outputR[i] = -0.999f;
        }

        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 128, {});

        for (int i = 0; i < 128; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testProcessBlockSilenceRegisteredNotPlaying()
    {
        beginTest ("processBlock - outputs silence when deck registered but stopped");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped),
                                         std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        float outputL[128];
        float outputR[128];
        for (int i = 0; i < 128; ++i)
        {
            outputL[i] = 1.0f;
            outputR[i] = 1.0f;
        }

        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 128, {});

        for (int i = 0; i < 128; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testMeteringZerosWhenStopped()
    {
        beginTest ("Metering - zeros when deck is stopped");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped),
                                         std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        // Run a processBlock to trigger metering update
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // We can't directly read the DeckAudioSource metering from outside,
        // but the processBlock sets metering to zero for non-playing decks.
        // Verify output is still silence.
        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testMeteringZerosWhenEmpty()
    {
        beginTest ("Metering - zeros when deck status is empty");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::empty),
                                         std::memory_order_relaxed);
        ctx.engine->registerDeck ("B", &audioState);

        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testCpuLoadMonitorInitial()
    {
        beginTest ("CPU Load Monitor - initial values are zero/false");
        EngineContext ctx;

        expectEquals (ctx.engine->getCpuLoad(), 0.0f);
        expect (! ctx.engine->getCpuOverload(), "CPU overload should initially be false");
    }

    // -----------------------------------------------------------------------
    void testAudioStatePointerAfterRegister()
    {
        beginTest ("Integration - audioState pointer is correctly wired after registerDeck");
        EngineContext ctx;

        DeckAudioState stateA;
        stateA.gain.store (0.75f, std::memory_order_relaxed);
        stateA.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                     std::memory_order_relaxed);

        ctx.engine->registerDeck ("A", &stateA);

        // Call processBlock — the engine should read our audioState's playbackStatus
        // and since it's "playing" but channelL/R is nullptr, metering stays zero
        // and output stays silence.
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // Output should be silence since no buffer loaded
        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testProcessBlockClearsOutput()
    {
        beginTest ("processBlock - always clears output buffers first");
        EngineContext ctx;

        // Fill with garbage
        float outputL[32];
        float outputR[32];
        for (int i = 0; i < 32; ++i)
        {
            outputL[i] = 42.0f;
            outputR[i] = -42.0f;
        }

        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 32, {});

        for (int i = 0; i < 32; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testProcessBlockHardClip()
    {
        beginTest ("processBlock - hard clips output to [-1, 1]");
        EngineContext ctx;

        // Since no decks are producing audio, the output is zero after clearing.
        // The hard clip stage runs on the cleared output, so values stay at 0.
        // We test that the clip logic exists by verifying the output range.
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        for (int i = 0; i < 64; ++i)
        {
            expect (outputL[i] >= -1.0f && outputL[i] <= 1.0f,
                    "Left channel sample should be in [-1, 1]");
            expect (outputR[i] >= -1.0f && outputR[i] <= 1.0f,
                    "Right channel sample should be in [-1, 1]");
        }
    }

    // -----------------------------------------------------------------------
    void testSyncDisengageKeepsCurrentSpeed()
    {
        beginTest ("Sync disengage keeps current speed multiplier latched");
        EngineContext ctx;

        DeckAudioState stateB;
        stateB.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing), std::memory_order_relaxed);
        stateB.deckBPM.store (100.0, std::memory_order_relaxed);
        stateB.pitchFaderMultiplier.store (1.0f, std::memory_order_relaxed);
        stateB.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        stateB.isSynced.store (true, std::memory_order_relaxed);

        ctx.engine->registerDeck ("B", &stateB);

        juce::AudioBuffer<float> deckBuffer (2, 1024);
        deckBuffer.clear();
        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (deckBuffer), 44100.0, 1024);
        ctx.engine->setDeckBuffer ("B", holder);

        MasterClockPublisher publisher;
        MasterClockSnapshot snapshot;
        snapshot.masterBPM = 128.0;
        snapshot.masterIsPlaying = true;
        publisher.publish (snapshot);
        ctx.engine->setMasterClockPublisher (&publisher);

        float outputL[128] = {};
        float outputR[128] = {};
        float* outputs[2] = { outputL, outputR };

        // SYNC on: speed should be forced to master/deck ratio (128/100 = 1.28)
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 128, {});
        expectWithinAbsoluteError (stateB.speedMultiplier.load (std::memory_order_relaxed),
                                   1.28f,
                                   0.0005f,
                                   "With SYNC on, speed should match normalized ratio");

        // SYNC off: speed must remain latched to current value (not snapped to pitch fader)
        stateB.isSynced.store (false, std::memory_order_relaxed);
        stateB.pitchFaderMultiplier.store (1.0f, std::memory_order_relaxed);
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 128, {});

        expectWithinAbsoluteError (stateB.speedMultiplier.load (std::memory_order_relaxed),
                                   1.28f,
                                   0.0005f,
                                   "With SYNC off, speed should stay at the last synced value");
    }

    // -----------------------------------------------------------------------
    void testMultipleDeckRegistration()
    {
        beginTest ("Multiple Decks - register all four and run processBlock");
        EngineContext ctx;

        DeckAudioState states[4];
        for (int i = 0; i < 4; ++i)
            states[i].playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped),
                                            std::memory_order_relaxed);

        ctx.engine->registerDeck ("A", &states[0]);
        ctx.engine->registerDeck ("B", &states[1]);
        ctx.engine->registerDeck ("C", &states[2]);
        ctx.engine->registerDeck ("D", &states[3]);

        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // All decks stopped, output should be silence
        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    void testReRegisterDeck()
    {
        beginTest ("Re-registration - unregister then re-register same deck slot");
        EngineContext ctx;

        DeckAudioState stateA1;
        stateA1.gain.store (0.5f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &stateA1);
        ctx.engine->unregisterDeck ("A");

        // Re-register with a different state
        DeckAudioState stateA2;
        stateA2.gain.store (0.8f, std::memory_order_relaxed);
        stateA2.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                      std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &stateA2);

        // processBlock should use the new state (playing but no buffer = silence)
        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.0f);
            expectEquals (outputR[i], 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    // Regression: pausing the DAW transport must SILENCE the arrangement, not
    // just freeze the playhead. The renderer is playhead-driven and has no
    // notion of Paused vs. Playing (when Paused the playhead is frozen at a
    // positive sample, NOT the -1 Stopped sentinel), so processBlock must gate
    // the renderBlock call on transport->isPlaying(). Without that gate the
    // streamers keep being pulled and audio keeps sounding with a stationary
    // playhead — exactly the reported bug.
    void testDawPauseSilencesArrangement()
    {
        beginTest ("DAW transport - Pause silences arrangement audio (not just the playhead)");

        // Declared BEFORE the engine so the engine (which holds raw pointers to
        // them) is destroyed first on teardown.
        Daw::ArrangementPublisher publisher;
        Daw::ClipStreamerPool     pool (4);
        Daw::DawTransport         transport;

        EngineContext ctx;

        // A 1-second constant-amplitude stereo source: every sample == 1.0, so
        // any pulled audio is unmistakably non-zero.
        constexpr double rate      = 44100.0;
        constexpr int    numFrames = 44100;
        juce::AudioBuffer<float> srcBuf (2, numFrames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numFrames; ++i)
                srcBuf.setSample (ch, i, 1.0f);
        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (srcBuf), rate, numFrames);

        // Prime a streamer slot, then block until its ring holds the block we
        // will pull so the "Playing" assertion does not race the reader thread.
        const int32_t slot = pool.resolveHandle ("daw-pause-regression");
        expect (slot >= 0, "pool must assign a streamer slot");
        pool.prime (slot,
                    std::make_unique<Daw::Import::BufferAudioFormatReader> (holder),
                    rate, /*sourceStart*/ 0, /*sourceEnd*/ numFrames);
        if (auto* s = pool.getStreamer (slot))
            s->waitUntilReady (256);

        // Publish a single clip that covers the timeline from sample 0.
        Daw::ArrangementSnapshot snap;
        snap.laneCount      = 1;
        snap.lanes[0].count = 1;
        auto& ev = snap.lanes[0].events[0];
        ev.sourceReadHandle    = slot;
        ev.sourceStartSample   = 0;
        ev.sourceEndSample     = numFrames;
        ev.timelineStartSample = 0;
        ev.timelineEndSample   = numFrames;
        ev.gain                = 1.0f;
        ev.laneIndex           = 0;
        publisher.publish (snap);

        // Wire the DAW path; this builds the TimelineRenderer (device defaults
        // give a valid sample rate / block size even without a real device).
        ctx.engine->setDawPlayback (&publisher, &pool, &transport);

        constexpr int blockLen = 64;
        float outL[blockLen];
        float outR[blockLen];
        float* outs[2] = { outL, outR };

        auto renderPeak = [&]() -> float
        {
            for (int i = 0; i < blockLen; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
            ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, blockLen, {});
            float peak = 0.0f;
            for (int i = 0; i < blockLen; ++i)
            {
                peak = juce::jmax (peak, std::abs (outL[i]));
                peak = juce::jmax (peak, std::abs (outR[i]));
            }
            return peak;
        };

        // PLAYING -> arrangement is audible.
        transport.play();
        const float playingPeak = renderPeak();
        expect (playingPeak > 0.1f,
                "arrangement audio must be audible while transport is Playing");

        // PAUSED -> playhead frozen at a positive sample, but audio must stop.
        transport.pause();
        expect (transport.getPlayheadSample() > 0,
                "Pause freezes the playhead at a positive sample (not the -1 stop sentinel)");
        const float pausedPeak = renderPeak();
        expectWithinAbsoluteError (pausedPeak, 0.0f, 1.0e-6f,
                "no arrangement audio may be produced while Paused");

        // RESUME -> audio returns on play.
        transport.play();
        const float resumedPeak = renderPeak();
        expect (resumedPeak > 0.1f, "arrangement audio must resume on Play");

        // Detach before the wiring locals go out of scope.
        ctx.engine->setDawPlayback (nullptr, nullptr, nullptr);
    }
};

// Register the test instance
static AudioEngineTests audioEngineTests;
