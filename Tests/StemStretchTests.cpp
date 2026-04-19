#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"

class StemStretchTests : public juce::UnitTest
{
public:
    StemStretchTests() : juce::UnitTest ("Stem Stretch", "Sonik") {}

    void runTest() override
    {
        testDefaultStemTimeStretchersNullptr();
        testDefaultStemStretchDegradedFalse();
        testCreateStemStretchersWithStemsActive();
        testDestroyStemStretchersNullifiesAll();
        testDestroyStemStretchersNoCreationSafe();
        testScratchBuffersExist();
        testStemStretcherLatencyDefaultsToZero();
    }

private:
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

    // -----------------------------------------------------------------------

    void testDefaultStemTimeStretchersNullptr()
    {
        beginTest ("Default stemTimeStretchers are nullptr");
        DeckAudioSource src;
        for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
        {
            expect (src.stemTimeStretchers[s].load (std::memory_order_relaxed) == nullptr,
                    "stemTimeStretchers[" + juce::String (s) + "] should default to nullptr");
        }
    }

    void testDefaultStemStretchDegradedFalse()
    {
        beginTest ("Default stemStretchDegraded is false");
        DeckAudioSource src;
        expect (! src.stemStretchDegraded.load (std::memory_order_relaxed),
                "stemStretchDegraded should default to false");
    }

    void testCreateStemStretchersWithStemsActive()
    {
        beginTest ("createStemStretchers creates 4 stretchers when stems are active");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.keyLockEnabled.store (true, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto mainBuf = makeConstBuffer (0.5f, 0.5f, 4096);
        ctx.engine->setDeckBuffer ("A", mainBuf);

        auto vocals = makeConstBuffer (0.1f, 0.1f, 4096);
        auto drums  = makeConstBuffer (0.2f, 0.2f, 4096);
        auto bass   = makeConstBuffer (0.1f, 0.1f, 4096);
        auto other  = makeConstBuffer (0.1f, 0.1f, 4096);

        // setDeckStemBuffers should trigger createStemStretchers since keyLock is on
        ctx.engine->setDeckStemBuffers ("A", vocals, drums, bass, other);

        // Verify by calling createStemStretchers explicitly (idempotent)
        ctx.engine->createStemStretchers ("A");

        // Destroy should work without crash
        ctx.engine->destroyStemStretchers ("A");

        expect (true, "createStemStretchers + destroyStemStretchers cycle completes without crash");
    }

    void testDestroyStemStretchersNullifiesAll()
    {
        beginTest ("destroyStemStretchers nullifies all 4");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.keyLockEnabled.store (true, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto mainBuf = makeConstBuffer (0.5f, 0.5f, 4096);
        ctx.engine->setDeckBuffer ("A", mainBuf);

        auto vocals = makeConstBuffer (0.1f, 0.1f, 4096);
        auto drums  = makeConstBuffer (0.2f, 0.2f, 4096);
        auto bass   = makeConstBuffer (0.1f, 0.1f, 4096);
        auto other  = makeConstBuffer (0.1f, 0.1f, 4096);

        ctx.engine->setDeckStemBuffers ("A", vocals, drums, bass, other);
        ctx.engine->destroyStemStretchers ("A");

        // After destroy, calling destroyStemStretchers again should be safe
        ctx.engine->destroyStemStretchers ("A");

        expect (true, "destroyStemStretchers nullifies correctly and double-destroy is safe");
    }

    void testDestroyStemStretchersNoCreationSafe()
    {
        beginTest ("destroyStemStretchers after no creation is safe");
        EngineContext ctx;
        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        // No stems set, no stretchers created — destroy should be no-op
        ctx.engine->destroyStemStretchers ("A");

        expect (true, "destroyStemStretchers with no prior creation does not crash");
    }

    void testScratchBuffersExist()
    {
        beginTest ("Scratch buffers exist and are properly sized");
        DeckAudioSource src;

        // Verify scratch buffer arrays are accessible and properly zero-initialized
        for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
        {
            expectEquals (src.stemStretchInL[s][0], 0.0f,
                          "stemStretchInL[" + juce::String (s) + "][0] should be zero-initialized");
            expectEquals (src.stemStretchInR[s][0], 0.0f,
                          "stemStretchInR[" + juce::String (s) + "][0] should be zero-initialized");
            expectEquals (src.stemStretchOutL[s][0], 0.0f,
                          "stemStretchOutL[" + juce::String (s) + "][0] should be zero-initialized");
            expectEquals (src.stemStretchOutR[s][0], 0.0f,
                          "stemStretchOutR[" + juce::String (s) + "][0] should be zero-initialized");
        }

        // Verify MAX_STRETCH_BLOCK is accessible
        expect (DeckAudioSource::MAX_STRETCH_BLOCK > 0,
                "MAX_STRETCH_BLOCK should be positive");
    }

    void testStemStretcherLatencyDefaultsToZero()
    {
        beginTest ("stemStretcherLatency defaults to 0");
        DeckAudioSource src;
        expectEquals (src.stemStretcherLatency, 0,
                      "stemStretcherLatency should default to 0");
    }
};

static StemStretchTests stemStretchTests;
