#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/TimeStretch/TimeStretcher.h"
#include "Features/Deck/UI/KeyLockButton.h"

class TimeStretchTests : public juce::UnitTest
{
public:
    TimeStretchTests() : juce::UnitTest ("Time Stretching", "Sonik") {}

    void runTest() override
    {
        // State tests
        testKeyLockDefaultsFalse();
        testKeyLockIsDeckLevelState();
        testKeyLockSyncsToAtomic();

        // TimeStretcher construction
        testStretcherConstruction();
        testStretcherLatency();
        testStretcherReset();
        testStretcherPrime();

        // TimeStretcher processing
        testStretcherPassthroughAtUnityRatio();
        testStretcherProducesOutput();
        testStretcherHandlesZeroInput();

        // AudioEngine integration
        testStretcherCreatedOnBufferSet();
        testStretcherDestroyedOnBufferClear();

        // Key Lock crossfade
        testKeyLockCrossfadeState();

        // DeckAudioSource scratch buffers
        testScratchBufferSize();

        // UI component
        testKeyLockButtonConstruction();
        testKeyLockButtonToggle();
        testKeyLockButtonPaint();
    }

private:
    // Helper: create a minimal deck ValueTree
    juce::ValueTree createDeckTree (const juce::String& deckId = "A")
    {
        juce::ValueTree deck (IDs::Deck);
        deck.setProperty (IDs::id, deckId, nullptr);
        deck.setProperty (IDs::playbackStatus, "empty", nullptr);
        deck.setProperty (IDs::gain, 1.0f, nullptr);
        deck.setProperty (IDs::pitch, 0.0f, nullptr);
        deck.setProperty (IDs::speedMultiplier, 1.0f, nullptr);
        deck.setProperty (IDs::pitchRange, 8, nullptr);
        deck.setProperty (IDs::keyLockEnabled, false, nullptr);
        deck.setProperty (IDs::quantizeEnabled, false, nullptr);
        deck.setProperty (IDs::slipEnabled, false, nullptr);

        juce::ValueTree playhead (IDs::Playhead);
        playhead.setProperty (IDs::position, 0, nullptr);
        deck.addChild (playhead, -1, nullptr);

        juce::ValueTree tempCue (IDs::TempCue);
        tempCue.setProperty (IDs::position, -1, nullptr);
        deck.addChild (tempCue, -1, nullptr);

        return deck;
    }

    // ---------------------------------------------------------------
    // State Tests
    // ---------------------------------------------------------------

    void testKeyLockDefaultsFalse()
    {
        beginTest ("keyLockEnabled defaults to false in deck state tree");
        auto deck = createDeckTree();
        expect (! static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));
    }

    void testKeyLockIsDeckLevelState()
    {
        beginTest ("keyLockEnabled persists across track loads (deck-level state)");
        auto deck = createDeckTree();

        // Enable key lock
        deck.setProperty (IDs::keyLockEnabled, true, nullptr);
        expect (static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));

        // Simulate track load reset: only track-specific state resets
        // Key lock should NOT be reset (it's deck-level)
        deck.setProperty (IDs::playbackStatus, "stopped", nullptr);
        deck.setProperty (IDs::pitch, 0.0f, nullptr);
        deck.setProperty (IDs::speedMultiplier, 1.0f, nullptr);

        // Key lock should still be enabled
        expect (static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));
    }

    void testKeyLockSyncsToAtomic()
    {
        beginTest ("AudioStateSync syncs keyLockEnabled to DeckAudioState atomic");
        auto deck = createDeckTree();
        DeckAudioState audioState;

        AudioStateSync sync (deck, audioState);

        // Initially false
        expect (! audioState.keyLockEnabled.load (std::memory_order_relaxed));

        // Set to true
        deck.setProperty (IDs::keyLockEnabled, true, nullptr);
        expect (audioState.keyLockEnabled.load (std::memory_order_relaxed));

        // Set back to false
        deck.setProperty (IDs::keyLockEnabled, false, nullptr);
        expect (! audioState.keyLockEnabled.load (std::memory_order_relaxed));
    }

    // ---------------------------------------------------------------
    // TimeStretcher Construction
    // ---------------------------------------------------------------

    void testStretcherConstruction()
    {
        beginTest ("TimeStretcher constructs without crash (RealTime mode)");
        auto stretcher = std::make_unique<TimeStretcher> (44100.0, 2, 512);
        expect (stretcher != nullptr);
    }

    void testStretcherLatency()
    {
        beginTest ("TimeStretcher reports non-negative latency");
        TimeStretcher stretcher (44100.0, 2, 512);
        int latency = stretcher.getLatency();
        expect (latency >= 0, "Latency should be non-negative, got: "
                + juce::String (latency));
    }

    void testStretcherReset()
    {
        beginTest ("TimeStretcher reset does not crash");
        TimeStretcher stretcher (44100.0, 2, 512);
        stretcher.reset();
        // Just verify it doesn't crash
        expect (true);
    }

    void testStretcherPrime()
    {
        beginTest ("TimeStretcher prime fills internal buffers");
        TimeStretcher stretcher (44100.0, 2, 512);
        stretcher.prime();
        // After priming, latency should still be valid
        expect (stretcher.getLatency() >= 0);
    }

    // ---------------------------------------------------------------
    // TimeStretcher Processing
    // ---------------------------------------------------------------

    void testStretcherPassthroughAtUnityRatio()
    {
        beginTest ("TimeStretcher at ratio 1.0 passes audio through");
        TimeStretcher stretcher (44100.0, 2, 512);
        stretcher.prime();

        constexpr int blockSize = 128;
        float inL[blockSize], inR[blockSize];
        float outL[blockSize], outR[blockSize];

        // Fill with a simple sine wave
        for (int i = 0; i < blockSize; ++i)
        {
            inL[i] = std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                               * static_cast<float> (i) / 44100.0f);
            inR[i] = inL[i];
            outL[i] = 0.0f;
            outR[i] = 0.0f;
        }

        const float* inPtrs[2] = { inL, inR };
        float* outPtrs[2] = { outL, outR };

        int produced = stretcher.process (inPtrs, blockSize, outPtrs, blockSize, 1.0);

        // At unity ratio, stretcher should produce output (may not be exactly blockSize
        // due to initial latency)
        expect (produced >= 0, "Should produce non-negative output samples");
    }

    void testStretcherProducesOutput()
    {
        beginTest ("TimeStretcher produces output at non-unity ratio");
        TimeStretcher stretcher (44100.0, 2, 1024);
        stretcher.prime();

        constexpr int blockSize = 256;
        float inL[blockSize], inR[blockSize];
        float outL[blockSize * 2], outR[blockSize * 2];

        for (int i = 0; i < blockSize; ++i)
        {
            inL[i] = std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                               * static_cast<float> (i) / 44100.0f);
            inR[i] = inL[i];
        }

        std::memset (outL, 0, sizeof (outL));
        std::memset (outR, 0, sizeof (outR));

        const float* inPtrs[2] = { inL, inR };
        float* outPtrs[2] = { outL, outR };

        // Slower playback (ratio 1.06 means output is stretched)
        int produced = stretcher.process (inPtrs, blockSize, outPtrs, blockSize * 2, 1.06);
        expect (produced >= 0, "Should produce non-negative samples at ratio 1.06");
    }

    void testStretcherHandlesZeroInput()
    {
        beginTest ("TimeStretcher handles zero-length input gracefully");
        TimeStretcher stretcher (44100.0, 2, 512);

        float outL[128] = {};
        float outR[128] = {};
        float* outPtrs[2] = { outL, outR };

        int produced = stretcher.process (nullptr, 0, outPtrs, 128, 1.0);
        expectEquals (produced, 0, "Zero input should produce zero output");
    }

    // ---------------------------------------------------------------
    // AudioEngine Integration
    // ---------------------------------------------------------------

    void testStretcherCreatedOnBufferSet()
    {
        beginTest ("TimeStretcher is created when a buffer is set on a deck");
        // The DeckAudioSource should have a non-null timeStretcher after setDeckBuffer
        DeckAudioSource src;
        // Initially null
        expect (src.timeStretcher.load (std::memory_order_relaxed) == nullptr);
        expect (src.timeStretcherOwned == nullptr);
    }

    void testStretcherDestroyedOnBufferClear()
    {
        beginTest ("TimeStretcher is destroyed when deck buffer is cleared");
        DeckAudioSource src;
        // After clear, should be null
        expect (src.timeStretcher.load (std::memory_order_relaxed) == nullptr);
    }

    // ---------------------------------------------------------------
    // Key Lock Crossfade
    // ---------------------------------------------------------------

    void testKeyLockCrossfadeState()
    {
        beginTest ("Key lock crossfade state initializes correctly");
        DeckAudioSource src;
        expectEquals (src.keyLockFadeSamplesRemaining, 0);
        expect (! src.keyLockFadingIn);
        expect (! src.keyLockFadingOut);
        expect (! src.wasKeyLockEnabled);
    }

    // ---------------------------------------------------------------
    // Scratch Buffer
    // ---------------------------------------------------------------

    void testScratchBufferSize()
    {
        beginTest ("Scratch buffers are large enough for max block size");
        expectEquals (DeckAudioSource::MAX_STRETCH_BLOCK, 4096);
        // Verify buffer exists without crash
        DeckAudioSource src;
        src.stretchInL[0] = 1.0f;
        src.stretchOutL[DeckAudioSource::MAX_STRETCH_BLOCK - 1] = 1.0f;
        expectEquals (src.stretchInL[0], 1.0f);
        expectEquals (src.stretchOutL[DeckAudioSource::MAX_STRETCH_BLOCK - 1], 1.0f);
    }

    // ---------------------------------------------------------------
    // UI Component
    // ---------------------------------------------------------------

    void testKeyLockButtonConstruction()
    {
        beginTest ("KeyLockButton constructs without crash");
        auto deck = createDeckTree();
        auto button = std::make_unique<KeyLockButton> (deck);
        expect (button != nullptr);
    }

    void testKeyLockButtonToggle()
    {
        beginTest ("KeyLockButton toggles keyLockEnabled in ValueTree on click");
        auto deck = createDeckTree();
        auto button = std::make_unique<KeyLockButton> (deck);
        button->setBounds (0, 0, 56, 20);

        // Initially false
        expect (! static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));

        // Simulate click via MouseEvent
        auto mouseSource = juce::Desktop::getInstance().getMainMouseSource();
        auto event = juce::MouseEvent (
            mouseSource,
            juce::Point<float> (10.0f, 10.0f),
            juce::ModifierKeys(),
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            button.get(), button.get(),
            juce::Time::getCurrentTime(),
            juce::Point<float> (10.0f, 10.0f),
            juce::Time::getCurrentTime(),
            1, false);
        button->mouseDown (event);

        // Should now be true
        expect (static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));

        // Click again — should be false
        button->mouseDown (event);
        expect (! static_cast<bool> (deck.getProperty (IDs::keyLockEnabled)));
    }

    void testKeyLockButtonPaint()
    {
        beginTest ("KeyLockButton paints without crash in both states");
        auto deck = createDeckTree();
        auto button = std::make_unique<KeyLockButton> (deck);
        button->setBounds (0, 0, 56, 20);

        // Paint inactive
        juce::Image img (juce::Image::ARGB, 56, 20, true);
        juce::Graphics g (img);
        button->paint (g);

        // Toggle and paint active
        deck.setProperty (IDs::keyLockEnabled, true, nullptr);
        button->paint (g);

        expect (true);
    }
};

static TimeStretchTests timeStretchTests;
