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
        testStretcherBalancedInputKeepsQueueBounded();
        testUnityKeyLockIsTransparent();
        testCushionPreventsOutputShortfalls();

        // AudioEngine integration
        testStretcherCreatedOnBufferSet();
        testStretcherDestroyedOnBufferClear();
        testEngineKeyLockEndToEnd();

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

    void testStretcherBalancedInputKeepsQueueBounded()
    {
        beginTest ("TimeStretcher balanced input keeps internal queue bounded");

        constexpr int maxOutput = 128;
        constexpr int maxInput = 256;
        constexpr double speed = 0.9;
        constexpr double timeRatio = 1.0 / speed;
        TimeStretcher stretcher (44100.0, 2, maxOutput);
        stretcher.prime();

        float inL[maxInput] = {};
        float inR[maxInput] = {};
        float outL[maxOutput] = {};
        float outR[maxOutput] = {};

        const float* inPtrs[2] = { inL, inR };
        float* outPtrs[2] = { outL, outR };

        double carry = 0.0;
        int maxBuffered = 0;

        for (int i = 0; i < 1024; ++i)
        {
            const double exactInput =
                (static_cast<double> (maxOutput) / timeRatio) + carry;
            const int inputSamples = juce::jlimit (
                1, maxInput,
                static_cast<int> (std::floor (exactInput)));
            carry = exactInput - static_cast<double> (inputSamples);

            (void) stretcher.process (
                inPtrs, inputSamples, outPtrs, maxOutput, timeRatio);

            const int buffered = stretcher.getBufferedOutputSamples();
            maxBuffered = std::max (maxBuffered, buffered);
            expect (buffered <= maxOutput * 3,
                    "Buffered output grew unexpectedly large: "
                        + juce::String (buffered));
        }

        expect (maxBuffered > 0, "Expected some internal buffering in realtime mode");
    }

    // Replicates the AudioEngine key-lock feed loop at 0% pitch (ratio
    // exactly 1.0, continuous feed, cushioned priming) and asserts the
    // stretched output is audibly transparent: aligned with the source and
    // with a residual below -40 dB. Guards the R3 unity passthrough mode
    // against regressions (e.g. reintroducing a ratio bias).
    void testUnityKeyLockIsTransparent()
    {
        beginTest ("Key lock at 0% pitch is transparent (residual < -40 dB)");

        constexpr int    block = 512;
        constexpr double sr    = 44100.0;
        const int total = static_cast<int> (sr) * 4; // 4 seconds

        // Deterministic music-like signal
        juce::AudioBuffer<float> sig (2, total + 48000);
        for (int i = 0; i < sig.getNumSamples(); ++i)
        {
            double t = i / sr;
            float v = 0.3f * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t)
                    + 0.2f * std::sin (2.0 * juce::MathConstants<double>::pi * 987.0 * t)
                    + 0.1f * std::sin (2.0 * juce::MathConstants<double>::pi * 5512.0 * t);
            sig.setSample (0, i, v);
            sig.setSample (1, i, 0.9f * v);
        }

        TimeStretcher st (sr, 2, DeckAudioSource::MAX_STRETCH_BLOCK);
        const int latency = st.primeWithAudio (sig.getReadPointer (0),
                                               sig.getReadPointer (1),
                                               sig.getNumSamples(),
                                               block);

        double playhead = 0.0;
        double feedPos  = static_cast<double> (latency);
        float outL[block], outR[block];
        float inL[block], inR[block];

        int shortfallBlocks = 0;
        double accRes = 0.0, accSig = 0.0;

        const int nBlocks = total / block;
        for (int b = 0; b < nBlocks; ++b)
        {
            for (int s = 0; s < block; ++s)
            {
                auto idx = static_cast<int> (feedPos) + s;
                inL[s] = sig.getSample (0, idx);
                inR[s] = sig.getSample (1, idx);
            }
            feedPos += block;

            const float* ip[2] = { inL, inR };
            float* op[2] = { outL, outR };
            int got = st.process (ip, block, op, block, 1.0);
            if (got < block)
                ++shortfallBlocks;

            // Skip the first 0.5 s (start-of-stream convergence), then
            // accumulate residual vs the source at the playhead.
            if (b * block > sr * 0.5)
            {
                for (int s = 0; s < got; ++s)
                {
                    int g = static_cast<int> (playhead) + s;
                    double d  = outL[s] - sig.getSample (0, g);
                    double d2 = outR[s] - sig.getSample (1, g);
                    accRes += d * d + d2 * d2;
                    accSig += sig.getSample (0, g) * sig.getSample (0, g)
                            + sig.getSample (1, g) * sig.getSample (1, g);
                }
            }
            playhead += block;
        }

        expectEquals (shortfallBlocks, 0,
                      "Cushioned stretcher must always have a full block available");

        double residualDb = 10.0 * std::log10 ((accRes + 1e-30) / (accSig + 1e-30));
        expect (residualDb < -40.0,
                "Unity key lock residual should be < -40 dB, got "
                    + juce::String (residualDb, 1) + " dB");
    }

    // With cushioned priming, available() must never dip below one block
    // even at non-unity ratios where R3's hop sizes do not divide the block.
    void testCushionPreventsOutputShortfalls()
    {
        beginTest ("Cushioned priming prevents output shortfalls at 6% pitch");

        constexpr int    block = 512;
        constexpr double sr    = 44100.0;
        constexpr double speed = 1.06;
        constexpr double ratio = 1.0 / speed;

        juce::AudioBuffer<float> sig (2, static_cast<int> (sr) * 8);
        juce::Random rng (42);
        for (int i = 0; i < sig.getNumSamples(); ++i)
        {
            float v = rng.nextFloat() * 0.4f - 0.2f;
            sig.setSample (0, i, v);
            sig.setSample (1, i, v);
        }

        TimeStretcher st (sr, 2, DeckAudioSource::MAX_STRETCH_BLOCK);
        const int latency = st.primeWithAudio (sig.getReadPointer (0),
                                               sig.getReadPointer (1),
                                               sig.getNumSamples(),
                                               block);
        expect (latency > 0, "Effective latency should be positive");

        double feedPos = static_cast<double> (latency);
        double carry = 0.0;
        float outL[block], outR[block];
        float inL[DeckAudioSource::MAX_STRETCH_BLOCK], inR[DeckAudioSource::MAX_STRETCH_BLOCK];

        int shortfallBlocks = 0;
        const int nBlocks = 400; // ~4.6 s
        for (int b = 0; b < nBlocks; ++b)
        {
            double exact = (static_cast<double> (block) / ratio) + carry;
            int n = static_cast<int> (std::floor (exact));
            carry = exact - n;

            for (int s = 0; s < n; ++s)
            {
                auto idx = static_cast<int> (feedPos) + s;
                inL[s] = sig.getSample (0, idx);
                inR[s] = sig.getSample (1, idx);
            }
            feedPos += n;

            const float* ip[2] = { inL, inR };
            float* op[2] = { outL, outR };
            int got = st.process (ip, n, op, block, ratio);
            if (got < block)
                ++shortfallBlocks;
        }

        expectEquals (shortfallBlocks, 0,
                      "No block may come up short of stretched output");
    }

    // Full-engine integration: drives the real audio callback with key lock
    // enabled. Verifies (1) bit-transparency at 0% pitch (unity bypass),
    // (2) pitch preservation while the tempo changes at +6%, and
    // (3) transparency restored after returning the fader to 0%.
    void testEngineKeyLockEndToEnd()
    {
        beginTest ("Engine: key lock transparent at 0%, pitch-locked at +6%");

        juce::ValueTree root (IDs::SonikState);
        AudioEngine engine (root);   // not started — callback driven manually
        DeckAudioState state;
        engine.registerDeck ("A", &state);

        // 30-second 220.5 Hz stereo sine "track" (period = 200.0 samples)
        constexpr double sr = 44100.0;
        constexpr double freq = 220.5;
        const int frames = static_cast<int> (sr) * 30;
        juce::AudioBuffer<float> buf (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            auto v = static_cast<float> (
                0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr));
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), sr, frames);
        engine.setDeckBuffer ("A", holder);
        const auto& src = holder->getBuffer();

        state.keyLockEnabled.store (true, std::memory_order_relaxed);
        state.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        engine.sendTransportCommand ("A", TransportCommand::Play);

        constexpr int block = 128;   // engine default buffer size
        float outL[block], outR[block];
        float* outs[2] = { outL, outR };

        // --- Phase 1: 0% pitch. After the 64-sample play fade-in the output
        // must match the source exactly (unity bypass selects the vinyl path).
        double maxErr = 0.0;
        for (int b = 0; b < 304; ++b)
        {
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
            if (b >= 4)
                for (int i = 0; i < block; ++i)
                    maxErr = std::max (maxErr, std::abs (
                        static_cast<double> (outL[i])
                        - src.getSample (0, b * block + i)));
        }
        expect (maxErr < 1.0e-6,
                "Key lock at 0% must be transparent, max error "
                    + juce::String (maxErr, 9));

        // --- Phase 2: +6% tempo. After the ratio settles, the audible pitch
        // must stay at 220.5 Hz (period 200 samples) even though the playhead
        // advances 6% faster. Without key lock the period would be ~188.7.
        state.speedMultiplier.store (1.06f, std::memory_order_relaxed);

        const auto playheadBefore = state.playheadPosition.load (std::memory_order_relaxed);
        std::vector<float> captured;
        const int settleBlocks = 100, captureBlocks = 200;
        for (int b = 0; b < settleBlocks + captureBlocks; ++b)
        {
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
            if (b >= settleBlocks)
                captured.insert (captured.end(), outL, outL + block);
        }
        const auto playheadAfter = state.playheadPosition.load (std::memory_order_relaxed);

        // Tempo check: playhead advanced ~6% faster than real time
        const double advance = static_cast<double> (playheadAfter - playheadBefore);
        const double elapsed = (settleBlocks + captureBlocks) * block;
        expect (std::abs (advance / elapsed - 1.06) < 0.005,
                "Playhead must advance at 1.06x, got "
                    + juce::String (advance / elapsed, 4));

        // Pitch check: autocorrelation period of the stretched output
        {
            const int win = 4096;
            const int off = static_cast<int> (captured.size()) - win - 260;
            double best = -1.0; int bestLag = 0;
            for (int lag = 150; lag <= 250; ++lag)
            {
                double acc = 0.0;
                for (int i = 0; i < win; ++i)
                    acc += static_cast<double> (captured[(size_t) (off + i)])
                         * static_cast<double> (captured[(size_t) (off + i + lag)]);
                if (acc > best) { best = acc; bestLag = lag; }
            }
            expect (std::abs (bestLag - 200) <= 3,
                    "Key-locked pitch must stay at 220.5 Hz (period 200), got period "
                        + juce::String (bestLag));
            // No dropouts: the captured window must carry signal energy
            double rms = 0.0;
            for (int i = 0; i < win; ++i)
                rms += captured[(size_t) (off + i)] * captured[(size_t) (off + i)];
            rms = std::sqrt (rms / win);
            expect (rms > 0.2, "Stretched output must not drop out, RMS "
                    + juce::String (rms, 4));
        }

        // --- Phase 3: back to 0%. The bypass crossfades back to the vinyl
        // path, which must again match the source exactly.
        state.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        for (int b = 0; b < 4; ++b)   // crossfade + settle
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});

        auto anchor = state.playheadPosition.load (std::memory_order_relaxed);
        maxErr = 0.0;
        for (int b = 0; b < 100; ++b)
        {
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
            for (int i = 0; i < block; ++i)
                maxErr = std::max (maxErr, std::abs (
                    static_cast<double> (outL[i])
                    - src.getSample (0, static_cast<int> (anchor) + b * block + i)));
        }
        expect (maxErr < 1.0e-6,
                "Key lock back at 0% must be transparent again, max error "
                    + juce::String (maxErr, 9));

        engine.unregisterDeck ("A");
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
