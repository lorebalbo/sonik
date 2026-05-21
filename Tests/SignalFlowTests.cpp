//==============================================================================
// PRD-0053: SignalFlowTests
//
// Verifies that the new pipeline skeleton (ChannelStripProcessor → ABBus →
// CrossfaderStage → MasterStage) produces output bit-equivalent to the
// pre-refactor PRD-0002 direct per-deck sum for all representative input
// signals, within a floating-point tolerance of 1e-6 per sample.
//
// Test topology mirrors AudioEngine::audioDeviceIOCallbackWithContext:
//   1. Fill per-deck scratch buffers with a known signal.
//   2. Run ChannelStripProcessor (identity copy → channelScratch).
//   3. Run ABBus (accumulate channelScratch into both buses at unity).
//   4. Run CrossfaderStage (masterScratch = 0.5 * (busA + busB)).
//   5. Run MasterStage (output = masterScratch).
//   Compare output with a direct sum of the same deck buffers.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Features/Mixer/Routing/ChannelStripSnapshot.h"
#include "Features/Mixer/Routing/CrossfaderSnapshot.h"
#include "Features/Mixer/Routing/MasterSnapshot.h"
#include "Features/Mixer/Routing/ChannelStripProcessor.h"
#include "Features/Mixer/Routing/ABBus.h"
#include "Features/Mixer/Routing/CrossfaderStage.h"
#include "Features/Mixer/Routing/MasterStage.h"
#include <array>
#include <vector>
#include <cmath>

class SignalFlowTests final : public juce::UnitTest
{
public:
    SignalFlowTests() : juce::UnitTest ("SignalFlow", "Sonik") {}

    void runTest() override
    {
        runSilenceTest();
        runSingleDeckTest();
        runTwoDeckMixedSineTest();
        runAllFourDecksTest();
        runNearClippingLevelTest();
    }

private:
    static constexpr int   kBlockSize = 512;
    static constexpr float kTolerance = 1e-6f;

    //--------------------------------------------------------------------------
    // Helper: run the pipeline for `numActiveDecks` decks with given input
    // and compare to a reference direct sum.
    //
    // deckInputL/R are arrays of 4 pointers (may be nullptr for inactive decks).
    //--------------------------------------------------------------------------
    void runPipelineAndCompare (const char*                testName,
                                const float* const         deckInputL[4],
                                const float* const         deckInputR[4],
                                int                        numSamples)
    {
        // --- Allocate processing buffers ---
        std::array<std::vector<float>, 4> deckScrL, deckScrR, chScrL, chScrR;
        for (int i = 0; i < 4; ++i)
        {
            deckScrL[static_cast<size_t>(i)].resize (static_cast<size_t>(numSamples), 0.0f);
            deckScrR[static_cast<size_t>(i)].resize (static_cast<size_t>(numSamples), 0.0f);
            chScrL[static_cast<size_t>(i)].resize   (static_cast<size_t>(numSamples), 0.0f);
            chScrR[static_cast<size_t>(i)].resize   (static_cast<size_t>(numSamples), 0.0f);
        }

        std::vector<float> busAL (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> busAR (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> busBL (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> busBR (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> masterL (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> masterR (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> outL (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> outR (static_cast<size_t>(numSamples), 0.0f);

        // --- Reference: direct per-deck sum ---
        std::vector<float> refL (static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> refR (static_cast<size_t>(numSamples), 0.0f);
        for (int slot = 0; slot < 4; ++slot)
        {
            if (deckInputL[slot] == nullptr) continue;
            juce::FloatVectorOperations::add (refL.data(), deckInputL[slot], numSamples);
            juce::FloatVectorOperations::add (refR.data(), deckInputR[slot], numSamples);
        }

        // --- Pipeline stages ---
        std::array<ChannelStripProcessor, 4> strips;
        CrossfaderStage xfStage;
        MasterStage     mxStage;

        for (int slot = 0; slot < 4; ++slot)
            strips[static_cast<size_t>(slot)].prepareToPlay (44100.0, numSamples, 2);
        xfStage.prepareToPlay (44100.0, numSamples, 2);
        mxStage.prepareToPlay (44100.0, numSamples, 2);

        for (int slot = 0; slot < 4; ++slot)
        {
            if (deckInputL[slot] == nullptr) continue;

            // Copy deck inputs to deck scratch (simulating what the deck loop does)
            juce::FloatVectorOperations::copy (
                deckScrL[static_cast<size_t>(slot)].data(), deckInputL[slot], numSamples);
            juce::FloatVectorOperations::copy (
                deckScrR[static_cast<size_t>(slot)].data(), deckInputR[slot], numSamples);

            ChannelStripSnapshot chSnap;  // all defaults
            strips[static_cast<size_t>(slot)].process (
                deckScrL[static_cast<size_t>(slot)].data(),
                deckScrR[static_cast<size_t>(slot)].data(),
                chScrL[static_cast<size_t>(slot)].data(),
                chScrR[static_cast<size_t>(slot)].data(),
                numSamples, chSnap);

            ABBus::accumulate (
                chScrL[static_cast<size_t>(slot)].data(),
                chScrR[static_cast<size_t>(slot)].data(),
                true, true,
                busAL.data(), busAR.data(),
                busBL.data(), busBR.data(),
                numSamples);
        }

        CrossfaderSnapshot xfSnap;
        // PRD-0057: drive the stage with the internal Linear curve so the
        // pre-PRD-0057 "pipeline == direct sum" invariant still holds when
        // every channel is dual-assigned (assignA == assignB == true): both
        // buses carry channelSum, and gainA + gainB == 1 keeps the output
        // identical to the reference direct sum. The Smooth/Sharp DSP
        // behaviours are exercised by CrossfaderTests instead.
        xfSnap.curve = CrossfaderCurve::Linear;
        xfStage.process (busAL.data(), busAR.data(),
                         busBL.data(), busBR.data(),
                         masterL.data(), masterR.data(),
                         numSamples, xfSnap);

        MasterSnapshot mxSnap;
        mxStage.process (masterL.data(), masterR.data(),
                         outL.data(), outR.data(),
                         numSamples, mxSnap);

        // --- Compare pipeline output to reference direct sum ---
        for (int i = 0; i < numSamples; ++i)
        {
            float diffL = std::abs (outL[static_cast<size_t>(i)] - refL[static_cast<size_t>(i)]);
            float diffR = std::abs (outR[static_cast<size_t>(i)] - refR[static_cast<size_t>(i)]);

            if (diffL > kTolerance || diffR > kTolerance)
            {
                juce::String msg;
                msg << testName << ": mismatch at sample " << i
                    << "  pipelineL=" << outL[static_cast<size_t>(i)]
                    << "  refL=" << refL[static_cast<size_t>(i)]
                    << "  diffL=" << diffL;
                expect (false, msg);
                return;   // first mismatch is enough to report
            }
        }

        expect (true, juce::String (testName) + ": all " + juce::String (numSamples) + " samples match");
    }

    //--------------------------------------------------------------------------
    // Test 1: All inputs are zero — output must be zero.
    //--------------------------------------------------------------------------
    void runSilenceTest()
    {
        beginTest ("Pipeline: silence produces zero output");

        std::vector<float> zero (static_cast<size_t>(kBlockSize), 0.0f);

        const float* inL[4] = { zero.data(), zero.data(), zero.data(), zero.data() };
        const float* inR[4] = { zero.data(), zero.data(), zero.data(), zero.data() };
        runPipelineAndCompare ("Silence", inL, inR, kBlockSize);
    }

    //--------------------------------------------------------------------------
    // Test 2: Single active deck with a sine wave — output equals the sine.
    //--------------------------------------------------------------------------
    void runSingleDeckTest()
    {
        beginTest ("Pipeline: single deck matches direct sum");

        std::vector<float> sineL (static_cast<size_t>(kBlockSize));
        std::vector<float> sineR (static_cast<size_t>(kBlockSize));
        for (int i = 0; i < kBlockSize; ++i)
        {
            sineL[static_cast<size_t>(i)] = std::sin (static_cast<float>(i) * 0.1f) * 0.5f;
            sineR[static_cast<size_t>(i)] = std::sin (static_cast<float>(i) * 0.1f + 0.3f) * 0.5f;
        }

        // Only slot 0 is active
        const float* inL[4] = { sineL.data(), nullptr, nullptr, nullptr };
        const float* inR[4] = { sineR.data(), nullptr, nullptr, nullptr };
        runPipelineAndCompare ("SingleDeck", inL, inR, kBlockSize);
    }

    //--------------------------------------------------------------------------
    // Test 3: Two active decks with independent sine waves — output equals
    // the direct sum of both signals.
    //--------------------------------------------------------------------------
    void runTwoDeckMixedSineTest()
    {
        beginTest ("Pipeline: two decks mixed sines match direct sum");

        std::vector<float> sineA_L (static_cast<size_t>(kBlockSize));
        std::vector<float> sineA_R (static_cast<size_t>(kBlockSize));
        std::vector<float> sineB_L (static_cast<size_t>(kBlockSize));
        std::vector<float> sineB_R (static_cast<size_t>(kBlockSize));

        for (int i = 0; i < kBlockSize; ++i)
        {
            sineA_L[static_cast<size_t>(i)] =
                std::sin (static_cast<float>(i) * 0.08f) * 0.4f;
            sineA_R[static_cast<size_t>(i)] =
                std::sin (static_cast<float>(i) * 0.08f + 0.5f) * 0.4f;
            sineB_L[static_cast<size_t>(i)] =
                std::sin (static_cast<float>(i) * 0.13f) * 0.3f;
            sineB_R[static_cast<size_t>(i)] =
                std::sin (static_cast<float>(i) * 0.13f + 1.0f) * 0.3f;
        }

        // Slots 0 and 1 active
        const float* inL[4] = { sineA_L.data(), sineB_L.data(), nullptr, nullptr };
        const float* inR[4] = { sineA_R.data(), sineB_R.data(), nullptr, nullptr };
        runPipelineAndCompare ("TwoDeckMixedSines", inL, inR, kBlockSize);
    }

    //--------------------------------------------------------------------------
    // Test 4: All four decks active — output equals the sum of all four.
    //--------------------------------------------------------------------------
    void runAllFourDecksTest()
    {
        beginTest ("Pipeline: four decks match direct sum");

        constexpr int kN = static_cast<int>(kBlockSize);
        std::array<std::vector<float>, 4> dL, dR;
        const float freqs[4] = { 0.07f, 0.11f, 0.17f, 0.23f };
        for (int slot = 0; slot < 4; ++slot)
        {
            dL[static_cast<size_t>(slot)].resize (static_cast<size_t>(kN));
            dR[static_cast<size_t>(slot)].resize (static_cast<size_t>(kN));
            for (int i = 0; i < kN; ++i)
            {
                dL[static_cast<size_t>(slot)][static_cast<size_t>(i)] =
                    std::sin (static_cast<float>(i) * freqs[slot]) * 0.2f;
                dR[static_cast<size_t>(slot)][static_cast<size_t>(i)] =
                    std::cos (static_cast<float>(i) * freqs[slot]) * 0.2f;
            }
        }

        const float* inL[4] = {
            dL[0].data(), dL[1].data(), dL[2].data(), dL[3].data()
        };
        const float* inR[4] = {
            dR[0].data(), dR[1].data(), dR[2].data(), dR[3].data()
        };
        runPipelineAndCompare ("AllFourDecks", inL, inR, kBlockSize);
    }

    //--------------------------------------------------------------------------
    // Test 5: Near-clipping levels — pipeline must not saturate or truncate
    // before the hard-clip stage (which is tested in AudioEngineTests).
    //--------------------------------------------------------------------------
    void runNearClippingLevelTest()
    {
        beginTest ("Pipeline: near-clipping levels match direct sum");

        std::vector<float> hotL (static_cast<size_t>(kBlockSize));
        std::vector<float> hotR (static_cast<size_t>(kBlockSize));

        for (int i = 0; i < kBlockSize; ++i)
        {
            // 0.95 amplitude sine — well below the PRD-0002 hard-clip at 1.0
            hotL[static_cast<size_t>(i)] =
                std::sin (static_cast<float>(i) * 0.05f) * 0.95f;
            hotR[static_cast<size_t>(i)] =
                std::cos (static_cast<float>(i) * 0.05f) * 0.95f;
        }

        const float* inL[4] = { hotL.data(), nullptr, nullptr, nullptr };
        const float* inR[4] = { hotR.data(), nullptr, nullptr, nullptr };
        runPipelineAndCompare ("NearClipping", inL, inR, kBlockSize);
    }
};

static SignalFlowTests gSignalFlowTests;
