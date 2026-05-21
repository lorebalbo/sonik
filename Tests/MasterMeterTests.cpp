//==============================================================================
// PRD-0058: MasterMeterTests
//
// Covers:
//   1. MasterStage with unity master gain produces bit-exact identity copy.
//   2. -6 dB master gain attenuates input amplitude to ≈ 0.5 of original.
//   3. +6 dB master gain on a +0.5 input does not exceed +1.0 (the
//      MasterStage alone does NOT pre-clip; the PRD-0002 hard-clip safety
//      net in AudioEngine catches anything that would exceed ±1.0).
//   4. Peak meter latches and holds: a single transient sample at 0.9
//      remains visible for ~1.5 s of subsequent silence (peak-hold).
//   5. RMS meter reports a value within 1 dB of the expected sqrt(mean(x²)).
//   6. Clip flag latches on a sample ≥ 1.0 and stays latched until
//      clearClip() is called (PRD-0058 §1.5.5 manual clear path).
//   7. Per-channel meter follows ChannelStripProcessor output (one active
//      channel with snapshot fader = 1.0, gain = 1.0).
//   8. Master gain smoother: a 24 dB instantaneous jump produces no zipper
//      and no step discontinuity (ramp length 7 ms — PRD-0058 §1.3.1).
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Source/Features/Mixer/Routing/MasterStage.h"
#include "../Source/Features/Mixer/Routing/MasterSnapshot.h"
#include "../Source/Features/Mixer/Routing/ChannelStripProcessor.h"
#include "../Source/Features/Mixer/Routing/ChannelStripSnapshot.h"
#include "../Source/Features/Mixer/State/MixerMeterSnapshot.h"
#include "../Source/Features/Mixer/State/MixerParam.h"

#include <cmath>
#include <vector>

class MasterMeterTests final : public juce::UnitTest
{
public:
    MasterMeterTests() : juce::UnitTest ("MasterMeter", "Sonik") {}

    void runTest() override
    {
        testUnityGainIsBitExact();
        testMinus6dbHalvesAmplitude();
        testPlus6dbDoesNotPreClip();
        testPeakHoldLatchesAndDecays();
        testRmsTracksSineWithin1Db();
        testClipLatchesAndManualClear();
        testPerChannelMeterPostFader();
        testMasterGainSmootherNoZipper();
    }

private:
    static constexpr double kSampleRate = 44100.0;
    static constexpr int    kBlockSize  = 512;

    //--------------------------------------------------------------------------
    // 1. Unity master gain → output is a bit-exact copy of input.
    //--------------------------------------------------------------------------
    void testUnityGainIsBitExact()
    {
        beginTest ("MasterStage: unity master gain produces bit-exact identity copy");

        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);

        std::vector<float> inL (kBlockSize), inR (kBlockSize), outL (kBlockSize), outR (kBlockSize);
        for (int i = 0; i < kBlockSize; ++i)
        {
            inL[(size_t) i] = std::sin (static_cast<float> (i) * 0.07f) * 0.4f;
            inR[(size_t) i] = std::cos (static_cast<float> (i) * 0.09f) * 0.3f;
        }

        MasterSnapshot snap;
        snap.masterGain = 1.0f;
        mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        for (int i = 0; i < kBlockSize; ++i)
        {
            expect (outL[(size_t) i] == inL[(size_t) i], "unity L mismatch");
            expect (outR[(size_t) i] == inR[(size_t) i], "unity R mismatch");
        }
    }

    //--------------------------------------------------------------------------
    // 2. -6 dB → amplitude ≈ 0.5 (after the 7 ms smoother settles).
    //--------------------------------------------------------------------------
    void testMinus6dbHalvesAmplitude()
    {
        beginTest ("MasterStage: -6 dB master gain attenuates to ~0.5 amplitude");

        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);

        // Need a non-trivial transition so seed at unity then drop to -6 dB.
        // Run 5 settle blocks so the 7 ms ramp has long since completed.
        const float gainLinear = MixerParam::dbToLinear (-6.0f);
        const float expected   = gainLinear; // 0.5 * 1.0 input

        std::vector<float> inL (kBlockSize, 1.0f), inR (kBlockSize, 1.0f);
        std::vector<float> outL (kBlockSize),       outR (kBlockSize);

        MasterSnapshot snap;
        snap.masterGain = gainLinear;
        for (int b = 0; b < 5; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        // Inspect the last sample (smoother fully settled).
        const float gotL = outL[(size_t) (kBlockSize - 1)];
        const float gotR = outR[(size_t) (kBlockSize - 1)];
        expectWithinAbsoluteError (gotL, expected, 1e-4f);
        expectWithinAbsoluteError (gotR, expected, 1e-4f);
        // -6.0206 dB → linear 0.5012 (within 1%)
        expectWithinAbsoluteError (gotL, 0.5f, 0.01f);
    }

    //--------------------------------------------------------------------------
    // 3. +6 dB master gain on +0.5 input: MasterStage alone outputs ~1.0,
    //    NOT pre-clipped. (PRD-0002 hard-clip in AudioEngine catches the
    //    output post-MasterStage.)
    //--------------------------------------------------------------------------
    void testPlus6dbDoesNotPreClip()
    {
        beginTest ("MasterStage: +6 dB does not pre-clip (safety net lives in AudioEngine)");

        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);

        const float gainLinear = MixerParam::dbToLinear (+6.0f); // ≈ 1.995
        std::vector<float> inL (kBlockSize, 0.5f), inR (kBlockSize, 0.5f);
        std::vector<float> outL (kBlockSize),       outR (kBlockSize);

        MasterSnapshot snap;
        snap.masterGain = gainLinear;
        for (int b = 0; b < 5; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        // 0.5 * 1.995 ≈ 0.9977 — below the hard-clip threshold; MasterStage
        // does not limit, so the output is exactly the product.
        const float gotL = outL[(size_t) (kBlockSize - 1)];
        expectWithinAbsoluteError (gotL, 0.5f * gainLinear, 1e-4f);
        expect (gotL <= 1.0f,           "0.5 * +6dB stays under 1.0");
        expect (gotL > 0.99f && gotL < 1.0f, "0.5 * +6dB ≈ 0.998");

        // Now confirm a true overshoot is NOT clipped by MasterStage.
        // 0.8 * +6dB ≈ 1.596 → MasterStage outputs 1.596 (clipping is the
        // engine's job).
        for (auto& v : inL) v = 0.8f;
        for (auto& v : inR) v = 0.8f;
        for (int b = 0; b < 5; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);
        expect (outL[(size_t) (kBlockSize - 1)] > 1.5f,
                "MasterStage does not pre-clip; output > 1.0 is expected");
    }

    //--------------------------------------------------------------------------
    // 4. Peak meter latches and decays. A transient at 0.9 followed by
    //    silence — peakHold should still read close to 0.9 well before 1.5 s
    //    has elapsed, and reach (or approach) 0 after 1.5 s.
    //--------------------------------------------------------------------------
    void testPeakHoldLatchesAndDecays()
    {
        beginTest ("MasterStage: peak-hold latches transient and decays over ~1.5 s");

        MixerMeterSnapshot meters;
        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);
        mx.setMeterSlot (&meters.master);

        MasterSnapshot snap;
        snap.masterGain = 1.0f;

        // First block: a single transient at index 0, rest silence.
        std::vector<float> inL (kBlockSize, 0.0f), inR (kBlockSize, 0.0f);
        std::vector<float> outL (kBlockSize),     outR (kBlockSize);
        inL[0] = 0.9f;
        inR[0] = 0.9f;
        mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        const float peakAfter1Block = meters.master.levelPeakHoldL.load (std::memory_order_relaxed);
        expect (peakAfter1Block > 0.85f, "peakHold latches near 0.9 immediately after the transient");

        // Run silence for ~0.5 s and confirm peakHold is still well above 0.
        const int blocksForHalfSec = static_cast<int> (std::ceil (0.5 * kSampleRate / kBlockSize));
        inL[0] = 0.0f; inR[0] = 0.0f;
        for (int b = 0; b < blocksForHalfSec; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        const float peakHoldAfterHalfSec = meters.master.levelPeakHoldL.load (std::memory_order_relaxed);
        // After 0.5 s of a 1.5 s linear decay starting from 0.9, expect ≈ 0.6.
        expect (peakHoldAfterHalfSec > 0.45f && peakHoldAfterHalfSec < 0.75f,
                juce::String ("peakHold mid-decay = ") + juce::String (peakHoldAfterHalfSec));

        // Run silence for the remaining ~1.5 s and confirm peakHold ≈ 0.
        const int blocksForRest = static_cast<int> (std::ceil (1.6 * kSampleRate / kBlockSize));
        for (int b = 0; b < blocksForRest; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        const float peakHoldFinal = meters.master.levelPeakHoldL.load (std::memory_order_relaxed);
        expect (peakHoldFinal < 0.05f,
                juce::String ("peakHold fully decayed = ") + juce::String (peakHoldFinal));
    }

    //--------------------------------------------------------------------------
    // 5. RMS tracks a 1 kHz sine to within 1 dB.
    //--------------------------------------------------------------------------
    void testRmsTracksSineWithin1Db()
    {
        beginTest ("MasterStage: RMS meter tracks sine within 1 dB");

        MixerMeterSnapshot meters;
        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);
        mx.setMeterSlot (&meters.master);

        const float amplitude   = 0.5f;
        const float freqHz      = 1000.0f;
        const float twoPiOverSr = juce::MathConstants<float>::twoPi * freqHz / static_cast<float> (kSampleRate);

        std::vector<float> inL (kBlockSize), inR (kBlockSize);
        std::vector<float> outL (kBlockSize), outR (kBlockSize);

        MasterSnapshot snap;
        snap.masterGain = 1.0f;

        // Process enough blocks to fill the 300 ms RMS window plus headroom.
        const int totalBlocks = static_cast<int> (std::ceil (0.6 * kSampleRate / kBlockSize));
        int phase = 0;
        for (int b = 0; b < totalBlocks; ++b)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float s = amplitude * std::sin (twoPiOverSr * static_cast<float> (phase + i));
                inL[(size_t) i] = s;
                inR[(size_t) i] = s;
            }
            phase += kBlockSize;
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);
        }

        const float rmsL     = meters.master.levelRmsL.load (std::memory_order_relaxed);
        const float expected = amplitude / std::sqrt (2.0f); // 0.3535...
        const float gotDb    = MixerParam::linearToDb (rmsL);
        const float expDb    = MixerParam::linearToDb (expected);
        expect (std::fabs (gotDb - expDb) < 1.0f,
                juce::String ("RMS mismatch — got ") + juce::String (gotDb)
                + " dB, expected " + juce::String (expDb) + " dB");
    }

    //--------------------------------------------------------------------------
    // 6. Clip flag latches and is cleared via clearClip().
    //--------------------------------------------------------------------------
    void testClipLatchesAndManualClear()
    {
        beginTest ("MasterStage: clip flag latches on ≥1.0 sample, clears manually");

        MixerMeterSnapshot meters;
        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);
        mx.setMeterSlot (&meters.master);

        MasterSnapshot snap;
        snap.masterGain = 1.0f;

        std::vector<float> inL (kBlockSize, 0.0f), inR (kBlockSize, 0.0f);
        std::vector<float> outL (kBlockSize),     outR (kBlockSize);
        // Put a single 1.0 sample inside the block; everything else silence.
        inL[16] = 1.0f;
        mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        expect (meters.master.clip.load (std::memory_order_relaxed),
                "clip latches after one ≥1.0 sample");

        // Run silence — clip should remain latched for the next several
        // blocks (well under the 3 s auto-clear window).
        inL[16] = 0.0f;
        for (int b = 0; b < 4; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);
        expect (meters.master.clip.load (std::memory_order_relaxed),
                "clip stays latched well before 3 s auto-clear");

        // Manual clear from the message-thread caller.
        meters.master.clearClip();
        // One more silent block so the audio thread observes the cleared
        // flag and resets its internal counter.
        mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);
        expect (! meters.master.clip.load (std::memory_order_relaxed),
                "clip stays cleared after manual clearClip() + silence");
    }

    //--------------------------------------------------------------------------
    // 7. Per-channel meter follows ChannelStripProcessor output.
    //--------------------------------------------------------------------------
    void testPerChannelMeterPostFader()
    {
        beginTest ("ChannelStripProcessor: per-channel meter follows post-fader output");

        MixerMeterSnapshot meters;
        ChannelStripProcessor strip;
        strip.prepareToPlay (kSampleRate, kBlockSize, 2);
        strip.setMeterSlot (&meters.channels[0]);

        ChannelStripSnapshot chSnap;     // gain=1.0, fader=1.0, eq=flat
        chSnap.gain  = 1.0f;
        chSnap.fader = 1.0f;

        const float amplitude   = 0.4f;
        const float freqHz      = 440.0f;
        const float twoPiOverSr = juce::MathConstants<float>::twoPi * freqHz / static_cast<float> (kSampleRate);

        std::vector<float> inL (kBlockSize), inR (kBlockSize);
        std::vector<float> outL (kBlockSize), outR (kBlockSize);

        const int totalBlocks = static_cast<int> (std::ceil (0.6 * kSampleRate / kBlockSize));
        int phase = 0;
        for (int b = 0; b < totalBlocks; ++b)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float s = amplitude * std::sin (twoPiOverSr * static_cast<float> (phase + i));
                inL[(size_t) i] = s;
                inR[(size_t) i] = s;
            }
            phase += kBlockSize;
            strip.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, chSnap);
        }

        const float rmsL = meters.channels[0].levelRmsL.load (std::memory_order_relaxed);
        const float expected = amplitude / std::sqrt (2.0f);
        const float gotDb = MixerParam::linearToDb (rmsL);
        const float expDb = MixerParam::linearToDb (expected);
        expect (std::fabs (gotDb - expDb) < 1.0f,
                juce::String ("channel RMS mismatch — got ") + juce::String (gotDb)
                + " dB, expected " + juce::String (expDb) + " dB");

        // Master meter slot was never wired → it remains at default zeros,
        // confirming the channel and master meters are independent.
        expect (meters.master.levelRmsL.load (std::memory_order_relaxed) == 0.0f,
                "master meter untouched when only channel meter is wired");
    }

    //--------------------------------------------------------------------------
    // 8. Master gain smoother: 24 dB instantaneous jump produces a smooth
    //    ramp (no per-sample diff exceeds the linear ramp step size).
    //--------------------------------------------------------------------------
    void testMasterGainSmootherNoZipper()
    {
        beginTest ("MasterStage: 24 dB gain jump produces a smooth ramp, no zipper");

        MasterStage mx;
        mx.prepareToPlay (kSampleRate, kBlockSize, 2);

        // Seed at unity by running a few blocks.
        std::vector<float> inL (kBlockSize, 1.0f), inR (kBlockSize, 1.0f);
        std::vector<float> outL (kBlockSize),     outR (kBlockSize);

        MasterSnapshot snap;
        snap.masterGain = 1.0f;
        for (int b = 0; b < 3; ++b)
            mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        // Jump to -24 dB (24 dB drop). Process one block and inspect the
        // per-sample output for any step that exceeds the expected linear
        // ramp slope. 7 ms ramp at 44.1 kHz ≈ 309 samples; the linear delta
        // per sample over the full transition is (1.0 - target) / 309.
        const float target = MixerParam::dbToLinear (-24.0f); // ≈ 0.0631
        const float rampSamples = static_cast<float> (kSampleRate) * 0.007f;
        const float maxStep = (1.0f - target) / rampSamples * 1.5f; // 50 % headroom

        snap.masterGain = target;
        mx.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlockSize, snap);

        float maxObserved = 0.0f;
        for (int i = 1; i < kBlockSize; ++i)
        {
            const float d = std::fabs (outL[(size_t) i] - outL[(size_t) (i - 1)]);
            if (d > maxObserved) maxObserved = d;
        }
        expect (maxObserved < maxStep,
                juce::String ("max step ") + juce::String (maxObserved)
                + " exceeded expected ramp step " + juce::String (maxStep));

        // And the absolute first-sample step from the last block's 1.0 to
        // this block's first sample should not exceed the same ramp step.
        const float firstStep = std::fabs (outL[0] - 1.0f);
        expect (firstStep < maxStep,
                juce::String ("first-sample step ") + juce::String (firstStep)
                + " exceeded expected ramp step " + juce::String (maxStep));
    }
};

static MasterMeterTests masterMeterTests;
