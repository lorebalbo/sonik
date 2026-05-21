//==============================================================================
// PRD-0054: ChannelStripProcessor unit tests
//
// Covers:
//   - dB→linear mapping via MixerParam (incl. -60 dB floor and +12 dB ceiling)
//   - Fader-and-gain composition (multiplicative product at settled samples)
//   - Unity output when both gain and fader are at default (1.0)
//   - Smoother ramps: no step discontinuity on full-range instantaneous changes
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Source/Features/Mixer/Routing/ChannelStripProcessor.h"
#include "../Source/Features/Mixer/Routing/ChannelStripSnapshot.h"
#include "../Source/Features/Mixer/State/MixerParam.h"
#include "../Source/Features/Mixer/State/MixerIdentifiers.h"
#include "../Source/Features/Mixer/State/MixerStateSchema.h"
#include "../Source/Features/Mixer/State/MixerStateBridge.h"
#include "../Source/Features/Mixer/State/MixerAtomicSnapshot.h"

class ChannelStripTests final : public juce::UnitTest
{
public:
    ChannelStripTests() : juce::UnitTest ("ChannelStrip", "Sonik") {}

    void runTest() override
    {
        testDbToLinearMapping();
        testUnityPassthrough();
        testGainAndFaderComposition();
        testSmootherNoContinuityBreak();
        testFaderAloneScalesOutput();
        testGainPlus12dbViaSnapshot();
        testSmootherRampLengthIsSevenMs();
        testMixerChannelStatePersistsAcrossTrackLoad();
    }

private:
    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    // Process `numBlocks` of `blockSize` silence through the strip with the
    // given snapshot, then process one final block of `input` and return
    // the first output sample.  The silence blocks let the smoothers settle.
    static float processAndSettle (ChannelStripProcessor& strip,
                                   const ChannelStripSnapshot& snap,
                                   float inputSample,
                                   int blockSize = 512,
                                   int settleBlocks = 3)
    {
        std::vector<float> silenceL (static_cast<size_t>(blockSize), 0.0f);
        std::vector<float> silenceR (static_cast<size_t>(blockSize), 0.0f);
        std::vector<float> outL     (static_cast<size_t>(blockSize), 0.0f);
        std::vector<float> outR     (static_cast<size_t>(blockSize), 0.0f);

        // Run silence through while converging to the target values.
        for (int b = 0; b < settleBlocks; ++b)
            strip.process (silenceL.data(), silenceR.data(),
                           outL.data(), outR.data(),
                           blockSize, snap);

        // Now process a block with the test input sample everywhere.
        std::vector<float> inputL (static_cast<size_t>(blockSize), inputSample);
        std::vector<float> inputR (static_cast<size_t>(blockSize), inputSample);
        strip.process (inputL.data(), inputR.data(),
                       outL.data(), outR.data(),
                       blockSize, snap);

        // Return the last output sample (well past the 309-sample ramp).
        return outL[static_cast<size_t>(blockSize - 1)];
    }

    // -------------------------------------------------------------------------
    // Test 1: dB→linear mapping via MixerParam (PRD-0054 AC dB→linear)
    // -------------------------------------------------------------------------
    void testDbToLinearMapping()
    {
        beginTest ("dB-to-linear: 0 dB = 1.0");
        expectWithinAbsoluteError (MixerParam::dbToLinear (0.0f), 1.0f, 1e-5f);

        beginTest ("dB-to-linear: -60 dB = 0.0 (floor)");
        expectEquals (MixerParam::dbToLinear (-60.0f), 0.0f);

        beginTest ("dB-to-linear: below -60 dB = 0.0 (hard floor)");
        expectEquals (MixerParam::dbToLinear (-100.0f), 0.0f);

        beginTest ("dB-to-linear: +12 dB = pow(10, 0.6)");
        const float expected12 = std::pow (10.0f, 12.0f / 20.0f);
        expectWithinAbsoluteError (MixerParam::dbToLinear (12.0f), expected12, 1e-5f);

        beginTest ("dB-to-linear: -6 dB ≈ 0.501");
        expectWithinAbsoluteError (MixerParam::dbToLinear (-6.0f), std::pow (10.0f, -6.0f / 20.0f), 1e-5f);
    }

    // -------------------------------------------------------------------------
    // Test 2: Unity pass-through (gain=1.0, fader=1.0, PRD-0054 §identity)
    // -------------------------------------------------------------------------
    void testUnityPassthrough()
    {
        beginTest ("Unity passthrough: gain=1.0, fader=1.0 produces no attenuation");

        ChannelStripProcessor strip;
        strip.prepareToPlay (44100.0, 512, 2);

        ChannelStripSnapshot snap; // defaults: gain=1.0, fader=1.0
        float out = processAndSettle (strip, snap, 0.8f);
        expectWithinAbsoluteError (out, 0.8f, 1e-5f);
    }

    // -------------------------------------------------------------------------
    // Test 3: Fader-and-gain composition (PRD-0054 AC multiplicative product)
    // -------------------------------------------------------------------------
    void testGainAndFaderComposition()
    {
        beginTest ("Gain * fader composition: 0.5 * 0.5 = 0.25 at settled samples");

        ChannelStripProcessor strip;
        strip.prepareToPlay (44100.0, 512, 2);

        ChannelStripSnapshot snap;
        snap.gain  = 0.5f;
        snap.fader = 0.5f;

        const float input = 1.0f;
        float out = processAndSettle (strip, snap, input);
        // After smoother settles, output = input * gain * fader = 1.0 * 0.5 * 0.5 = 0.25
        expectWithinAbsoluteError (out, 0.25f, 1e-4f);
    }

    // -------------------------------------------------------------------------
    // Test 4: No step discontinuity on full-range gain change (PRD-0054 AC smoothing)
    // -------------------------------------------------------------------------
    void testSmootherNoContinuityBreak()
    {
        beginTest ("Smoother: no step discontinuity on gain jump from 1.0 to 0.0");

        ChannelStripProcessor strip;
        strip.prepareToPlay (44100.0, 512, 2);

        // First settle at gain=1.0, fader=1.0
        {
            ChannelStripSnapshot snap; // gain=1.0, fader=1.0
            float out0 = processAndSettle (strip, snap, 1.0f);
            expectWithinAbsoluteError (out0, 1.0f, 1e-4f);
        }

        // Now immediately request gain=0.0 and run just ONE block.
        // The smoother should ramp down smoothly: no sample should jump
        // discontinuously (all consecutive samples should differ by less
        // than 2/309 ≈ 0.0065 per sample at 44100 Hz).
        ChannelStripSnapshot snapOff;
        snapOff.gain  = 0.0f;
        snapOff.fader = 1.0f;

        const int blockSize = 512;
        std::vector<float> inputL  (static_cast<size_t>(blockSize), 1.0f);
        std::vector<float> inputR  (static_cast<size_t>(blockSize), 1.0f);
        std::vector<float> outL    (static_cast<size_t>(blockSize), 0.0f);
        std::vector<float> outR    (static_cast<size_t>(blockSize), 0.0f);
        strip.process (inputL.data(), inputR.data(), outL.data(), outR.data(), blockSize, snapOff);

        // Maximum allowable per-sample step: 2x the linear ramp increment
        // (ramp = 1.0 / round(44100 * 0.007) ≈ 1/309 ≈ 0.00324).
        // We use 0.015 as a generous tolerance to account for any floating-point edge.
        const float maxStep = 0.015f;
        for (int i = 1; i < blockSize; ++i)
        {
            float delta = std::abs (outL[static_cast<size_t>(i)] - outL[static_cast<size_t>(i - 1)]);
            if (delta > maxStep)
            {
                // Fail with a descriptive message on the first bad sample.
                expect (false, "Step discontinuity at sample " + juce::String (i)
                               + ": delta=" + juce::String (delta));
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Test 5: Fader alone scales output proportionally
    // -------------------------------------------------------------------------
    void testFaderAloneScalesOutput()
    {
        beginTest ("Fader alone: fader=0.0 produces silence, fader=1.0 passes unity");

        ChannelStripProcessor strip;
        strip.prepareToPlay (44100.0, 512, 2);

        // Fader = 0.0 (muted)
        ChannelStripSnapshot snapMute;
        snapMute.gain  = 1.0f;
        snapMute.fader = 0.0f;
        float outMute = processAndSettle (strip, snapMute, 1.0f);
        expectWithinAbsoluteError (outMute, 0.0f, 1e-4f);

        // Fader = 1.0 (full)
        ChannelStripSnapshot snapFull;
        snapFull.gain  = 1.0f;
        snapFull.fader = 1.0f;
        float outFull = processAndSettle (strip, snapFull, 0.7f);
        expectWithinAbsoluteError (outFull, 0.7f, 1e-4f);
    }

    // -------------------------------------------------------------------------
    // Test 6: +12 dB ceiling via the snapshot path (PRD-0054 AC upper bound)
    // The atomic stores the linear amplitude already (clamped in the bridge);
    // ChannelStripProcessor must faithfully scale by that value.
    // -------------------------------------------------------------------------
    void testGainPlus12dbViaSnapshot()
    {
        beginTest ("Gain stage applies +12 dB linear amplitude faithfully");

        ChannelStripProcessor strip;
        strip.prepareToPlay (44100.0, 512, 2);

        ChannelStripSnapshot snap;
        snap.gain  = MixerParam::dbToLinear (12.0f);  // ~3.981
        snap.fader = 1.0f;

        const float input = 0.1f;
        float out = processAndSettle (strip, snap, input);
        expectWithinAbsoluteError (out, input * snap.gain, 1e-4f);
    }

    // -------------------------------------------------------------------------
    // Test 7: Smoother ramp length is exactly 7 ms at the configured sample
    // rate (PRD-0054 AC: 7 ms time constant).  We verify by counting how many
    // samples it takes for a unit step to settle (within 1%).  Expected:
    // round(sampleRate * 0.007) samples for a linear ramp.
    // -------------------------------------------------------------------------
    void testSmootherRampLengthIsSevenMs()
    {
        beginTest ("Smoother ramp length = round(sampleRate * 0.007) samples");

        constexpr double sr = 48000.0;
        const int expectedRamp = static_cast<int> (std::round (sr * 0.007));

        ChannelStripProcessor strip;
        strip.prepareToPlay (sr, 1024, 2);

        // Settle at unity, then jump to gain=0 and measure samples until output
        // reaches < 1% of the unit step (i.e., gain has ramped from 1.0 → ~0).
        ChannelStripSnapshot snapUnity;
        processAndSettle (strip, snapUnity, 1.0f, 1024, 2);

        ChannelStripSnapshot snapZero;
        snapZero.gain  = 0.0f;
        snapZero.fader = 1.0f;

        const int blockSize = 2048;
        std::vector<float> inputL (static_cast<size_t>(blockSize), 1.0f);
        std::vector<float> inputR (static_cast<size_t>(blockSize), 1.0f);
        std::vector<float> outL   (static_cast<size_t>(blockSize), 0.0f);
        std::vector<float> outR   (static_cast<size_t>(blockSize), 0.0f);
        strip.process (inputL.data(), inputR.data(), outL.data(), outR.data(),
                       blockSize, snapZero);

        int settleSample = -1;
        for (int i = 0; i < blockSize; ++i)
        {
            if (std::abs (outL[static_cast<size_t>(i)]) < 0.01f)
            {
                settleSample = i;
                break;
            }
        }

        expect (settleSample >= 0, "smoother reached < 1% within the block");
        // Allow ±5 samples slack — linear ramp endpoint can land just before or
        // after the 1% threshold depending on rounding.
        expect (std::abs (settleSample - expectedRamp) <= 5,
                juce::String ("smoother settle samples: ") + juce::String (settleSample)
                + " (expected ~" + juce::String (expectedRamp) + ")");
    }

    // -------------------------------------------------------------------------
    // Test 8: PRD-0054 AC — mixer channel gain & fader persist across deck
    // track-load events. The mixer state is owned by MixerStateSchema and is
    // entirely decoupled from per-deck transport / track-load flow. We simulate
    // a "track load" by mutating deck-level ValueTrees and verify the mixer
    // channel atomics remain unchanged.
    // -------------------------------------------------------------------------
    void testMixerChannelStatePersistsAcrossTrackLoad()
    {
        beginTest ("Mixer channel gain/fader persist across simulated track-load events");

        juce::ValueTree    root ("SonikState");
        MixerStateSchema   schema (root);
        MixerAtomicSnapshot atomics;
        MixerStateBridge   bridge (schema, atomics);

        // Set non-default channel A gain (+3 dB) and fader (0.42).
        schema.getChannelTree (0).setProperty (MixerIDs::gain, 3.0f, nullptr);
        schema.getChannelTree (0).setProperty (MixerIDs::fader, 0.42f, nullptr);

        const float gainBefore  = atomics.getChannel (0).gain.load();
        const float faderBefore = atomics.getChannel (0).fader.load();

        // Simulate a deck-side track-load event: create + mutate a Decks
        // sub-tree on the same root.  The mixer schema is a sibling tree and
        // must not be touched by deck-level operations.
        juce::ValueTree decks ("Decks");
        root.addChild (decks, -1, nullptr);
        juce::ValueTree deck ("Deck");
        decks.addChild (deck, -1, nullptr);
        deck.setProperty ("trackId",        "track-1", nullptr);
        deck.setProperty ("playheadPosition", 0,        nullptr);
        // Repeat: simulate eject + reload.
        deck.setProperty ("trackId",        "",        nullptr);
        deck.setProperty ("trackId",        "track-2", nullptr);

        const float gainAfter  = atomics.getChannel (0).gain.load();
        const float faderAfter = atomics.getChannel (0).fader.load();

        expectWithinAbsoluteError (gainAfter,  gainBefore,  1e-6f);
        expectWithinAbsoluteError (faderAfter, faderBefore, 1e-6f);
    }
};

static ChannelStripTests channelStripTestsInstance;
