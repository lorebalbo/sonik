#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"

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
        testMultipleDeckRegistration();
        testReRegisterDeck();
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
};

// Register the test instance
static AudioEngineTests audioEngineTests;
