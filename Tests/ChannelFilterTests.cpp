//==============================================================================
// PRD-0056: ChannelFilter unit tests
//
// Covers:
//   - Detent bypass at filter = 0 is bit-exact to input.
//   - Detent bypass via state-setter snap (MixerParam::snapFilterDetent).
//   - Full HPF (filter = +1.0) attenuates a 50 Hz sine by ≥ 20 dB.
//   - Full LPF (filter = -1.0) attenuates an 8 kHz sine by ≥ 20 dB.
//   - Engaged-side cutoff endpoints (filter = +1.0 → ~20 kHz, filter = -1.0
//     → ~20 Hz, filter just outside detent → mirror) match the PRD §1.5.4
//     formula within 1%.
//   - Sweep 0 → +1 → 0 → -1 → 0 in small steps: no NaN/Inf, no per-sample
//     peak overshoot > 1 % of the pre-sweep steady state.
//   - ChannelStripProcessor with filter=0 leaves the SignalFlow direct-sum
//     equivalence intact (within the 1e-6 tolerance).
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Source/Features/Mixer/Dsp/ChannelFilter.h"
#include "../Source/Features/Mixer/Routing/ChannelStripProcessor.h"
#include "../Source/Features/Mixer/Routing/ChannelStripSnapshot.h"
#include "../Source/Features/Mixer/State/MixerParam.h"

#include <cmath>
#include <vector>

class ChannelFilterTests final : public juce::UnitTest
{
public:
    ChannelFilterTests() : juce::UnitTest ("ChannelFilter", "Sonik") {}

    void runTest() override
    {
        testDetentSnapAtSetter();
        testBypassIsBitExact();
        testHpfAttenuatesLowFrequency();
        testLpfAttenuatesHighFrequency();
        testCutoffEndpointsMatchExponentialMap();
        testSweepProducesNoNanOrOvershoot();
        testStripWithFilterZeroPreservesDirectSum();
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

    static double rms (const std::vector<float>& buf, int from, int to)
    {
        double s = 0.0;
        for (int i = from; i < to; ++i)
            s += static_cast<double> (buf[(size_t) i]) * buf[(size_t) i];
        return std::sqrt (s / std::max (1, to - from));
    }

    // Drive the filter at a constant bipolar parameter for `total` samples
    // and return the rendered output (left channel only).
    static std::vector<float> runFilterConstant (float bipolar,
                                                 const std::vector<float>& input)
    {
        ChannelFilter f;
        f.prepare (kSampleRate);
        f.setTarget (bipolar);

        std::vector<float> out (input.size(), 0.0f);
        for (size_t i = 0; i < input.size(); ++i)
        {
            float l = input[i], r = input[i];
            f.processSample (l, r);
            out[i] = l;
        }
        return out;
    }

    //--------------------------------------------------------------------------
    // Test: detent snap at the state-setter boundary (PRD §1.5.6).
    //--------------------------------------------------------------------------
    void testDetentSnapAtSetter()
    {
        beginTest ("snapFilterDetent: |x|<epsilon snaps to 0, otherwise pass-through");
        expectEquals (MixerParam::snapFilterDetent ( 0.000f),  0.0f);
        expectEquals (MixerParam::snapFilterDetent ( 0.019f),  0.0f);
        expectEquals (MixerParam::snapFilterDetent (-0.019f),  0.0f);
        expectEquals (MixerParam::snapFilterDetent ( 0.020f),  0.020f);
        expectEquals (MixerParam::snapFilterDetent (-0.020f), -0.020f);
        expectEquals (MixerParam::snapFilterDetent ( 1.000f),  1.0f);
        expectEquals (MixerParam::snapFilterDetent (-1.000f), -1.0f);
        // Defensive clamp at ±1.
        expectEquals (MixerParam::snapFilterDetent ( 2.0f),  1.0f);
        expectEquals (MixerParam::snapFilterDetent (-2.0f), -1.0f);
    }

    //--------------------------------------------------------------------------
    // Test: filter at exactly 0 is a bit-exact bypass (no SVF math).
    //--------------------------------------------------------------------------
    void testBypassIsBitExact()
    {
        beginTest ("Bypass at filter=0 is bit-exact pass-through");

        const int n = 2048;
        std::vector<float> input ((size_t) n);
        for (int i = 0; i < n; ++i)
            input[(size_t) i] = std::sin (0.13f * (float) i) * 0.7f
                              + std::sin (0.31f * (float) i) * 0.2f;

        auto out = runFilterConstant (0.0f, input);

        for (int i = 0; i < n; ++i)
        {
            const float d = std::abs (out[(size_t) i] - input[(size_t) i]);
            if (d > 0.0f)
            {
                expect (false, "bypass mismatch at sample " + juce::String (i)
                                + " delta=" + juce::String (d));
                return;
            }
        }
        expect (true);
    }

    //--------------------------------------------------------------------------
    // Test: full HPF (filter = +1.0) attenuates 50 Hz by ≥ 20 dB.
    //--------------------------------------------------------------------------
    void testHpfAttenuatesLowFrequency()
    {
        beginTest ("Full HPF removes low-frequency content (50 Hz → ≥ 20 dB cut)");

        const int settle  = 8192;
        const int analyse = 16384;
        const int total   = settle + analyse;

        std::vector<float> input ((size_t) total);
        generateSine (input, 50.0f, 0.5f);

        auto out = runFilterConstant (+1.0f, input);

        const double inRms  = rms (input, settle, total);
        const double outRms = rms (out,   settle, total);
        const double atten  = 20.0 * std::log10 ((outRms + 1e-12) / (inRms + 1e-12));

        expect (atten <= -20.0,
                "HPF @ 50 Hz attenuation = " + juce::String (atten, 2) + " dB (need ≤ -20)");
    }

    //--------------------------------------------------------------------------
    // Test: full LPF (filter = -1.0) attenuates 8 kHz by ≥ 20 dB.
    //--------------------------------------------------------------------------
    void testLpfAttenuatesHighFrequency()
    {
        beginTest ("Full LPF removes high-frequency content (8 kHz → ≥ 20 dB cut)");

        const int settle  = 8192;
        const int analyse = 16384;
        const int total   = settle + analyse;

        std::vector<float> input ((size_t) total);
        generateSine (input, 8000.0f, 0.5f);

        auto out = runFilterConstant (-1.0f, input);

        const double inRms  = rms (input, settle, total);
        const double outRms = rms (out,   settle, total);
        const double atten  = 20.0 * std::log10 ((outRms + 1e-12) / (inRms + 1e-12));

        expect (atten <= -20.0,
                "LPF @ 8 kHz attenuation = " + juce::String (atten, 2) + " dB (need ≤ -20)");
    }

    //--------------------------------------------------------------------------
    // Test: cutoff endpoints reach the PRD §1.5.4 exponential targets within
    // 1 %. We let the per-sample 10 ms smoother converge and read the
    // internal cutoff via the test-only accessor.
    //--------------------------------------------------------------------------
    void testCutoffEndpointsMatchExponentialMap()
    {
        beginTest ("Cutoff endpoints match exponential map within ±1 %");

        auto runAndSettle = [] (float bipolar)
        {
            ChannelFilter f;
            f.prepare (kSampleRate);
            f.setTarget (bipolar);
            // 200 ms is ≫ 10 ms smoother τ → fully settled.
            const int n = static_cast<int> (kSampleRate * 0.20);
            float l = 0.0f, r = 0.0f;
            for (int i = 0; i < n; ++i)
                f.processSample (l, r);
            return f.currentCutoffHz();
        };

        // filter = +1.0 → HPF cutoff ≈ 20 kHz.
        const float hpfTop = runAndSettle (+1.0f);
        expect (std::abs (hpfTop - 20000.0f) <= 200.0f,
                "HPF @ +1.0 cutoff = " + juce::String (hpfTop) + " Hz (need ~20000)");

        // filter = -1.0 → LPF cutoff ≈ 20 Hz.
        const float lpfBot = runAndSettle (-1.0f);
        expect (std::abs (lpfBot - 20.0f) <= 0.2f,
                "LPF @ -1.0 cutoff = " + juce::String (lpfBot) + " Hz (need ~20)");

        // filter just past detent on the HPF side → ~20 Hz.
        const float hpfBot = runAndSettle (ChannelFilter::kDetentEpsilon + 1e-4f);
        expect (std::abs (hpfBot - 20.0f) / 20.0f <= 0.01f,
                "HPF @ +epsilon cutoff = " + juce::String (hpfBot) + " Hz (need ~20)");

        // filter just past detent on the LPF side → ~20 kHz.
        const float lpfTop = runAndSettle (-(ChannelFilter::kDetentEpsilon + 1e-4f));
        expect (std::abs (lpfTop - 20000.0f) / 20000.0f <= 0.01f,
                "LPF @ -epsilon cutoff = " + juce::String (lpfTop) + " Hz (need ~20000)");
    }

    //--------------------------------------------------------------------------
    // Test: aggressive sweep through the detent and across both sides must
    // not produce NaN/Inf and must not introduce a per-sample peak overshoot
    // > 1 % above the pre-sweep input peak amplitude.
    //--------------------------------------------------------------------------
    void testSweepProducesNoNanOrOvershoot()
    {
        beginTest ("Sweep 0 → +1 → 0 → -1 → 0 produces no NaN/Inf and no >1% overshoot");

        ChannelFilter f;
        f.prepare (kSampleRate);

        // Drive with a moderate-amplitude broadband-ish signal (sum of three
        // sines) so any filter ringing or coefficient-update click would
        // manifest as an instantaneous overshoot above the input peak.
        const int n = static_cast<int> (kSampleRate * 2.0); // 2 seconds
        std::vector<float> input ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float t = (float) i / (float) kSampleRate;
            input[(size_t) i] = 0.25f * (std::sin (juce::MathConstants<float>::twoPi *  220.0f * t)
                                       + std::sin (juce::MathConstants<float>::twoPi * 1000.0f * t)
                                       + std::sin (juce::MathConstants<float>::twoPi * 3500.0f * t));
        }

        // The PRD's "pre-sweep steady state" reference is the input itself
        // (bypass passes input through). Compute its peak.
        float inputPeak = 0.0f;
        for (int i = 0; i < n; ++i)
            inputPeak = std::max (inputPeak, std::abs (input[(size_t) i]));
        const float overshootCeiling = inputPeak * 1.01f;

        // Sweep schedule: 0 → +1 → 0 → -1 → 0 over the full 2 s buffer,
        // updating the target every 64 samples (a worst-case DJ-rate update).
        const int stepSamples = 64;
        const int totalSteps  = n / stepSamples;
        float outPeak = 0.0f;
        int step = 0;
        for (int i = 0; i < n; ++i)
        {
            if (i % stepSamples == 0)
            {
                const float u = (float) step / (float) std::max (1, totalSteps - 1);
                // Triangle wave 0 → 1 → 0 → -1 → 0, four segments.
                float target;
                if      (u < 0.25f) target =        (u / 0.25f);              // 0 → 1
                else if (u < 0.50f) target =  1.0f - ((u - 0.25f) / 0.25f);   // 1 → 0
                else if (u < 0.75f) target =       -((u - 0.50f) / 0.25f);    // 0 → -1
                else                target = -1.0f + ((u - 0.75f) / 0.25f);   // -1 → 0
                f.setTarget (target);
                ++step;
            }

            float l = input[(size_t) i], r = input[(size_t) i];
            f.processSample (l, r);

            if (! std::isfinite (l) || ! std::isfinite (r))
            {
                expect (false, "non-finite sample at i=" + juce::String (i));
                return;
            }
            outPeak = std::max (outPeak, std::abs (l));
            outPeak = std::max (outPeak, std::abs (r));
        }

        expect (outPeak <= overshootCeiling,
                "Sweep peak " + juce::String (outPeak)
                + " exceeded 101% of input peak " + juce::String (inputPeak));
    }

    //--------------------------------------------------------------------------
    // Test: ChannelStripProcessor with default snapshot (filter=0) still
    // produces bit-exact (1e-6) pass-through, so PRD-0053's SignalFlow
    // direct-sum equivalence is preserved by the PRD-0056 wiring.
    //--------------------------------------------------------------------------
    void testStripWithFilterZeroPreservesDirectSum()
    {
        beginTest ("ChannelStripProcessor with filter=0 preserves direct-sum equivalence");

        const int n = 512;
        std::vector<float> inL ((size_t) n), inR ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            inL[(size_t) i] = std::sin (0.1f * (float) i) * 0.5f;
            inR[(size_t) i] = std::sin (0.1f * (float) i + 0.3f) * 0.5f;
        }
        std::vector<float> outL ((size_t) n, 0.0f), outR ((size_t) n, 0.0f);

        ChannelStripProcessor strip;
        strip.prepareToPlay (kSampleRate, n, 2);

        ChannelStripSnapshot snap;  // defaults: gain=1, eq=1, filter=0, fader=1
        strip.process (inL.data(), inR.data(), outL.data(), outR.data(), n, snap);

        for (int i = 0; i < n; ++i)
        {
            const float dL = std::abs (outL[(size_t) i] - inL[(size_t) i]);
            const float dR = std::abs (outR[(size_t) i] - inR[(size_t) i]);
            if (dL > 1e-6f || dR > 1e-6f)
            {
                expect (false, "mismatch at sample " + juce::String (i)
                                + "  dL=" + juce::String (dL)
                                + "  dR=" + juce::String (dR));
                return;
            }
        }
        expect (true);
    }
};

static ChannelFilterTests gChannelFilterTests;
