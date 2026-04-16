#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/AudioEngine/AudioFileLoader.h"
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/Database/TrackDatabase.h"

class AudioFileLoaderTests : public juce::UnitTest
{
public:
    AudioFileLoaderTests() : juce::UnitTest ("Audio File Loader", "Sonik") {}

    void runTest() override
    {
        testAudioBufferHolderConstruction();
        testAudioBufferHolderRefCounting();
        testAudioBufferHolderMoveSemantics();
        testDeckAudioSourceDefaultState();
        testDeckAudioSourceAtomicPointerStores();
        testDeckAudioSourceMeteringDefaults();
        testSetDeckBuffer();
        testClearDeckBuffer();
        testSetDeckBufferWithNullHolder();
        testSetDeckBufferWithMonoBuffer();
        testIsSupportedExtensionPositive();
        testIsSupportedExtensionNegative();
        testIsSupportedExtensionCaseInsensitive();
        testProcessBlockReadsFromBuffer();
        testProcessBlockSilenceWhenBufferCleared();
        testProcessBlockSilenceWhenStopped();
        testInitialLoadingState();
        testResetTrackSpecificStateClearsLoading();
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

    // Helper: DeckStateManager context with temp database
    struct DeckContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> mgr;

        DeckContext()
        {
            dbFile = juce::File::createTempFile ("sonik_loader_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            mgr = std::make_unique<DeckStateManager> (*db);
        }

        ~DeckContext()
        {
            mgr.reset();
            db.reset();
            dbFile.deleteFile();
        }
    };

    // Helper: create a stereo AudioBufferHolder with known values
    AudioBufferHolder::Ptr makeStereoBuffer (float leftVal, float rightVal, int numFrames, double sampleRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (2, numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            buf.setSample (0, i, leftVal);
            buf.setSample (1, i, rightVal);
        }
        return new AudioBufferHolder (std::move (buf), sampleRate, static_cast<int64_t> (numFrames));
    }

    // -----------------------------------------------------------------------
    // AudioBufferHolder tests
    // -----------------------------------------------------------------------

    void testAudioBufferHolderConstruction()
    {
        beginTest ("AudioBufferHolder - construction with 2-channel buffer");

        const int numFrames = 1024;
        const double sr = 48000.0;
        juce::AudioBuffer<float> buf (2, numFrames);
        buf.setSample (0, 0, 0.5f);
        buf.setSample (1, 0, -0.25f);

        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), sr, numFrames);

        expectEquals (holder->getSampleRate(), sr);
        expectEquals (holder->getNumFrames(), static_cast<int64_t> (numFrames));
        expectEquals (holder->getBuffer().getNumChannels(), 2);
        expectEquals (holder->getBuffer().getNumSamples(), numFrames);
        expectEquals (holder->getBuffer().getSample (0, 0), 0.5f);
        expectEquals (holder->getBuffer().getSample (1, 0), -0.25f);
    }

    void testAudioBufferHolderRefCounting()
    {
        beginTest ("AudioBufferHolder - ref counting: copy Ptr, release one, other still valid");

        auto holder1 = makeStereoBuffer (1.0f, -1.0f, 512);
        {
            AudioBufferHolder::Ptr holder2 = holder1;
            expect (holder2 != nullptr, "Copy should be valid");
            expect (holder1.get() == holder2.get(), "Both should point to the same object");
            expectEquals (holder2->getNumFrames(), static_cast<int64_t> (512));
        }
        // holder2 is now out of scope / released
        expect (holder1 != nullptr, "Original should still be valid after copy goes out of scope");
        expectEquals (holder1->getNumFrames(), static_cast<int64_t> (512));
    }

    void testAudioBufferHolderMoveSemantics()
    {
        beginTest ("AudioBufferHolder - move construction preserves buffer data");

        const int numFrames = 256;
        juce::AudioBuffer<float> buf (2, numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            buf.setSample (0, i, static_cast<float> (i) / static_cast<float> (numFrames));
            buf.setSample (1, i, -static_cast<float> (i) / static_cast<float> (numFrames));
        }

        // Move the buffer into the holder
        float expectedFirst = buf.getSample (0, 0);
        float expectedLast  = buf.getSample (0, numFrames - 1);

        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), 44100.0, numFrames);

        expectEquals (holder->getBuffer().getSample (0, 0), expectedFirst);
        expectEquals (holder->getBuffer().getSample (0, numFrames - 1), expectedLast);
        expectEquals (holder->getBuffer().getNumChannels(), 2);
        expectEquals (holder->getBuffer().getNumSamples(), numFrames);
    }

    // -----------------------------------------------------------------------
    // DeckAudioSource tests
    // -----------------------------------------------------------------------

    void testDeckAudioSourceDefaultState()
    {
        beginTest ("DeckAudioSource - default state: nullptr channels, 0 frames, nullptr holder");

        DeckAudioSource source;
        expect (source.channelL.load() == nullptr, "channelL should default to nullptr");
        expect (source.channelR.load() == nullptr, "channelR should default to nullptr");
        expectEquals (source.bufferNumFrames.load(), static_cast<int64_t> (0));
        expect (source.bufferHolder == nullptr, "bufferHolder should default to nullptr");
    }

    void testDeckAudioSourceAtomicPointerStores()
    {
        beginTest ("DeckAudioSource - atomic pointer stores and reads");

        DeckAudioSource source;
        float testBufferL[16] = {};
        float testBufferR[16] = {};

        for (int i = 0; i < 16; ++i)
        {
            testBufferL[i] = static_cast<float> (i);
            testBufferR[i] = static_cast<float> (i) * 2.0f;
        }

        source.channelL.store (testBufferL, std::memory_order_release);
        source.channelR.store (testBufferR, std::memory_order_release);
        source.bufferNumFrames.store (16, std::memory_order_relaxed);

        expect (source.channelL.load (std::memory_order_acquire) == testBufferL);
        expect (source.channelR.load (std::memory_order_acquire) == testBufferR);
        expectEquals (source.bufferNumFrames.load(), static_cast<int64_t> (16));

        // Verify data is accessible via the stored pointer
        expectEquals (source.channelL.load()[5], 5.0f);
        expectEquals (source.channelR.load()[5], 10.0f);
    }

    void testDeckAudioSourceMeteringDefaults()
    {
        beginTest ("DeckAudioSource - metering defaults: peakL/peakR/rmsL/rmsR all 0.0f");

        DeckAudioSource source;
        expectEquals (source.peakL.load(), 0.0f);
        expectEquals (source.peakR.load(), 0.0f);
        expectEquals (source.rmsL.load(), 0.0f);
        expectEquals (source.rmsR.load(), 0.0f);
    }

    // -----------------------------------------------------------------------
    // AudioEngine buffer delivery tests
    // -----------------------------------------------------------------------

    void testSetDeckBuffer()
    {
        beginTest ("setDeckBuffer - sets channelL/R to buffer read pointers");
        EngineContext ctx;

        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeStereoBuffer (0.5f, 0.25f, 1024);
        ctx.engine->setDeckBuffer ("A", holder);

        // Verify by running processBlock with playing status — output should contain our values
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        // Default gain is 1.0, so output should match the buffer values
        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.5f);
            expectEquals (outputR[i], 0.25f);
        }
    }

    void testClearDeckBuffer()
    {
        beginTest ("clearDeckBuffer - clears channelL/R to nullptr and bufferHolder to nullptr");
        EngineContext ctx;

        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        // Set a buffer first
        auto holder = makeStereoBuffer (0.5f, 0.25f, 1024);
        ctx.engine->setDeckBuffer ("A", holder);

        // Now clear it
        ctx.engine->clearDeckBuffer ("A");

        // Set to playing and run processBlock — should be silence
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

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

    void testSetDeckBufferWithNullHolder()
    {
        beginTest ("setDeckBuffer with null holder - clears buffer (same as clearDeckBuffer)");
        EngineContext ctx;

        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        // Set a buffer first
        auto holder = makeStereoBuffer (0.5f, 0.25f, 512);
        ctx.engine->setDeckBuffer ("A", holder);

        // Now set null holder — should clear
        ctx.engine->setDeckBuffer ("A", nullptr);

        // Verify silence
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

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

    void testSetDeckBufferWithMonoBuffer()
    {
        beginTest ("setDeckBuffer with mono buffer - rejected (clears buffer)");
        EngineContext ctx;

        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        // Create a mono buffer (1 channel)
        juce::AudioBuffer<float> monoBuf (1, 512);
        for (int i = 0; i < 512; ++i)
            monoBuf.setSample (0, i, 0.75f);

        AudioBufferHolder::Ptr monoHolder = new AudioBufferHolder (std::move (monoBuf), 44100.0, 512);
        ctx.engine->setDeckBuffer ("A", monoHolder);

        // Should have been rejected — output should be silence
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

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
    // AudioFileLoader static helper tests
    // -----------------------------------------------------------------------

    void testIsSupportedExtensionPositive()
    {
        beginTest ("isSupportedExtension - .mp3, .flac, .wav, .aiff, .aif return true");

        expect (AudioFileLoader::isSupportedExtension (".mp3"),  ".mp3 should be supported");
        expect (AudioFileLoader::isSupportedExtension (".flac"), ".flac should be supported");
        expect (AudioFileLoader::isSupportedExtension (".wav"),  ".wav should be supported");
        expect (AudioFileLoader::isSupportedExtension (".aiff"), ".aiff should be supported");
        expect (AudioFileLoader::isSupportedExtension (".aif"),  ".aif should be supported");
    }

    void testIsSupportedExtensionNegative()
    {
        beginTest ("isSupportedExtension - .ogg, .wma, .txt, .jpg, empty return false");

        expect (! AudioFileLoader::isSupportedExtension (".ogg"), ".ogg should not be supported");
        expect (! AudioFileLoader::isSupportedExtension (".wma"), ".wma should not be supported");
        expect (! AudioFileLoader::isSupportedExtension (".txt"), ".txt should not be supported");
        expect (! AudioFileLoader::isSupportedExtension (".jpg"), ".jpg should not be supported");
        expect (! AudioFileLoader::isSupportedExtension (""),     "empty should not be supported");
    }

    void testIsSupportedExtensionCaseInsensitive()
    {
        beginTest ("isSupportedExtension - case insensitive: .MP3, .Flac, .WAV all return true");

        expect (AudioFileLoader::isSupportedExtension (".MP3"),  ".MP3 should be supported");
        expect (AudioFileLoader::isSupportedExtension (".Flac"), ".Flac should be supported");
        expect (AudioFileLoader::isSupportedExtension (".WAV"),  ".WAV should be supported");
        expect (AudioFileLoader::isSupportedExtension (".AIFF"), ".AIFF should be supported");
        expect (AudioFileLoader::isSupportedExtension (".AIF"),  ".AIF should be supported");
    }

    // -----------------------------------------------------------------------
    // processBlock with loaded buffer tests
    // -----------------------------------------------------------------------

    void testProcessBlockReadsFromBuffer()
    {
        beginTest ("processBlock - reads from buffer with known sample values");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);

        ctx.engine->registerDeck ("A", &audioState);

        // Create buffer with 0.5 for L and 0.25 for R
        auto holder = makeStereoBuffer (0.5f, 0.25f, 256);
        ctx.engine->setDeckBuffer ("A", holder);

        float outputL[64] = {};
        float outputR[64] = {};
        float* outputs[2] = { outputL, outputR };
        ctx.engine->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, 64, {});

        for (int i = 0; i < 64; ++i)
        {
            expectEquals (outputL[i], 0.5f);
            expectEquals (outputR[i], 0.25f);
        }
    }

    void testProcessBlockSilenceWhenBufferCleared()
    {
        beginTest ("processBlock - silence when buffer cleared");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

        ctx.engine->registerDeck ("A", &audioState);

        // Set then clear buffer
        auto holder = makeStereoBuffer (0.5f, 0.25f, 256);
        ctx.engine->setDeckBuffer ("A", holder);
        ctx.engine->clearDeckBuffer ("A");

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

    void testProcessBlockSilenceWhenStopped()
    {
        beginTest ("processBlock - silence when deck is stopped even with buffer loaded");
        EngineContext ctx;

        DeckAudioState audioState;
        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::stopped),
                                        std::memory_order_relaxed);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);

        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeStereoBuffer (0.5f, 0.25f, 256);
        ctx.engine->setDeckBuffer ("A", holder);

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
    // Deck state loading properties tests
    // -----------------------------------------------------------------------

    void testInitialLoadingState()
    {
        beginTest ("Deck state - initial loading state: idle, progress 0, error empty");
        DeckContext ctx;
        ctx.mgr->addDeck(); // A

        auto deckTree = ctx.mgr->getDeckState ("A");
        expect (deckTree.isValid());

        expectEquals (deckTree.getProperty (IDs::loadingStatus).toString(),
                      juce::String ("idle"));
        expectEquals (static_cast<float> (deckTree.getProperty (IDs::loadingProgress)),
                      0.0f);
        expectEquals (deckTree.getProperty (IDs::loadingError).toString(),
                      juce::String (""));
    }

    void testResetTrackSpecificStateClearsLoading()
    {
        beginTest ("Deck state - resetTrackSpecificState restores loading to idle");
        DeckContext ctx;
        ctx.mgr->addDeck(); // A

        auto deckTree = ctx.mgr->getDeckState ("A");
        expect (deckTree.isValid());

        // Manually set to "loading"
        deckTree.setProperty (IDs::loadingStatus,   "loading",        nullptr);
        deckTree.setProperty (IDs::loadingProgress,  0.5f,            nullptr);
        deckTree.setProperty (IDs::loadingError,     "some error",    nullptr);

        // ejectTrack triggers resetTrackSpecificState internally
        // First we need to move to a state where eject is allowed
        // loadTrack sets status to "stopped", then ejectTrack calls resetTrackSpecificState
        TrackMetadata meta;
        meta.filePath = "/test.wav";
        meta.title = "Test";
        ctx.mgr->loadTrack ("A", meta);

        // Now eject — this calls resetTrackSpecificState
        expect (ctx.mgr->ejectTrack ("A"));

        // Verify loading state is reset
        auto deckAfter = ctx.mgr->getDeckState ("A");
        expectEquals (deckAfter.getProperty (IDs::loadingStatus).toString(),
                      juce::String ("idle"));
        expectEquals (static_cast<float> (deckAfter.getProperty (IDs::loadingProgress)),
                      0.0f);
        expectEquals (deckAfter.getProperty (IDs::loadingError).toString(),
                      juce::String (""));
    }
};

// Register the test instance
static AudioFileLoaderTests audioFileLoaderTests;
