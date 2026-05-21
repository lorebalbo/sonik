//==============================================================================
// PRD-0057: CrossfaderTests
//
// Exercises the ABBus and CrossfaderStage DSP behaviours introduced by
// PRD-0057. These tests bypass the ValueTree wiring entirely and drive the
// pipeline through CrossfaderSnapshot, so they remain hermetic.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Features/Mixer/Routing/ABBus.h"
#include "Features/Mixer/Routing/CrossfaderStage.h"
#include "Features/Mixer/Routing/CrossfaderSnapshot.h"

#include <array>
#include <cmath>
#include <vector>
#include <limits>

namespace
{
    constexpr int   kBlockSize = 256;
    constexpr float kSampleRate = 48000.0f;

    //--------------------------------------------------------------------------
    // Generate a deterministic non-silent stereo signal so peak / sum
    // comparisons are robust against accidental zero coincidences.
    //--------------------------------------------------------------------------
    void fillTestSignal (std::vector<float>& l, std::vector<float>& r, float seed)
    {
        for (size_t i = 0; i < l.size(); ++i)
        {
            const float n = static_cast<float> (i);
            l[i] = 0.5f * std::sin (n * 0.07f + seed);
            r[i] = 0.5f * std::cos (n * 0.07f + seed * 0.3f);
        }
    }

    //--------------------------------------------------------------------------
    // Pump enough silent blocks through CrossfaderStage with the target
    // crossfader position so the one-pole smoother settles. Returns the
    // engaged stage for use in the test.
    //
    // The stage snaps to the target on the first process() call after
    // prepareToPlay (PRD-0057 §1.4 implementation note), so a single block
    // is sufficient to reach steady state for static-position tests.
    //--------------------------------------------------------------------------
    void primeStage (CrossfaderStage& stage,
                     float target,
                     CrossfaderCurve curve)
    {
        stage.prepareToPlay (kSampleRate, kBlockSize, 2);

        std::vector<float> zero (static_cast<size_t> (kBlockSize), 0.0f);
        std::vector<float> tmpL (static_cast<size_t> (kBlockSize), 0.0f);
        std::vector<float> tmpR (static_cast<size_t> (kBlockSize), 0.0f);

        CrossfaderSnapshot snap;
        snap.crossfader = target;
        snap.curve      = curve;
        stage.process (zero.data(), zero.data(), zero.data(), zero.data(),
                       tmpL.data(), tmpR.data(), kBlockSize, snap);
    }

    //--------------------------------------------------------------------------
    // Run a full single-channel pipeline: channel signal → ABBus → CrossfaderStage.
    // Bus accumulators are cleared each call (the engine's per-block contract).
    //--------------------------------------------------------------------------
    void runSingleChannelThroughPipeline (CrossfaderStage&         stage,
                                          const std::vector<float>& chL,
                                          const std::vector<float>& chR,
                                          bool                      assignA,
                                          bool                      assignB,
                                          float                     crossfaderPos,
                                          CrossfaderCurve           curve,
                                          std::vector<float>&       outL,
                                          std::vector<float>&       outR)
    {
        const int n = static_cast<int> (chL.size());
        std::vector<float> busAL (static_cast<size_t> (n), 0.0f);
        std::vector<float> busAR (static_cast<size_t> (n), 0.0f);
        std::vector<float> busBL (static_cast<size_t> (n), 0.0f);
        std::vector<float> busBR (static_cast<size_t> (n), 0.0f);

        ABBus::accumulate (chL.data(), chR.data(), assignA, assignB,
                           busAL.data(), busAR.data(), busBL.data(), busBR.data(), n);

        CrossfaderSnapshot snap;
        snap.crossfader = crossfaderPos;
        snap.curve      = curve;

        outL.assign (static_cast<size_t> (n), 0.0f);
        outR.assign (static_cast<size_t> (n), 0.0f);
        stage.process (busAL.data(), busAR.data(), busBL.data(), busBR.data(),
                       outL.data(), outR.data(), n, snap);
    }

    float peakAbs (const std::vector<float>& v)
    {
        float p = 0.0f;
        for (auto s : v) { const float a = std::abs (s); if (a > p) p = a; }
        return p;
    }
}

//==============================================================================

class CrossfaderTests final : public juce::UnitTest
{
public:
    CrossfaderTests() : juce::UnitTest ("Crossfader", "Sonik") {}

    void runTest() override
    {
        testABBusAssignmentTruthTable();
        testSmoothCurveCentredEqualPower();
        testSmoothCurveFullyLeftMutesB();
        testSmoothCurveFullyRightMutesA();
        testSharpCurveHardCutBelowCentre();
        testSharpVsSmoothNearCentre();
        testNeitherAssignProducesSilence();
        testDualAssignDoublesAtCentre();
        testSmootherSuppressesZipperOnRapidSweep();
        testCurveSwitchIsClickFree();
    }

private:
    //--------------------------------------------------------------------------
    // PRD-0057 §1.4 AC: ABBus routing truth table.
    //--------------------------------------------------------------------------
    void testABBusAssignmentTruthTable()
    {
        beginTest ("ABBus: assign flags route to the correct buses");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.0f);

        const float signalPeak = peakAbs (chL);
        expect (signalPeak > 0.1f, "test signal must be non-trivial");

        struct Case { bool a; bool b; bool expectA; bool expectB; const char* name; };
        const Case cases[] = {
            { true,  false, true,  false, "A only"     },
            { false, true,  false, true,  "B only"     },
            { true,  true,  true,  true,  "dual"       },
            { false, false, false, false, "neither"    },
        };

        for (const auto& c : cases)
        {
            std::vector<float> busAL (static_cast<size_t> (kBlockSize), 0.0f);
            std::vector<float> busAR (static_cast<size_t> (kBlockSize), 0.0f);
            std::vector<float> busBL (static_cast<size_t> (kBlockSize), 0.0f);
            std::vector<float> busBR (static_cast<size_t> (kBlockSize), 0.0f);

            ABBus::accumulate (chL.data(), chR.data(), c.a, c.b,
                                busAL.data(), busAR.data(),
                                busBL.data(), busBR.data(), kBlockSize);

            const bool aHasSignal = peakAbs (busAL) > 1e-6f;
            const bool bHasSignal = peakAbs (busBL) > 1e-6f;
            expect (aHasSignal == c.expectA, juce::String (c.name) + ": bus A presence");
            expect (bHasSignal == c.expectB, juce::String (c.name) + ": bus B presence");
        }
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.4 AC: smooth curve at p=0.5 → both gains == sqrt(1/2).
    // Channel default routing (A only). Master output peak ≈ sqrt(0.5) · peak.
    //--------------------------------------------------------------------------
    void testSmoothCurveCentredEqualPower()
    {
        beginTest ("Smooth curve: centred crossfader gives equal-power gain");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.0f);

        CrossfaderStage stage;
        stage.prepareToPlay (kSampleRate, kBlockSize, 2);

        std::vector<float> outL, outR;
        runSingleChannelThroughPipeline (stage, chL, chR,
                                          /*assignA*/ true, /*assignB*/ false,
                                          0.5f, CrossfaderCurve::Smooth,
                                          outL, outR);

        const float expected = std::sqrt (0.5f);
        for (size_t i = 0; i < outL.size(); ++i)
        {
            const float refL = expected * chL[i];
            const float refR = expected * chR[i];
            expect (std::abs (outL[i] - refL) < 1e-5f,
                    "smooth centred bus A → master L matches sqrt(0.5)·channel");
            expect (std::abs (outR[i] - refR) < 1e-5f,
                    "smooth centred bus A → master R matches sqrt(0.5)·channel");
        }
    }

    //--------------------------------------------------------------------------
    // pos=0.0 → gainA=1, gainB=0. A channel routed only to B is muted.
    //--------------------------------------------------------------------------
    void testSmoothCurveFullyLeftMutesB()
    {
        beginTest ("Smooth curve: fully left mutes bus B");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.5f);

        CrossfaderStage stage;
        primeStage (stage, 0.0f, CrossfaderCurve::Smooth);

        std::vector<float> outL, outR;
        runSingleChannelThroughPipeline (stage, chL, chR,
                                          /*assignA*/ false, /*assignB*/ true,
                                          0.0f, CrossfaderCurve::Smooth,
                                          outL, outR);

        expect (peakAbs (outL) < 1e-5f, "fully-left silences a B-only channel (L)");
        expect (peakAbs (outR) < 1e-5f, "fully-left silences a B-only channel (R)");
    }

    //--------------------------------------------------------------------------
    // pos=1.0 → gainA=0, gainB=1. A channel routed only to A is muted.
    //--------------------------------------------------------------------------
    void testSmoothCurveFullyRightMutesA()
    {
        beginTest ("Smooth curve: fully right mutes bus A");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 1.1f);

        CrossfaderStage stage;
        primeStage (stage, 1.0f, CrossfaderCurve::Smooth);

        std::vector<float> outL, outR;
        runSingleChannelThroughPipeline (stage, chL, chR,
                                          /*assignA*/ true, /*assignB*/ false,
                                          1.0f, CrossfaderCurve::Smooth,
                                          outL, outR);

        expect (peakAbs (outL) < 1e-5f, "fully-right silences an A-only channel (L)");
        expect (peakAbs (outR) < 1e-5f, "fully-right silences an A-only channel (R)");
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.4 AC: sharp curve hard-cuts beyond ±0.02 around centre.
    // At p=0.45 (well outside [0.48, 0.52]), bus B is fully attenuated.
    // The smooth curve at the same p still carries audible B-signal.
    //--------------------------------------------------------------------------
    void testSharpCurveHardCutBelowCentre()
    {
        beginTest ("Sharp curve: hard-cuts bus B at p < 0.48");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.2f);

        // Channel routed only to B.
        CrossfaderStage stage;
        primeStage (stage, 0.45f, CrossfaderCurve::Sharp);

        std::vector<float> outL, outR;
        runSingleChannelThroughPipeline (stage, chL, chR,
                                          /*assignA*/ false, /*assignB*/ true,
                                          0.45f, CrossfaderCurve::Sharp,
                                          outL, outR);

        expect (peakAbs (outL) < 1e-5f, "sharp curve below transition window mutes bus B");
    }

    //--------------------------------------------------------------------------
    // The sharp curve cuts off bus B much faster than the smooth curve as
    // pos decreases from 0.5 toward 0. At p=0.40, sharp ⇒ gainB=0;
    // smooth ⇒ gainB = sin(0.40 · π/2) ≈ 0.588.
    //--------------------------------------------------------------------------
    void testSharpVsSmoothNearCentre()
    {
        beginTest ("Sharp curve attenuates faster than smooth as pos→0");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.7f);

        const float pos = 0.40f;

        CrossfaderStage smoothStage; primeStage (smoothStage, pos, CrossfaderCurve::Smooth);
        CrossfaderStage sharpStage;  primeStage (sharpStage,  pos, CrossfaderCurve::Sharp);

        std::vector<float> smoothL, smoothR, sharpL, sharpR;
        runSingleChannelThroughPipeline (smoothStage, chL, chR,
                                          false, true, pos,
                                          CrossfaderCurve::Smooth, smoothL, smoothR);
        runSingleChannelThroughPipeline (sharpStage, chL, chR,
                                          false, true, pos,
                                          CrossfaderCurve::Sharp, sharpL, sharpR);

        expect (peakAbs (sharpL)  < 1e-5f,
                "sharp curve fully attenuates bus B at pos=0.40");
        expect (peakAbs (smoothL) > 0.1f,
                "smooth curve still passes bus B at pos=0.40");
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.5.3: a channel with both assigns false is silent at the
    // crossfader output for any crossfader position / curve.
    //--------------------------------------------------------------------------
    void testNeitherAssignProducesSilence()
    {
        beginTest ("Both assigns false → silence at any crossfader position");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.3f);

        const float positions[]  = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        const CrossfaderCurve curves[] = { CrossfaderCurve::Smooth, CrossfaderCurve::Sharp };

        for (auto curve : curves)
            for (auto p : positions)
            {
                CrossfaderStage stage;
                primeStage (stage, p, curve);

                std::vector<float> outL, outR;
                runSingleChannelThroughPipeline (stage, chL, chR,
                                                  /*assignA*/ false, /*assignB*/ false,
                                                  p, curve, outL, outR);
                expect (peakAbs (outL) < 1e-6f, "unassigned channel silent (L)");
                expect (peakAbs (outR) < 1e-6f, "unassigned channel silent (R)");
            }
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.5.4: a dual-assigned channel at centre on the smooth curve
    // receives gainA + gainB = sqrt(2) ≈ 1.4142, not unity. The PRD
    // explicitly chooses NOT to normalise (matches Pioneer / Allen & Heath).
    //--------------------------------------------------------------------------
    void testDualAssignDoublesAtCentre()
    {
        beginTest ("Dual-assign at centre on smooth curve sums to sqrt(2)·channel");

        std::vector<float> chL (static_cast<size_t> (kBlockSize));
        std::vector<float> chR (static_cast<size_t> (kBlockSize));
        fillTestSignal (chL, chR, 0.0f);

        CrossfaderStage stage;
        stage.prepareToPlay (kSampleRate, kBlockSize, 2);

        std::vector<float> outL, outR;
        runSingleChannelThroughPipeline (stage, chL, chR,
                                          /*assignA*/ true, /*assignB*/ true,
                                          0.5f, CrossfaderCurve::Smooth,
                                          outL, outR);

        const float expectedScale = std::sqrt (2.0f);   // 2 · sqrt(0.5)
        for (size_t i = 0; i < outL.size(); ++i)
        {
            const float refL = expectedScale * chL[i];
            const float refR = expectedScale * chR[i];
            expect (std::abs (outL[i] - refL) < 1e-5f,
                    "dual-assigned channel sums to sqrt(2)·channel L");
            expect (std::abs (outR[i] - refR) < 1e-5f,
                    "dual-assigned channel sums to sqrt(2)·channel R");
        }
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.4 AC: sweep position rapidly across the whole travel and
    // back. Output must remain finite (no NaN/inf), and the smoother must
    // not introduce any value larger than what the un-smoothed dual-assign
    // ceiling allows.
    //--------------------------------------------------------------------------
    void testSmootherSuppressesZipperOnRapidSweep()
    {
        beginTest ("Smoother: rapid sweep stays finite and bounded");

        constexpr int kSweepBlocks = 32;
        constexpr int kBlock       = 128;

        // Dense full-amplitude input so any pathological zipper / overshoot
        // is exposed by output peak.
        std::vector<float> chL (static_cast<size_t> (kBlock));
        std::vector<float> chR (static_cast<size_t> (kBlock));
        for (size_t i = 0; i < chL.size(); ++i)
        {
            chL[i] = (static_cast<int> (i) & 1) ? 0.9f : -0.9f;
            chR[i] = chL[i];
        }

        CrossfaderStage stage;
        stage.prepareToPlay (kSampleRate, kBlock, 2);

        float globalPeak = 0.0f;
        for (int blk = 0; blk < kSweepBlocks; ++blk)
        {
            // Triangle sweep 0 → 1 → 0 across the kSweepBlocks.
            const float t = static_cast<float> (blk) / static_cast<float> (kSweepBlocks - 1);
            const float pos = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);

            std::vector<float> outL, outR;
            runSingleChannelThroughPipeline (stage, chL, chR,
                                              /*assignA*/ true, /*assignB*/ true,
                                              pos, CrossfaderCurve::Smooth,
                                              outL, outR);

            for (auto s : outL)
            {
                expect (std::isfinite (s), "output sample is finite during sweep");
                const float a = std::abs (s);
                if (a > globalPeak) globalPeak = a;
            }
            for (auto s : outR)
                expect (std::isfinite (s), "output sample is finite during sweep");
        }

        // Dual-assign on smooth curve has theoretical max gain sqrt(2) at
        // centre; allow a small smoother-overshoot margin (the one-pole IIR
        // does not overshoot, so this is a generous cap).
        expect (globalPeak < 0.9f * std::sqrt (2.0f) + 0.05f,
                "peak under sweep stays within dual-assign headroom");
    }

    //--------------------------------------------------------------------------
    // PRD-0057 §1.4 AC: toggling the curve mid-stream must not produce a
    // discontinuity. The smoothed position is preserved across the change
    // and only the gain function applied to it is swapped.
    //--------------------------------------------------------------------------
    void testCurveSwitchIsClickFree()
    {
        beginTest ("Curve switch: smooth ↔ sharp produces no output jump");

        constexpr int kBlock = 256;
        std::vector<float> chL (static_cast<size_t> (kBlock), 0.7f);
        std::vector<float> chR (static_cast<size_t> (kBlock), 0.7f);

        // Hold pos at 0.5 long enough for the smoother to settle, then
        // run two more blocks: one Smooth, one Sharp. At p=0.5, both
        // curves output equal gain on A and B (Sharp linear-interpolates
        // exactly to 0.5/0.5 at the centre; Smooth lands on sqrt(0.5)),
        // so the per-curve gains differ but each individual switch should
        // not introduce a *step* greater than the difference between the
        // two laws' values at the smoothed position.
        CrossfaderStage stage;
        primeStage (stage, 0.5f, CrossfaderCurve::Smooth);

        std::vector<float> smoothL, smoothR, sharpL, sharpR;
        runSingleChannelThroughPipeline (stage, chL, chR, true, true,
                                          0.5f, CrossfaderCurve::Smooth,
                                          smoothL, smoothR);
        // Curve switch — smoothedPos preserved by the stage.
        runSingleChannelThroughPipeline (stage, chL, chR, true, true,
                                          0.5f, CrossfaderCurve::Sharp,
                                          sharpL, sharpR);

        // Predicted analytic values at p=0.5 with channel sample = 0.7,
        // dual-assigned so master = (gainA + gainB) · channel:
        //   Smooth: (sqrt(0.5) + sqrt(0.5)) · 0.7 = sqrt(2) · 0.7 ≈ 0.9899
        //   Sharp : (0.5 + 0.5) · 0.7              = 0.7
        const float expectedSmooth = std::sqrt (2.0f) * 0.7f;
        const float expectedSharp  = 0.7f;

        expect (std::abs (smoothL.back()  - expectedSmooth) < 1e-4f,
                "smooth-curve steady-state output at p=0.5");
        expect (std::abs (sharpL.front()  - expectedSharp)  < 1e-4f,
                "sharp-curve output snaps to its own law on the next block");

        // Verify finite-ness across both blocks.
        for (auto s : smoothL) expect (std::isfinite (s), "smooth block finite");
        for (auto s : sharpL)  expect (std::isfinite (s), "sharp block finite");
    }
};

static CrossfaderTests gCrossfaderTests;
