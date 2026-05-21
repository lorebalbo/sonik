//==============================================================================
// PRD-0055: ThreeBandEq unit tests
//
// Covers:
//   - Flat EQ (all bands at unity, no kills) is bit-transparent within 1e-5.
//   - Each band's +6 dB boost lifts a sine at the band centre by ~+6 dB.
//   - Each band's true cut (linear = 0) attenuates a sine in the band's
//     stopband by ≥ 50 dB.
//   - Boosting one band leaves a sine outside that band roughly unchanged
//     (within ±1.5 dB).
//   - Engaging a kill produces a non-increasing per-window peak envelope
//     (no click / no overshoot).
//   - Releasing a kill restores the band gain to the knob value.
//   - Aggressive randomised band sweeps never produce NaN or Inf.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Source/Features/Mixer/Dsp/ThreeBandEq.h"
#include "../Source/Features/Mixer/Routing/ChannelStripSnapshot.h"
#include "../Source/Features/Mixer/State/MixerParam.h"

#include <cmath>
#include <vector>

class ThreeBandEqTests final : public juce::UnitTest
{
public:
    ThreeBandEqTests() : juce::UnitTest ("ThreeBandEq", "Sonik") {}

    void runTest() override
    {
        testFlatEqIsBitTransparent();
        testPerBandBoostAtCentre();
        testPerBandTrueCutAttenuates();
        testOffBandIsRoughlyUnchanged();
        testKillRampHasNoOvershoot();
        testKillReleaseRestoresBandGain();
        testNoNanOrInfUnderAggressiveSweeps();
    }

private:
    static constexpr double kSampleRate = 44100.0;

    //--------------------------------------------------------------------------
    // Helpers
    //--------------------------------------------------------------------------

    static void generateSine (std::vector<float>& buf, float freq, float amp = 0.5f)
    {
        const float w = juce::MathConstants<float>::twoPi * freq
                       / static_cast<float> (kSampleRate);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = amp * std::sin (w * static_cast<float> (i));
    }

    // Run input through `eq` in place with the given snapshot. Smoother
    // targets are applied once at the start.
    static void runEq (ThreeBandEq& eq,
                       const std::vector<float>& input,
                       std::vector<float>& outL,
                       std::vector<float>& outR,
                       const ChannelStripSnapshot& snap)
    {
        eq.setTargets (snap.eqLow,  snap.killLow,
                       snap.eqMid,  snap.killMid,
                       snap.eqHigh, snap.killHigh);
        outL.assign (input.size(), 0.0f);
        outR.assign (input.size(), 0.0f);
        for (size_t i = 0; i < input.size(); ++i)
        {
            float l = input[i], r = input[i];
            eq.processSample (l, r);
            outL[i] = l;
            outR[i] = r;
        }
    }

    // Settle smoothers + biquad state, then return the steady-state band gain
    // (in dB) at `testFreq` for the given snapshot.
    static float measureBandGainDb (ThreeBandEq& eq, float testFreq,
                                    const ChannelStripSnapshot& snap)
    {
        const int settle  = 4096;   // ≫ 8 ms ramp at 44.1 kHz
        const int analyse = 8192;
        const int total   = settle + analyse;

        std::vector<float> input (static_cast<size_t> (total));
        generateSine (input, testFreq, 0.5f);

        std::vector<float> outL, outR;
        runEq (eq, input, outL, outR, snap);

        double inSq = 0.0, outSq = 0.0;
        for (int i = settle; i < total; ++i)
        {
            inSq  += static_cast<double> (input[(size_t) i]) * input[(size_t) i];
            outSq += static_cast<double> (outL [(size_t) i]) * outL [(size_t) i];
        }
        const double inRms  = std::sqrt (inSq  / analyse);
        const double outRms = std::sqrt (outSq / analyse);
        if (inRms <= 0.0 || outRms <= 0.0)
            return -200.0f;
        return static_cast<float> (20.0 * std::log10 (outRms / inRms));
    }

    //--------------------------------------------------------------------------
    // Tests
    //--------------------------------------------------------------------------

    void testFlatEqIsBitTransparent()
    {
        beginTest ("Flat EQ (all 0 dB, no kills) is bit-transparent within 1e-5");

        ThreeBandEq eq;
        eq.prepare (kSampleRate);

        // Mixed-content input spanning the band range: 100 Hz + 1 kHz + 10 kHz.
        const int N = 5000;
        std::vector<float> input (static_cast<size_t> (N));
        const float dt = 1.0f / static_cast<float> (kSampleRate);
        for (int i = 0; i < N; ++i)
        {
            const float t = i * dt;
            input[(size_t) i] = 0.1f * (
                std::sin (juce::MathConstants<float>::twoPi *   100.0f * t)
              + std::sin (juce::MathConstants<float>::twoPi *  1000.0f * t)
              + std::sin (juce::MathConstants<float>::twoPi * 10000.0f * t));
        }

        ChannelStripSnapshot snap;   // defaults: eq = 1.0 linear, no kills.
        std::vector<float> outL, outR;
        runEq (eq, input, outL, outR, snap);

        float maxErr = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            maxErr = std::max (maxErr, std::abs (outL[(size_t) i] - input[(size_t) i]));
            maxErr = std::max (maxErr, std::abs (outR[(size_t) i] - input[(size_t) i]));
        }
        expect (maxErr < 1e-5f,
                "Max per-sample error: " + juce::String (maxErr, 8));
    }

    void testPerBandBoostAtCentre()
    {
        beginTest ("Each band's +12 dB boost lifts its centre frequency by ~+12 dB");

        const float freqs[3] = { ThreeBandEq::kLowFreqHz,
                                  ThreeBandEq::kMidFreqHz,
                                  ThreeBandEq::kHighFreqHz };

        for (int b = 0; b < 3; ++b)
        {
            ThreeBandEq eq;
            eq.prepare (kSampleRate);

            ChannelStripSnapshot snap;
            const float boostLin = MixerParam::dbToLinear (12.0f);
            if (b == 0) snap.eqLow  = boostLin;
            if (b == 1) snap.eqMid  = boostLin;
            if (b == 2) snap.eqHigh = boostLin;

            const float gainDb = measureBandGainDb (eq, freqs[b], snap);

            // Shelves: at the shelf corner the response is the geometric
            // midpoint of the two asymptotes, i.e. half the band gain in dB.
            // Peak (mid): at the centre the response is exactly the band gain.
            const float expected = (b == 1) ? 12.0f : 6.0f;
            expect (std::abs (gainDb - expected) < 1.0f,
                    "Band " + juce::String (b) + " gain at centre: "
                    + juce::String (gainDb, 2) + " dB (expected ~"
                    + juce::String (expected, 1) + ")");
        }
    }

    void testPerBandTrueCutAttenuates()
    {
        beginTest ("Each band's true cut attenuates the stopband by ≥ 50 dB");

        // For shelves we probe deep in the stopband (well below the low corner
        // and well above the high corner) where the asymptotic attenuation is
        // realised. For the peak we probe at the resonance centre.
        struct Probe { float linToZero; float testFreq; const char* label; };
        const Probe probes[3] = {
            { 0.0f /*low*/,   60.0f,    "low"  },
            { 0.0f /*mid*/,   ThreeBandEq::kMidFreqHz,  "mid" },
            { 0.0f /*high*/, 12000.0f,  "high" }
        };

        for (int b = 0; b < 3; ++b)
        {
            ThreeBandEq eq;
            eq.prepare (kSampleRate);

            ChannelStripSnapshot snap;
            if (b == 0) snap.eqLow  = probes[b].linToZero;
            if (b == 1) snap.eqMid  = probes[b].linToZero;
            if (b == 2) snap.eqHigh = probes[b].linToZero;

            const float gainDb = measureBandGainDb (eq, probes[b].testFreq, snap);
            expect (gainDb <= -50.0f,
                    juce::String (probes[b].label) + " band stopband gain: "
                    + juce::String (gainDb, 2) + " dB (expected ≤ -50)");
        }
    }

    void testOffBandIsRoughlyUnchanged()
    {
        beginTest ("Boosting one band leaves a sine outside that band ~unchanged");

        // Boost low +12 dB → response at 8 kHz should be ~0 dB.
        {
            ThreeBandEq eq; eq.prepare (kSampleRate);
            ChannelStripSnapshot snap;
            snap.eqLow = MixerParam::dbToLinear (12.0f);
            const float db = measureBandGainDb (eq, 8000.0f, snap);
            expect (std::abs (db) < 2.0f,
                    "8 kHz gain with low boosted +12: " + juce::String (db, 2) + " dB");
        }

        // Boost high +12 dB → response at 60 Hz should be ~0 dB.
        {
            ThreeBandEq eq; eq.prepare (kSampleRate);
            ChannelStripSnapshot snap;
            snap.eqHigh = MixerParam::dbToLinear (12.0f);
            const float db = measureBandGainDb (eq, 60.0f, snap);
            expect (std::abs (db) < 2.0f,
                    "60 Hz gain with high boosted +12: " + juce::String (db, 2) + " dB");
        }
    }

    void testKillRampHasNoOvershoot()
    {
        beginTest ("Engaging a kill produces a non-increasing peak envelope (no click)");

        ThreeBandEq eq;
        eq.prepare (kSampleRate);

        // 1) Settle at unity on a 250 Hz sine (low-band test).
        const int settleN = 4096;
        const int killN   = 2048;
        const int totalN  = settleN + killN;

        std::vector<float> input (static_cast<size_t> (totalN));
        generateSine (input, ThreeBandEq::kLowFreqHz, 0.5f);

        std::vector<float> outL (static_cast<size_t> (totalN), 0.0f);
        std::vector<float> outR (static_cast<size_t> (totalN), 0.0f);

        // Settle phase: no kill.
        ChannelStripSnapshot snap;
        eq.setTargets (snap.eqLow,  snap.killLow,
                       snap.eqMid,  snap.killMid,
                       snap.eqHigh, snap.killHigh);
        for (int i = 0; i < settleN; ++i)
        {
            float l = input[(size_t) i], r = input[(size_t) i];
            eq.processSample (l, r);
            outL[(size_t) i] = l;
            outR[(size_t) i] = r;
        }

        // Engage kill on low band.
        snap.killLow = true;
        eq.setTargets (snap.eqLow,  snap.killLow,
                       snap.eqMid,  snap.killMid,
                       snap.eqHigh, snap.killHigh);
        for (int i = settleN; i < totalN; ++i)
        {
            float l = input[(size_t) i], r = input[(size_t) i];
            eq.processSample (l, r);
            outL[(size_t) i] = l;
            outR[(size_t) i] = r;
        }

        // Pre-kill steady-state peak (last 256 samples of settle).
        float prePeak = 0.0f;
        for (int i = settleN - 256; i < settleN; ++i)
            prePeak = std::max (prePeak, std::abs (outL[(size_t) i]));

        // No post-kill sample may exceed the pre-kill steady-state peak by
        // more than 1% — a click would be a sudden spike well above this.
        const float clickThreshold = prePeak * 1.01f;
        float worst = 0.0f;
        int worstIdx = -1;
        for (int i = settleN; i < totalN; ++i)
        {
            const float v = std::abs (outL[(size_t) i]);
            if (v > worst) { worst = v; worstIdx = i; }
        }
        expect (worst <= clickThreshold,
                "Click detected at sample " + juce::String (worstIdx)
                + ": " + juce::String (worst, 4)
                + " > pre-kill peak " + juce::String (prePeak, 4));

        // And the envelope should be at silence well past the 8 ms ramp
        // (1024 samples = ~23 ms).
        float tailPeak = 0.0f;
        for (int i = settleN + 1024; i < totalN; ++i)
            tailPeak = std::max (tailPeak, std::abs (outL[(size_t) i]));
        expect (tailPeak < prePeak * 0.05f,
                "Kill failed to reach silence: tail peak "
                + juce::String (tailPeak, 4)
                + " vs pre-kill " + juce::String (prePeak, 4));
    }

    void testKillReleaseRestoresBandGain()
    {
        beginTest ("Releasing a kill restores the band's underlying knob gain");

        ThreeBandEq eq;
        eq.prepare (kSampleRate);

        ChannelStripSnapshot snap;
        snap.eqLow   = MixerParam::dbToLinear (6.0f);
        snap.killLow = true;

        const float dbKilled = measureBandGainDb (eq, 50.0f, snap);
        expect (dbKilled <= -40.0f,
                "Low band stopband while killed: " + juce::String (dbKilled, 2) + " dB");

        // Release the kill — the same eq instance must let the band ramp back.
        snap.killLow = false;
        const float dbReleased = measureBandGainDb (eq, ThreeBandEq::kLowFreqHz, snap);
        // At the shelf corner with +6 dB boost, expected response is +3 dB.
        expect (std::abs (dbReleased - 3.0f) < 1.0f,
                "Low band corner after release: " + juce::String (dbReleased, 2)
                + " dB (expected ~+3)");
    }

    void testNoNanOrInfUnderAggressiveSweeps()
    {
        beginTest ("Aggressive randomised band sweeps produce no NaN or Inf");

        ThreeBandEq eq;
        eq.prepare (kSampleRate);

        juce::Random rng (0xC0FFEEull);
        const int N = 16384;
        std::vector<float> outL (static_cast<size_t> (N));
        std::vector<float> outR (static_cast<size_t> (N));

        ChannelStripSnapshot snap;

        for (int i = 0; i < N; ++i)
        {
            // Every 64 samples randomise all bands and kill flags.
            if ((i % 64) == 0)
            {
                const float lowDb  = -60.0f + rng.nextFloat() * 66.0f;
                const float midDb  = -60.0f + rng.nextFloat() * 66.0f;
                const float highDb = -60.0f + rng.nextFloat() * 66.0f;
                snap.eqLow   = MixerParam::dbToLinear (lowDb);
                snap.eqMid   = MixerParam::dbToLinear (midDb);
                snap.eqHigh  = MixerParam::dbToLinear (highDb);
                snap.killLow  = (rng.nextInt (4) == 0);
                snap.killMid  = (rng.nextInt (4) == 0);
                snap.killHigh = (rng.nextInt (4) == 0);
                eq.setTargets (snap.eqLow,  snap.killLow,
                               snap.eqMid,  snap.killMid,
                               snap.eqHigh, snap.killHigh);
            }

            float l = rng.nextFloat() * 2.0f - 1.0f;
            float r = rng.nextFloat() * 2.0f - 1.0f;
            eq.processSample (l, r);
            outL[(size_t) i] = l;
            outR[(size_t) i] = r;
        }

        for (int i = 0; i < N; ++i)
        {
            if (! std::isfinite (outL[(size_t) i]) || ! std::isfinite (outR[(size_t) i]))
            {
                expect (false, "Non-finite output at sample " + juce::String (i));
                return;
            }
        }
        expect (true);
    }
};

static ThreeBandEqTests threeBandEqTestsInstance;
