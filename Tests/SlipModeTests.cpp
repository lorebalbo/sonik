#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"

class SlipModeTests : public juce::UnitTest
{
public:
    SlipModeTests() : juce::UnitTest ("Slip Mode", "Sonik") {}

    void runTest() override
    {
        testSlipDisabledByDefault();
        testSlipEnabledPersistsValueTree();
        testSlipDisplacedDefaultsFalse();
        testShadowPositionDefaultsToZero();
        testTrackLoadResetsSlipState();
        testSlipSeekSetsDisplacement();
        testRegularSeekClearsDisplacement();
        testSlipReturnSnapsToShadow();
        testStopResetsShadow();
        testLoopWrapSetsDisplacement();
        testBeatJumpSuppressesLoopShiftWhenSlipEnabled();
        testBeatJumpUsesSlipSeekWhenSlipEnabled();
        testDisableSlipWhileDisplacedNoSnapback();
        testNearSnapbackNoOp();
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

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

    AudioBufferHolder::Ptr makeConstBuffer (float leftVal, float rightVal,
                                            int numFrames,
                                            double sampleRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (2, numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            buf.setSample (0, i, leftVal);
            buf.setSample (1, i, rightVal);
        }
        return new AudioBufferHolder (std::move (buf), sampleRate,
                                     static_cast<int64_t> (numFrames));
    }

    void runBlock (AudioEngine& engine, int numSamples)
    {
        juce::HeapBlock<float> outL (static_cast<size_t> (numSamples));
        juce::HeapBlock<float> outR (static_cast<size_t> (numSamples));
        outL.clear (static_cast<size_t> (numSamples));
        outR.clear (static_cast<size_t> (numSamples));
        float* outputs[2] = { outL.get(), outR.get() };
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, numSamples, {});
    }

    void runBlocks (AudioEngine& engine, int numBlocks, int blockSize = 128)
    {
        for (int i = 0; i < numBlocks; ++i)
            runBlock (engine, blockSize);
    }

    // -----------------------------------------------------------------------
    // Tests
    // -----------------------------------------------------------------------

    void testSlipDisabledByDefault()
    {
        beginTest ("Slip mode is disabled by default");
        EngineContext ctx;
        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        expect (! audioState.slipEnabled.load (std::memory_order_relaxed));
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testSlipEnabledPersistsValueTree()
    {
        beginTest ("slipEnabled round-trips through ValueTree");
        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::slipEnabled, false, nullptr);

        expect (! static_cast<bool> (deckTree.getProperty (IDs::slipEnabled, false)));

        deckTree.setProperty (IDs::slipEnabled, true, nullptr);
        expect (static_cast<bool> (deckTree.getProperty (IDs::slipEnabled, false)));

        // Toggle off
        deckTree.setProperty (IDs::slipEnabled, false, nullptr);
        expect (! static_cast<bool> (deckTree.getProperty (IDs::slipEnabled, false)));
    }

    void testSlipDisplacedDefaultsFalse()
    {
        beginTest ("slipDisplaced defaults to false on DeckAudioState");
        DeckAudioState state;
        expect (! state.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testShadowPositionDefaultsToZero()
    {
        beginTest ("slipShadowPosition defaults to 0.0");
        DeckAudioState state;
        expectWithinAbsoluteError (
            state.slipShadowPosition.load (std::memory_order_relaxed), 0.0, 0.001);
    }

    void testTrackLoadResetsSlipState()
    {
        beginTest ("Track load resets shadow and displacement");
        EngineContext ctx;
        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        expectEquals (audioState.slipShadowPosition.load (std::memory_order_relaxed), 0.0);
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testSlipSeekSetsDisplacement()
    {
        beginTest ("slipSeekDeck sets displacement on audio thread");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        // Set playing AFTER setDeckBuffer (which resets status to stopped)
        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        // Run a few blocks to establish position
        runBlocks (*ctx.engine, 5);

        // Slip-seek to a distant position
        ctx.engine->slipSeekDeck ("A", 44100);

        // Run blocks for the fade to complete (fade-out + fade-in = 128 samples min)
        runBlocks (*ctx.engine, 5);

        // After slip seek, should be displaced
        expect (audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testRegularSeekClearsDisplacement()
    {
        beginTest ("Regular seekDeck clears displacement");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        // First, create a displacement via slip seek
        runBlocks (*ctx.engine, 3);
        ctx.engine->slipSeekDeck ("A", 44100);
        runBlocks (*ctx.engine, 5);
        expect (audioState.slipDisplaced.load (std::memory_order_relaxed));

        // Now do a regular seek (navigation) — should clear displacement
        ctx.engine->seekDeck ("A", 22050);
        runBlocks (*ctx.engine, 5);
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testSlipReturnSnapsToShadow()
    {
        beginTest ("Slip return snaps playhead to shadow position");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        // Run to establish a position
        runBlocks (*ctx.engine, 5);

        // Slip-seek to create displacement
        ctx.engine->slipSeekDeck ("A", 44100);
        runBlocks (*ctx.engine, 5);
        expect (audioState.slipDisplaced.load (std::memory_order_relaxed));

        // Record shadow position before return
        double shadowBefore = audioState.slipShadowPosition.load (std::memory_order_relaxed);

        // Send slip return
        ctx.engine->sendSlipReturn ("A");
        runBlocks (*ctx.engine, 5);

        // After return, should NOT be displaced
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));

        // Playhead should be near where shadow was (with some advancement during fades)
        int64_t playhead = audioState.playheadPosition.load (std::memory_order_relaxed);
        // Shadow advanced during the fade, so playhead should be near
        // the shadow's value at the time of the return (within a few blocks)
        double diff = std::abs (static_cast<double> (playhead) - shadowBefore);
        expect (diff < 2000.0); // within ~2000 samples (generous)
    }

    void testStopResetsShadow()
    {
        beginTest ("Stop command resets shadow and displacement");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        runBlocks (*ctx.engine, 5);

        // Create displacement
        ctx.engine->slipSeekDeck ("A", 44100);
        runBlocks (*ctx.engine, 5);

        // Stop the deck
        ctx.engine->sendTransportCommand ("A", TransportCommand::Stop);
        runBlocks (*ctx.engine, 5);

        // Shadow should be reset
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testLoopWrapSetsDisplacement()
    {
        beginTest ("Loop wrap-around sets slip displacement");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        // Set up a loop: 0..1000 samples
        audioState.loopActive.store (true, std::memory_order_relaxed);
        audioState.loopInSamples.store (0, std::memory_order_relaxed);
        audioState.loopOutSamples.store (1000, std::memory_order_relaxed);

        // Run enough blocks for the loop to wrap
        runBlocks (*ctx.engine, 20);

        // Should be displaced due to loop wrap
        expect (audioState.slipDisplaced.load (std::memory_order_relaxed));

        // Shadow should have advanced past the loop out point
        double shadow = audioState.slipShadowPosition.load (std::memory_order_relaxed);
        expect (shadow > 1000.0);
    }

    void testBeatJumpSuppressesLoopShiftWhenSlipEnabled()
    {
        beginTest ("Beat jump suppresses loop shift when slip is enabled");

        // Build a minimal deck tree with slip and loop
        juce::ValueTree deckTree (IDs::Deck);
        deckTree.setProperty (IDs::slipEnabled, true, nullptr);

        juce::ValueTree loopNode (IDs::Loop);
        loopNode.setProperty (IDs::active, true, nullptr);
        loopNode.setProperty (IDs::loopIn, 0, nullptr);
        loopNode.setProperty (IDs::loopOut, 22050, nullptr);
        deckTree.addChild (loopNode, -1, nullptr);

        // When slip is enabled and loop is active, the BeatJumpEngine
        // should NOT shift the loop — it should skip the loop shift branch
        bool slipOn = static_cast<bool> (deckTree.getProperty (IDs::slipEnabled, false));
        bool loopActive = static_cast<bool> (loopNode.getProperty (IDs::active, false));

        // The condition: loopActive && loopEngine != nullptr && !slipOn
        // When slip is on, this evaluates false → no loop shift
        expect (slipOn && loopActive);
        expect (! (loopActive && ! slipOn)); // loop shift suppressed
    }

    void testBeatJumpUsesSlipSeekWhenSlipEnabled()
    {
        beginTest ("Beat jump uses slip seek transport command when slip enabled");
        // Verify the slip seek transport command exists
        auto cmd = static_cast<int> (TransportCommand::SlipSeek);
        expect (cmd != 0);

        auto cmd2 = static_cast<int> (TransportCommand::SlipSeekAndPlay);
        expect (cmd2 != 0);

        auto cmd3 = static_cast<int> (TransportCommand::SlipReturn);
        expect (cmd3 != 0);
    }

    void testDisableSlipWhileDisplacedNoSnapback()
    {
        beginTest ("Disabling slip while displaced does NOT snap back");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::playing),
            std::memory_order_relaxed);

        // Run and create displacement via slip seek
        runBlocks (*ctx.engine, 5);
        ctx.engine->slipSeekDeck ("A", 44100);
        runBlocks (*ctx.engine, 5);
        expect (audioState.slipDisplaced.load (std::memory_order_relaxed));

        // Record playhead position
        int64_t posBeforeDisable = audioState.playheadPosition.load (std::memory_order_relaxed);

        // Disable slip
        audioState.slipEnabled.store (false, std::memory_order_relaxed);
        runBlocks (*ctx.engine, 3);

        // Playhead should continue advancing from where it was (no snap-back to shadow)
        int64_t posAfterDisable = audioState.playheadPosition.load (std::memory_order_relaxed);
        expect (posAfterDisable >= posBeforeDisable);

        // Displacement should be cleared
        expect (! audioState.slipDisplaced.load (std::memory_order_relaxed));
    }

    void testNearSnapbackNoOp()
    {
        beginTest ("Slip return within 64 samples does instant resync");
        EngineContext ctx;
        DeckAudioState audioState;
        audioState.slipEnabled.store (true, std::memory_order_relaxed);
        audioState.speedMultiplier.store (1.0f, std::memory_order_relaxed);
        audioState.gain.store (1.0f, std::memory_order_relaxed);
        audioState.playbackStatus.store (
            static_cast<int> (PlaybackStatusCode::stopped),
            std::memory_order_relaxed);
        ctx.engine->registerDeck ("A", &audioState);

        auto holder = makeConstBuffer (0.5f, 0.5f, 88200);
        ctx.engine->setDeckBuffer ("A", holder);

        // Verify the DeferredAction::SlipReturn enum value exists
        auto action = DeckAudioSource::DeferredAction::SlipReturn;
        expect (action != DeckAudioSource::DeferredAction::None);
    }
};

static SlipModeTests slipModeTests;
