//==============================================================================
// PRD-0052: MixerStateSchemaTests — automated acceptance tests.
//
// Acceptance criteria from PRD-0052 §1.4 covered here:
//   AC-1  Schema creates "Mixer" sub-tree on construction.
//   AC-2  All four channel sub-trees (A–D) are present after construction.
//   AC-3  Default property values match documented defaults.
//   AC-4  MixerStateBridge mirrors ValueTree changes to MixerAtomicSnapshot.
//   AC-5  dB → linear conversion is correct (0 dB → 1.0, -60 dB → 0.0, +12 dB).
//   AC-6  EQ normalised conversion round-trips are correct.
//   AC-7  Filter bipolar ↔ normalised conversion is correct.
//   AC-8  resetChannel() restores all properties to defaults.
//   AC-9  resetChannel() resets MixerMeterSnapshot slots.
//   AC-10 MixerAtomicSnapshot::crossfader defaults to 0.5.
//   AC-11 Schema is idempotent: constructing twice over the same root is safe.
//   AC-12 ControlTargetRegistry contains all 44 mixer channel targets.
//   AC-13 MixerMeterSnapshot::resetAll() zeroes all slots.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Mixer/State/MixerIdentifiers.h"
#include "Features/Mixer/State/MixerParam.h"
#include "Features/Mixer/State/MixerAtomicSnapshot.h"
#include "Features/Mixer/State/MixerMeterSnapshot.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerStateBridge.h"
#include "Features/Midi/ControlTargetRegistry.h"

#include <cmath>

class MixerStateSchemaTests final : public juce::UnitTest
{
public:
    MixerStateSchemaTests() : juce::UnitTest ("Mixer State Schema", "Sonik") {}

    void runTest() override
    {
        testSchemaCreation();
        testDefaultValues();
        testBridgeMirrorsGain();
        testBridgeMirrorsEq();
        testBridgeMirrorsFilter();
        testBridgeMirrorsFader();
        testBridgeMirrorsKills();
        testBridgeMirrorsAssigns();
        testBridgeMirrorsCrossfader();
        testBridgeMirrorsMasterGain();
        testBridgeCrossfaderDefault();
        testDbToLinear();
        testNormalisedGainRoundTrip();
        testNormalisedEqRoundTrip();
        testFilterBipolarConversion();
        testResetChannel();
        testResetChannelClearsMeterSnapshot();
        testSchemaIdempotency();
        testRegistryContainsMixerChannelTargets();
        testMixerMeterSnapshotResetAll();
        testResolverValueTreeCoverage();
        testResolverMeterCoverage();
        testResolverInvalidPath();
        testResolverWriteTriggersBridge();
        testGainAndFaderClamping_PRD0054();
    }

private:
    //--------------------------------------------------------------------------
    // Helpers
    //--------------------------------------------------------------------------
    struct Ctx
    {
        juce::ValueTree      root { "SonikState" };
        MixerStateSchema     schema;
        MixerAtomicSnapshot  atomics;
        MixerStateBridge     bridge;

        Ctx() : schema (root), bridge (schema, atomics) {}
    };

    static bool approxEqual (float a, float b, float tol = 1e-5f)
    {
        return std::abs (a - b) <= tol;
    }

    //--------------------------------------------------------------------------
    // AC-1, AC-2: Tree structure
    //--------------------------------------------------------------------------
    void testSchemaCreation()
    {
        beginTest ("AC-1/AC-2: Schema creates Mixer tree with 4 channel sub-trees");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema (root);

        auto mixer = root.getChildWithName (MixerIDs::Mixer);
        expect (mixer.isValid(), "Mixer child must exist");

        auto channelContainer = mixer.getChildWithName (MixerIDs::channel);
        expect (channelContainer.isValid(), "channel container must exist");

        expectEquals (channelContainer.getNumChildren(), 4);
        expect (channelContainer.getChildWithName (MixerIDs::A).isValid(), "channel A");
        expect (channelContainer.getChildWithName (MixerIDs::B).isValid(), "channel B");
        expect (channelContainer.getChildWithName (MixerIDs::C).isValid(), "channel C");
        expect (channelContainer.getChildWithName (MixerIDs::D).isValid(), "channel D");

        auto master = mixer.getChildWithName (MixerIDs::master);
        expect (master.isValid(), "master sub-tree must exist");
    }

    //--------------------------------------------------------------------------
    // AC-3: Default values
    //--------------------------------------------------------------------------
    void testDefaultValues()
    {
        beginTest ("AC-3: Default property values");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema (root);

        // Crossfader on Mixer tree
        expect (approxEqual (
            (float) schema.getMixerTree().getProperty (MixerIDs::crossfader),
            MixerStateSchema::kDefaultCrossfader),
            "crossfader default is 0.5");

        // Master gain
        expect (approxEqual (
            (float) schema.getMasterTree().getProperty (MixerIDs::gain),
            MixerStateSchema::kDefaultGainDb),
            "master gain default is 0.0 dB");

        // Per-channel defaults
        const juce::Identifier channelIds[] = { MixerIDs::A, MixerIDs::B, MixerIDs::C, MixerIDs::D };
        for (int i = 0; i < 4; ++i)
        {
            auto ch = schema.getChannelTree (i);
            auto eq = schema.getChannelEqTree (i);
            expect (eq.isValid(), "eq sub-tree present");
            expect (approxEqual ((float) ch.getProperty (MixerIDs::gain),    0.0f), "gain");
            expect (approxEqual ((float) eq.getProperty (MixerIDs::high),    0.0f), "eq.high");
            expect (approxEqual ((float) eq.getProperty (MixerIDs::mid),     0.0f), "eq.mid");
            expect (approxEqual ((float) eq.getProperty (MixerIDs::low),     0.0f), "eq.low");
            expect (approxEqual ((float) ch.getProperty (MixerIDs::filter),  0.0f), "filter");
            expect (approxEqual ((float) ch.getProperty (MixerIDs::fader),   1.0f), "fader");
            expect (! (bool) eq.getProperty (MixerIDs::killHigh), "killHigh false");
            expect (! (bool) eq.getProperty (MixerIDs::killMid),  "killMid false");
            expect (! (bool) eq.getProperty (MixerIDs::killLow),  "killLow false");

            // A/C → assignA=true, assignB=false; B/D → assignA=false, assignB=true
            bool expectAssignA = (i == 0 || i == 2);
            bool expectAssignB = (i == 1 || i == 3);
            expect ((bool) ch.getProperty (MixerIDs::assignA) == expectAssignA, "assignA");
            expect ((bool) ch.getProperty (MixerIDs::assignB) == expectAssignB, "assignB");
        }
    }

    //--------------------------------------------------------------------------
    // AC-4 (gain): Bridge mirrors gain changes
    //--------------------------------------------------------------------------
    void testBridgeMirrorsGain()
    {
        beginTest ("AC-4: Bridge mirrors gain (dB → linear) for all channels");

        Ctx ctx;

        // Set channel B gain to +6 dB
        const float sixDb = 6.0f;
        ctx.schema.getChannelTree (1).setProperty (MixerIDs::gain, sixDb, nullptr);

        float expected = MixerParam::dbToLinear (sixDb);
        expect (approxEqual (ctx.atomics.getChannel (1).gain.load(), expected, 1e-4f),
                    "channel B gain atomic should mirror +6 dB → linear");

        // Verify 0 dB → 1.0
        ctx.schema.getChannelTree (0).setProperty (MixerIDs::gain, 0.0f, nullptr);
        expect (approxEqual (ctx.atomics.getChannel (0).gain.load(), 1.0f),
                    "0 dB → 1.0");

        // Verify ≤-60 dB → 0.0
        ctx.schema.getChannelTree (0).setProperty (MixerIDs::gain, -60.0f, nullptr);
        expect (approxEqual (ctx.atomics.getChannel (0).gain.load(), 0.0f),
                    "-60 dB → 0.0");
    }

    //--------------------------------------------------------------------------
    // AC-4 (EQ): Bridge mirrors EQ band changes
    //--------------------------------------------------------------------------
    void testBridgeMirrorsEq()
    {
        beginTest ("AC-4: Bridge mirrors EQ bands (dB → linear)");

        Ctx ctx;

        ctx.schema.getChannelEqTree (2).setProperty (MixerIDs::high, 3.0f, nullptr);
        ctx.schema.getChannelEqTree (2).setProperty (MixerIDs::mid,  0.0f, nullptr);
        ctx.schema.getChannelEqTree (2).setProperty (MixerIDs::low, -60.0f, nullptr);

        auto& ch = ctx.atomics.getChannel (2);
        expect (approxEqual (ch.eqHigh.load(), MixerParam::dbToLinear (3.0f), 1e-4f), "eqHigh");
        expect (approxEqual (ch.eqMid.load(),  1.0f),                                  "eqMid 0 dB");
        expect (approxEqual (ch.eqLow.load(),  0.0f),                                  "eqLow -60 dB");
    }

    //--------------------------------------------------------------------------
    // AC-4 (filter)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsFilter()
    {
        beginTest ("AC-4: Bridge mirrors filter bipolar value");

        Ctx ctx;

        ctx.schema.getChannelTree (3).setProperty (MixerIDs::filter, 0.75f, nullptr);
        expect (approxEqual (ctx.atomics.getChannel (3).filter.load(), 0.75f),
                    "filter stored as bipolar");
    }

    //--------------------------------------------------------------------------
    // AC-4 (fader)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsFader()
    {
        beginTest ("AC-4: Bridge mirrors channel fader");

        Ctx ctx;

        ctx.schema.getChannelTree (0).setProperty (MixerIDs::fader, 0.5f, nullptr);
        expect (approxEqual (ctx.atomics.getChannel (0).fader.load(), 0.5f),
                    "fader 0.5");
    }

    //--------------------------------------------------------------------------
    // AC-4 (kills)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsKills()
    {
        beginTest ("AC-4: Bridge mirrors kill switches");

        Ctx ctx;

        ctx.schema.getChannelEqTree (1).setProperty (MixerIDs::killHigh, true, nullptr);
        ctx.schema.getChannelEqTree (1).setProperty (MixerIDs::killMid,  true, nullptr);
        ctx.schema.getChannelEqTree (1).setProperty (MixerIDs::killLow,  false, nullptr);

        auto& ch = ctx.atomics.getChannel (1);
        expect (ch.killHigh.load(), "killHigh true");
        expect (ch.killMid.load(),  "killMid true");
        expect (! ch.killLow.load(), "killLow false");
    }

    //--------------------------------------------------------------------------
    // AC-4 (assigns)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsAssigns()
    {
        beginTest ("AC-4: Bridge mirrors crossfader assigns");

        Ctx ctx;

        ctx.schema.getChannelTree (0).setProperty (MixerIDs::assignA, false, nullptr);
        ctx.schema.getChannelTree (0).setProperty (MixerIDs::assignB, true,  nullptr);

        auto& ch = ctx.atomics.getChannel (0);
        expect (! ch.assignA.load(), "assignA false after write");
        expect (  ch.assignB.load(), "assignB true after write");
    }

    //--------------------------------------------------------------------------
    // AC-4 (crossfader)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsCrossfader()
    {
        beginTest ("AC-4: Bridge mirrors crossfader");

        Ctx ctx;

        ctx.schema.getMixerTree().setProperty (MixerIDs::crossfader, 0.25f, nullptr);
        expect (approxEqual (ctx.atomics.crossfader.load(), 0.25f),
                    "crossfader 0.25");
    }

    //--------------------------------------------------------------------------
    // AC-4 (master gain)
    //--------------------------------------------------------------------------
    void testBridgeMirrorsMasterGain()
    {
        beginTest ("AC-4: Bridge mirrors master gain (dB → linear)");

        Ctx ctx;

        ctx.schema.getMasterTree().setProperty (MixerIDs::gain, 6.0f, nullptr);
        expect (approxEqual (ctx.atomics.masterGain.load(),
                                  MixerParam::dbToLinear (6.0f), 1e-4f),
                    "master gain +6 dB → linear");
    }

    //--------------------------------------------------------------------------
    // AC-5: dB → linear
    //--------------------------------------------------------------------------
    void testDbToLinear()
    {
        beginTest ("AC-5: dB to linear conversion");

        expect (approxEqual (MixerParam::dbToLinear (0.0f),  1.0f),        "0 dB = 1.0");
        expect (approxEqual (MixerParam::dbToLinear (-60.0f), 0.0f),        "-60 dB = 0.0");
        expect (approxEqual (MixerParam::dbToLinear (-61.0f), 0.0f),        "<-60 dB = 0.0");
        expect (approxEqual (MixerParam::dbToLinear (12.0f),
                                  std::pow (10.0f, 12.0f / 20.0f), 1e-4f),
                    "+12 dB correct");
        expect (approxEqual (MixerParam::dbToLinear (6.0f),
                                  std::pow (10.0f, 6.0f / 20.0f), 1e-4f),
                    "+6 dB correct");
    }

    //--------------------------------------------------------------------------
    // AC-6: Gain normalised round-trip
    //--------------------------------------------------------------------------
    void testNormalisedGainRoundTrip()
    {
        beginTest ("AC-6: Normalised gain round-trip");

        auto roundTrip = [](float db)
        {
            return MixerParam::normalisedToGainDb (MixerParam::gainDbToNormalised (db));
        };

        expect (approxEqual (roundTrip (0.0f),   0.0f,  0.01f), "0 dB round-trip");
        expect (approxEqual (roundTrip (-60.0f), -60.0f, 0.01f), "-60 dB round-trip");
        expect (approxEqual (roundTrip (12.0f),  12.0f,  0.01f), "+12 dB round-trip");
        expect (approxEqual (roundTrip (6.0f),   6.0f,   0.01f), "+6 dB round-trip");

        // 0 dB must map exactly to 0.5 normalised
        expect (approxEqual (MixerParam::gainDbToNormalised (0.0f), 0.5f),
                    "0 dB → 0.5 normalised");
    }

    //--------------------------------------------------------------------------
    // AC-6 / EQ: EQ normalised round-trip
    //--------------------------------------------------------------------------
    void testNormalisedEqRoundTrip()
    {
        beginTest ("AC-6: Normalised EQ round-trip");

        auto roundTrip = [](float db)
        {
            return MixerParam::normalisedToEqDb (MixerParam::eqDbToNormalised (db));
        };

        expect (approxEqual (roundTrip (0.0f),  0.0f,  0.01f), "0 dB round-trip");
        expect (approxEqual (roundTrip (-60.0f),-60.0f, 0.01f), "-60 dB round-trip");
        expect (approxEqual (roundTrip (6.0f),  6.0f,  0.01f),  "+6 dB round-trip");

        // 0 dB must map exactly to 0.5 normalised
        expect (approxEqual (MixerParam::eqDbToNormalised (0.0f), 0.5f),
                    "EQ 0 dB → 0.5 normalised");
    }

    //--------------------------------------------------------------------------
    // AC-7: Filter conversion
    //--------------------------------------------------------------------------
    void testFilterBipolarConversion()
    {
        beginTest ("AC-7: Filter bipolar ↔ normalised");

        // 0.5 norm → 0.0 bipolar (bypass)
        expect (approxEqual (MixerParam::normalisedToFilterBipolar (0.5f), 0.0f),
                    "0.5 norm → bypass");
        // 0.0 norm → -1.0 bipolar (full LPF)
        expect (approxEqual (MixerParam::normalisedToFilterBipolar (0.0f), -1.0f),
                    "0.0 norm → -1.0 bipolar");
        // 1.0 norm → +1.0 bipolar
        expect (approxEqual (MixerParam::normalisedToFilterBipolar (1.0f), 1.0f),
                    "1.0 norm → +1.0 bipolar");

        // Round-trip
        auto rt = [](float b)
        {
            return MixerParam::normalisedToFilterBipolar (MixerParam::filterBipolarToNormalised (b));
        };
        expect (approxEqual (rt (0.0f),  0.0f),  "0.0 round-trip");
        expect (approxEqual (rt (0.75f), 0.75f), "0.75 round-trip");
        expect (approxEqual (rt (-0.5f),-0.5f),  "-0.5 round-trip");
    }

    //--------------------------------------------------------------------------
    // AC-8: resetChannel()
    //--------------------------------------------------------------------------
    void testResetChannel()
    {
        beginTest ("AC-8: resetChannel() restores defaults");

        Ctx ctx;

        // Modify all properties on channel C (index 2)
        auto ch = ctx.schema.getChannelTree (2);
        auto eq = ctx.schema.getChannelEqTree (2);
        ch.setProperty (MixerIDs::gain,     12.0f, nullptr);
        eq.setProperty (MixerIDs::high,      6.0f, nullptr);
        eq.setProperty (MixerIDs::mid,     -10.0f, nullptr);
        eq.setProperty (MixerIDs::low,     -60.0f, nullptr);
        eq.setProperty (MixerIDs::killHigh, true,  nullptr);
        eq.setProperty (MixerIDs::killMid,  true,  nullptr);
        eq.setProperty (MixerIDs::killLow,  true,  nullptr);
        ch.setProperty (MixerIDs::filter,   0.9f,  nullptr);
        ch.setProperty (MixerIDs::fader,    0.0f,  nullptr);
        ch.setProperty (MixerIDs::assignA,  false, nullptr);
        ch.setProperty (MixerIDs::assignB,  false, nullptr);

        ctx.schema.resetChannel (2);

        auto resetCh = ctx.schema.getChannelTree (2);
        auto resetEq = ctx.schema.getChannelEqTree (2);
        expect (approxEqual ((float) resetCh.getProperty (MixerIDs::gain),   0.0f), "gain reset");
        expect (approxEqual ((float) resetEq.getProperty (MixerIDs::high),   0.0f), "eq.high reset");
        expect (approxEqual ((float) resetCh.getProperty (MixerIDs::filter), 0.0f), "filter reset");
        expect (approxEqual ((float) resetCh.getProperty (MixerIDs::fader),  1.0f), "fader reset");
        expect (! (bool) resetEq.getProperty (MixerIDs::killHigh), "killHigh reset");
        expect (! (bool) resetEq.getProperty (MixerIDs::killLow),  "killLow reset");

        // C defaults: assignA=true, assignB=false
        expect ((bool) resetCh.getProperty (MixerIDs::assignA) == true,  "assignA reset to default");
        expect ((bool) resetCh.getProperty (MixerIDs::assignB) == false, "assignB reset to default");

        // Bridge should have updated the atomics
        auto& atomicCh = ctx.atomics.getChannel (2);
        expect (approxEqual (atomicCh.gain.load(),  1.0f),  "atomic gain reset to 1.0");
        expect (approxEqual (atomicCh.fader.load(), 1.0f),  "atomic fader reset to 1.0");
        expect (! atomicCh.killHigh.load(), "atomic killHigh reset");
    }

    //--------------------------------------------------------------------------
    // AC-9: resetChannel() clears meter snapshot
    //--------------------------------------------------------------------------
    void testResetChannelClearsMeterSnapshot()
    {
        beginTest ("AC-9: resetChannel() clears MixerMeterSnapshot");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        // Write some fake meter values
        meters.getChannel (0).levelPeakL.store (0.9f, std::memory_order_relaxed);
        meters.getChannel (0).clip.store       (true,  std::memory_order_relaxed);

        schema.resetChannel (0, &meters);

        expect (approxEqual (meters.getChannel (0).levelPeakL.load(), 0.0f),
                    "peak meter cleared");
        expect (! meters.getChannel (0).clip.load(), "clip cleared");
    }

    //--------------------------------------------------------------------------
    // AC-10: crossfader default
    //--------------------------------------------------------------------------
    void testBridgeCrossfaderDefault()
    {
        beginTest ("AC-10: crossfader atomic defaults to 0.5");

        Ctx ctx;
        expect (approxEqual (ctx.atomics.crossfader.load(), 0.5f),
                    "crossfader default 0.5");
    }

    //--------------------------------------------------------------------------
    // AC-11: Schema idempotency
    //--------------------------------------------------------------------------
    void testSchemaIdempotency()
    {
        beginTest ("AC-11: Schema is idempotent over same root state tree");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema1 (root);

        // Modify a property via schema1
        schema1.getChannelTree (0).setProperty (MixerIDs::gain, 6.0f, nullptr);

        // Construct a second schema over the same root
        MixerStateSchema schema2 (root);

        // Property should survive (not overwritten by defaults)
        expect (approxEqual (
            (float) schema2.getChannelTree (0).getProperty (MixerIDs::gain), 6.0f),
            "existing property not overwritten by second schema construction");

        // Root should still have exactly one Mixer child
        int mixerCount = 0;
        for (int i = 0; i < root.getNumChildren(); ++i)
            if (root.getChild (i).getType() == MixerIDs::Mixer)
                ++mixerCount;
        expectEquals (mixerCount, 1, "only one Mixer child");
    }

    //--------------------------------------------------------------------------
    // AC-12: ControlTargetRegistry contains mixer channel targets
    //--------------------------------------------------------------------------
    void testRegistryContainsMixerChannelTargets()
    {
        beginTest ("AC-12: ControlTargetRegistry contains all 44 mixer channel targets");

        using namespace sonik::midi;
        using namespace sonik::midi::detail;

        // Expected target IDs — 11 per channel × 4 channels = 44
        const char* expected[] = {
            "mixer.channel.A.gain",      "mixer.channel.A.eq.high",
            "mixer.channel.A.eq.mid",    "mixer.channel.A.eq.low",
            "mixer.channel.A.eq.killHigh", "mixer.channel.A.eq.killMid",
            "mixer.channel.A.eq.killLow",
            "mixer.channel.A.filter",    "mixer.channel.A.fader",
            "mixer.channel.A.assignA",   "mixer.channel.A.assignB",

            "mixer.channel.B.gain",      "mixer.channel.B.eq.high",
            "mixer.channel.B.eq.mid",    "mixer.channel.B.eq.low",
            "mixer.channel.B.eq.killHigh", "mixer.channel.B.eq.killMid",
            "mixer.channel.B.eq.killLow",
            "mixer.channel.B.filter",    "mixer.channel.B.fader",
            "mixer.channel.B.assignA",   "mixer.channel.B.assignB",

            "mixer.channel.C.gain",      "mixer.channel.C.eq.high",
            "mixer.channel.C.eq.mid",    "mixer.channel.C.eq.low",
            "mixer.channel.C.eq.killHigh", "mixer.channel.C.eq.killMid",
            "mixer.channel.C.eq.killLow",
            "mixer.channel.C.filter",    "mixer.channel.C.fader",
            "mixer.channel.C.assignA",   "mixer.channel.C.assignB",

            "mixer.channel.D.gain",      "mixer.channel.D.eq.high",
            "mixer.channel.D.eq.mid",    "mixer.channel.D.eq.low",
            "mixer.channel.D.eq.killHigh", "mixer.channel.D.eq.killMid",
            "mixer.channel.D.eq.killLow",
            "mixer.channel.D.filter",    "mixer.channel.D.fader",
            "mixer.channel.D.assignA",   "mixer.channel.D.assignB",
        };

        int found = 0;
        for (const char* expectedId : expected)
        {
            bool exists = false;
            for (std::size_t i = 0; i < kRegistrySize; ++i)
            {
                if (std::string_view (kRegistry[i].id) == std::string_view (expectedId))
                {
                    exists = true;
                    ++found;
                    break;
                }
            }
            expect (exists, juce::String ("Registry missing: ") + expectedId);
        }
        expectEquals (found, 44, "All 44 channel targets present");
    }

    //--------------------------------------------------------------------------
    // AC-13: MixerMeterSnapshot::resetAll()
    //--------------------------------------------------------------------------
    void testMixerMeterSnapshotResetAll()
    {
        beginTest ("AC-13: MixerMeterSnapshot::resetAll()");

        MixerMeterSnapshot snap;

        for (int i = 0; i < 4; ++i)
        {
            snap.getChannel (i).levelPeakL.store (0.9f, std::memory_order_relaxed);
            snap.getChannel (i).clip.store       (true, std::memory_order_relaxed);
        }
        snap.master.levelPeakL.store (0.8f, std::memory_order_relaxed);
        snap.master.clip.store       (true, std::memory_order_relaxed);

        snap.resetAll();

        for (int i = 0; i < 4; ++i)
        {
            expect (approxEqual (snap.getChannel (i).levelPeakL.load(), 0.0f));
            expect (! snap.getChannel (i).clip.load());
        }
        expect (approxEqual (snap.master.levelPeakL.load(), 0.0f));
        expect (! snap.master.clip.load());
    }

    //--------------------------------------------------------------------------
    // Resolver: every documented ValueTree dotted-path resolves to a valid ref.
    //--------------------------------------------------------------------------
    void testResolverValueTreeCoverage()
    {
        beginTest ("Resolver: ValueTree path coverage");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema (root);
        auto mixer = schema.getMixerTree();

        // Mixer-level + master
        auto xf = MixerIDs::resolveValueTreeProperty (mixer, "mixer.crossfader");
        expect (xf.isValid() && xf.property == MixerIDs::crossfader, "mixer.crossfader resolves");

        auto mg = MixerIDs::resolveValueTreeProperty (mixer, "mixer.master.gain");
        expect (mg.isValid() && mg.property == MixerIDs::gain,
                "mixer.master.gain resolves");

        const char letters[] = { 'A', 'B', 'C', 'D' };
        const char* topLevel[] = { "gain", "filter", "fader", "assignA", "assignB" };
        const juce::Identifier topLevelIds[] = { MixerIDs::gain, MixerIDs::filter,
                                                   MixerIDs::fader, MixerIDs::assignA,
                                                   MixerIDs::assignB };
        const char* eqBands[]  = { "high", "mid", "low", "killHigh", "killMid", "killLow" };
        const juce::Identifier eqIds[] = { MixerIDs::high, MixerIDs::mid, MixerIDs::low,
                                             MixerIDs::killHigh, MixerIDs::killMid,
                                             MixerIDs::killLow };

        for (int i = 0; i < 4; ++i)
        {
            for (int p = 0; p < 5; ++p)
            {
                juce::String path = juce::String ("mixer.channel.") + letters[i] + "." + topLevel[p];
                auto ref = MixerIDs::resolveValueTreeProperty (mixer, path);
                expect (ref.isValid() && ref.property == topLevelIds[p],
                        path + " resolves");
            }
            for (int b = 0; b < 6; ++b)
            {
                juce::String path = juce::String ("mixer.channel.") + letters[i] + ".eq." + eqBands[b];
                auto ref = MixerIDs::resolveValueTreeProperty (mixer, path);
                expect (ref.isValid() && ref.property == eqIds[b],
                        path + " resolves");
            }
        }
    }

    void testResolverMeterCoverage()
    {
        beginTest ("Resolver: meter slot coverage");

        MixerMeterSnapshot meters;

        const char letters[] = { 'A', 'B', 'C', 'D' };
        const char* floatSlots[] = {
            "levelPeakL", "levelPeakR", "levelPeakHoldL", "levelPeakHoldR",
            "levelRmsL", "levelRmsR"
        };

        for (int i = 0; i < 4; ++i)
        {
            for (auto* slot : floatSlots)
            {
                juce::String path = juce::String ("mixer.channel.") + letters[i] + "." + slot;
                auto ref = MixerIDs::resolveMeterSlot (meters, path);
                expect (ref.isValid() && ref.floatSlot != nullptr,
                        path + " resolves to float");
            }
            juce::String clipPath = juce::String ("mixer.channel.") + letters[i] + ".clip";
            auto clipRef = MixerIDs::resolveMeterSlot (meters, clipPath);
            expect (clipRef.isValid() && clipRef.boolSlot != nullptr,
                    clipPath + " resolves to bool");
        }

        for (auto* slot : floatSlots)
        {
            juce::String path = juce::String ("mixer.master.") + slot;
            auto ref = MixerIDs::resolveMeterSlot (meters, path);
            expect (ref.isValid() && ref.floatSlot != nullptr,
                    path + " resolves to float");
        }
        auto mClip = MixerIDs::resolveMeterSlot (meters, "mixer.master.clip");
        expect (mClip.isValid() && mClip.boolSlot != nullptr, "master.clip resolves");
    }

    void testResolverInvalidPath()
    {
        beginTest ("Resolver: invalid paths return invalid refs");

        juce::ValueTree root ("SonikState");
        MixerStateSchema schema (root);
        auto mixer = schema.getMixerTree();
        MixerMeterSnapshot meters;

        const char* badVtPaths[] = {
            "",
            "mixer",
            "mixer.bogus",
            "mixer.channel",
            "mixer.channel.Z.gain",
            "mixer.channel.A",
            "mixer.channel.A.bogus",
            "mixer.channel.A.eq",
            "mixer.channel.A.eq.bogus",
            "deck.0.play",
            "mixer.channel.A.levelPeakL"   // meter slot, not VT
        };
        for (auto* p : badVtPaths)
            expect (! MixerIDs::resolveValueTreeProperty (mixer, p).isValid(),
                    juce::String ("vt invalid: ") + p);

        const char* badMeterPaths[] = {
            "",
            "mixer.channel.A.gain",     // VT prop
            "mixer.crossfader",         // VT prop
            "mixer.channel.Z.levelPeakL",
            "mixer.master.bogus"
        };
        for (auto* p : badMeterPaths)
            expect (! MixerIDs::resolveMeterSlot (meters, p).isValid(),
                    juce::String ("meter invalid: ") + p);
    }

    void testResolverWriteTriggersBridge()
    {
        beginTest ("Resolver: writing through resolved ref updates atomics");

        Ctx ctx;
        auto mixer = ctx.schema.getMixerTree();

        auto ref = MixerIDs::resolveValueTreeProperty (mixer, "mixer.channel.B.eq.high");
        expect (ref.isValid(), "ref valid");

        ref.tree.setProperty (ref.property, 6.0f, nullptr);
        expect (approxEqual (ctx.atomics.getChannel (1).eqHigh.load(),
                              MixerParam::dbToLinear (6.0f), 1e-4f),
                "atomic eqHigh updated via resolver write");

        auto fxRef = MixerIDs::resolveValueTreeProperty (mixer, "mixer.crossfader");
        fxRef.tree.setProperty (fxRef.property, 0.25f, nullptr);
        expect (approxEqual (ctx.atomics.crossfader.load(), 0.25f),
                "atomic crossfader updated via resolver write");
    }

    //--------------------------------------------------------------------------
    // PRD-0054 AC: dB clamped to [-60, +12] and fader clamped to [0, 1] in
    // MixerStateBridge before reaching the audio-thread atomics.
    //--------------------------------------------------------------------------
    void testGainAndFaderClamping_PRD0054()
    {
        beginTest ("PRD-0054: channel gain dB above +12 clamps to +12 in the bridge");
        {
            Ctx ctx;
            // Write +18 dB to the tree (out of spec). The bridge must clamp
            // to +12 dB before dB→linear, so the atomic holds pow(10, 12/20).
            ctx.schema.getChannelTree (0).setProperty (MixerIDs::gain, 18.0f, nullptr);
            const float expected = MixerParam::dbToLinear (12.0f);
            expect (approxEqual (ctx.atomics.getChannel (0).gain.load(), expected, 1e-4f),
                    "channel gain dB +18 → linear clamped to +12 dB equivalent");
        }

        beginTest ("PRD-0054: channel gain dB below -60 is floored to 0 (–inf) in the bridge");
        {
            Ctx ctx;
            ctx.schema.getChannelTree (1).setProperty (MixerIDs::gain, -120.0f, nullptr);
            expect (approxEqual (ctx.atomics.getChannel (1).gain.load(), 0.0f, 1e-6f),
                    "dB -120 → linear 0.0 (clamp + floor)");
        }

        beginTest ("PRD-0054: master gain dB above +12 clamps to +12 in the bridge");
        {
            Ctx ctx;
            ctx.schema.getMasterTree().setProperty (MixerIDs::gain, 24.0f, nullptr);
            const float expected = MixerParam::dbToLinear (12.0f);
            expect (approxEqual (ctx.atomics.masterGain.load(), expected, 1e-4f),
                    "master gain dB +24 → linear clamped to +12 dB equivalent");
        }

        beginTest ("PRD-0054: channel fader above 1.0 clamps to 1.0 in the bridge");
        {
            Ctx ctx;
            ctx.schema.getChannelTree (2).setProperty (MixerIDs::fader, 1.7f, nullptr);
            expect (approxEqual (ctx.atomics.getChannel (2).fader.load(), 1.0f, 1e-6f),
                    "fader value 1.7 clamps to 1.0");
        }

        beginTest ("PRD-0054: channel fader below 0.0 clamps to 0.0 in the bridge");
        {
            Ctx ctx;
            ctx.schema.getChannelTree (3).setProperty (MixerIDs::fader, -0.5f, nullptr);
            expect (approxEqual (ctx.atomics.getChannel (3).fader.load(), 0.0f, 1e-6f),
                    "fader value -0.5 clamps to 0.0");
        }
    }
};

// Self-register with the JUCE unit test runner.
static MixerStateSchemaTests mixerStateSchemaTestsInstance;
