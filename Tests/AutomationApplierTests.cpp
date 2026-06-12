//==============================================================================
// PRD-0092: Automation Playback Applier tests.
//
// Verifies that, during DAW timeline playback, the AutomationApplier evaluates
// every enabled lane at the transport playhead and writes the result through the
// single-source-of-truth path: continuous mixer params → mixer ValueTree
// (linear + step/hold interpolation correct); tempo → injected sink AND the
// MasterClockManager override visible in the published snapshot with no other
// tempo field touched; deck booleans → injected sink write-on-change only;
// bypassed lanes inert; transport-stopped fully inert; and the re-entrancy guard
// stops the applier's own writes from being re-captured (no feedback loop).
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Daw/Automation/AutomationModel.h"
#include "../Source/Features/Daw/Automation/ContinuousLane.h"
#include "../Source/Features/Daw/Automation/BooleanLane.h"
#include "../Source/Features/Daw/Automation/AutomationApplier.h"
#include "../Source/Features/Daw/Automation/AutomationCaptureTaps.h"
#include "../Source/Features/Daw/Automation/ChannelContinuousAutomationCapture.h"
#include "../Source/Features/Daw/Automation/ChannelBooleanAutomationCapture.h"
#include "../Source/Features/Daw/Playback/DawTransport.h"
#include "../Source/Features/Daw/State/DawState.h"
#include "../Source/Features/Mixer/State/MixerStateSchema.h"
#include "../Source/Features/Mixer/State/MixerIdentifiers.h"
#include "../Source/Features/Sync/MasterClockPublisher.h"
#include "../Source/Features/Sync/MasterClockManager.h"
#include "../Source/Features/Deck/DeckIdentifiers.h"

#include <cstdint>
#include <vector>

using namespace Daw;

namespace
{

// A capture record for the boolean sink: who/what/state, in call order.
struct BoolWrite { int channel; juce::String param; bool state; };

// Base harness: mixer + automation model + transport + applier with spy sinks.
struct Harness
{
    juce::ValueTree  root  { "SonikState" };
    MixerStateSchema mixer { root };
    juce::ValueTree  daw   { DawState::createDawBranch() };
    AutomationModel  model { daw, nullptr };
    DawTransport     transport;

    std::vector<double>    tempoWrites;
    std::vector<BoolWrite> boolWrites;

    AutomationApplier applier {
        model, mixer, transport,
        [this] (double bpm)                                   { tempoWrites.push_back (bpm); },
        [this] (int ch, const juce::String& p, bool s)        { boolWrites.push_back ({ ch, p, s }); }
    };

    ContinuousLane continuous (const juce::String& owner, const juce::String& param)
    {
        return model.getOrCreateContinuousLane (owner, param);
    }

    BooleanLane boolean (const juce::String& owner, const juce::String& param)
    {
        return model.getOrCreateBooleanLane (owner, param);
    }
};

} // namespace

class AutomationApplierTests final : public juce::UnitTest
{
public:
    AutomationApplierTests()
        : juce::UnitTest ("Automation Playback Applier (PRD-0092)", "Sonik") {}

    void runTest() override
    {
        interpolationPureFunctionLinearAndStep();
        continuousMixerLaneWritesInterpolatedValue();
        volumeLaneWritesChannelFader();
        continuousMixerStepHoldWrites();
        tempoLaneDrivesSink();
        tempoLaneDrivesMasterClockOverride();
        booleanLaneWritesOnChangeOnly();
        bypassedLaneProducesNoWrite();
        transportStoppedIsInert();
        reentrancyGuardPreventsCaptureFeedback();
    }

private:
    //==========================================================================
    void interpolationPureFunctionLinearAndStep()
    {
        beginTest ("Interpolation is a pure function of (lane, playhead): linear and step");

        Harness h;
        auto linear = h.continuous ("A", "filter");
        linear.addBreakpoint (0,    0.0, Interpolation::Linear);
        linear.addBreakpoint (1000, 1.0, Interpolation::Linear);

        // Midpoint of a linear segment.
        auto mid = linear.evaluateAt (500);
        expect (mid.has_value());
        expectWithinAbsoluteError (*mid, 0.5, 1.0e-9);

        // Quarter point.
        expectWithinAbsoluteError (*linear.evaluateAt (250), 0.25, 1.0e-9);

        auto step = h.continuous ("B", "gain");
        step.addBreakpoint (0,    -6.0, Interpolation::Step);
        step.addBreakpoint (1000,  0.0, Interpolation::Step);

        // Step/hold: holds the left value across the whole segment.
        expectWithinAbsoluteError (*step.evaluateAt (1),   -6.0, 1.0e-9);
        expectWithinAbsoluteError (*step.evaluateAt (999), -6.0, 1.0e-9);
        // At the right breakpoint it takes the new value.
        expectWithinAbsoluteError (*step.evaluateAt (1000), 0.0, 1.0e-9);
    }

    //==========================================================================
    void continuousMixerLaneWritesInterpolatedValue()
    {
        beginTest ("Continuous mixer lane writes the linearly-interpolated value to the right property");

        Harness h;

        // Filter on A (channel top-level, bipolar).
        auto filter = h.continuous ("A", "filter");
        filter.addBreakpoint (0,    -1.0, Interpolation::Linear);
        filter.addBreakpoint (2000,  1.0, Interpolation::Linear);

        // EQ low on C (eq sub-tree, dB).
        auto low = h.continuous ("C", "eq.low");
        low.addBreakpoint (0,     0.0, Interpolation::Linear);
        low.addBreakpoint (2000, -12.0, Interpolation::Linear);

        // gain on B (channel top-level, dB).
        auto gain = h.continuous ("B", "gain");
        gain.addBreakpoint (0,   0.0, Interpolation::Linear);
        gain.addBreakpoint (2000, 6.0, Interpolation::Linear);

        h.transport.play();          // Stopped -> Playing resets playhead to 0
        h.transport.seek (500);      // 1/4 of the way
        h.applier.tick();

        auto chA = h.mixer.getChannelTree (0);
        auto eqC = h.mixer.getChannelEqTree (2);
        auto chB = h.mixer.getChannelTree (1);

        // filter: -1 + 2 * 0.25 = -0.5
        expectWithinAbsoluteError ((double) chA.getProperty (MixerIDs::filter), -0.5, 1.0e-6);
        // eq.low: 0 + (-12) * 0.25 = -3 dB
        expectWithinAbsoluteError ((double) eqC.getProperty (MixerIDs::low), -3.0, 1.0e-6);
        // gain: 0 + 6 * 0.25 = 1.5 dB
        expectWithinAbsoluteError ((double) chB.getProperty (MixerIDs::gain), 1.5, 1.0e-6);

        // No tempo / boolean writes happened for continuous mixer lanes.
        expectEquals ((int) h.tempoWrites.size(), 0);
        expectEquals ((int) h.boolWrites.size(),  0);
    }

    //==========================================================================
    void volumeLaneWritesChannelFader()
    {
        beginTest ("Volume lane writes the channel's authoritative fader property");

        Harness h;

        // A recorded fade-out on channel A: full open -> closed over 2000 samples.
        auto volume = h.continuous ("A", "volume");
        volume.addBreakpoint (0,    1.0, Interpolation::Linear);
        volume.addBreakpoint (2000, 0.0, Interpolation::Linear);

        h.transport.play();
        h.transport.seek (500);      // 1/4 of the way through the fade
        h.applier.tick();

        auto chA = h.mixer.getChannelTree (0);
        expectWithinAbsoluteError ((double) chA.getProperty (MixerIDs::fader), 0.75, 1.0e-6);

        // The fader of an untouched channel stays at its default.
        auto chB = h.mixer.getChannelTree (1);
        expectWithinAbsoluteError ((double) chB.getProperty (MixerIDs::fader,
                                                             MixerStateSchema::kDefaultFader),
                                   (double) MixerStateSchema::kDefaultFader, 1.0e-6);
    }

    //==========================================================================
    void continuousMixerStepHoldWrites()
    {
        beginTest ("Continuous mixer lane with step/hold writes the held left value");

        Harness h;
        auto mid = h.continuous ("D", "eq.mid");
        mid.addBreakpoint (0,    3.0, Interpolation::Step);
        mid.addBreakpoint (1000, 9.0, Interpolation::Step);

        h.transport.play();
        h.transport.seek (600);      // inside the first (step) segment
        h.applier.tick();

        auto eqD = h.mixer.getChannelEqTree (3);
        expectWithinAbsoluteError ((double) eqD.getProperty (MixerIDs::mid), 3.0, 1.0e-6);
    }

    //==========================================================================
    void tempoLaneDrivesSink()
    {
        beginTest ("Master tempo lane drives the tempo sink with the evaluated BPM (write-on-change)");

        Harness h;
        auto tempo = h.continuous ("master", "tempo");
        tempo.addBreakpoint (0,    120.0, Interpolation::Linear);
        tempo.addBreakpoint (1000, 140.0, Interpolation::Linear);

        h.transport.play();
        h.transport.seek (500);      // midpoint -> 130 BPM
        h.applier.tick();

        expectEquals ((int) h.tempoWrites.size(), 1);
        expectWithinAbsoluteError (h.tempoWrites.back(), 130.0, 1.0e-6);

        // Ticking again at the SAME playhead produces no redundant tempo write.
        h.applier.tick();
        expectEquals ((int) h.tempoWrites.size(), 1);

        // Moving the playhead changes the value -> one new write.
        h.transport.seek (750);      // -> 135 BPM
        h.applier.tick();
        expectEquals ((int) h.tempoWrites.size(), 2);
        expectWithinAbsoluteError (h.tempoWrites.back(), 135.0, 1.0e-6);

        // Tempo never wrote any mixer property.
        expectWithinAbsoluteError ((double) h.mixer.getChannelTree (0).getProperty (MixerIDs::filter,
                                   (double) MixerStateSchema::kDefaultFilter),
                                   (double) MixerStateSchema::kDefaultFilter, 1.0e-9);
    }

    //==========================================================================
    void tempoLaneDrivesMasterClockOverride()
    {
        beginTest ("Tempo automation drives MasterClockManager override -> published snapshot BPM, no other tempo field touched");

        Harness h;

        // A playing master deck on slot A with a 100-BPM beatgrid.
        juce::ValueTree decks (IDs::Decks);
        juce::ValueTree deck (IDs::Deck);
        deck.setProperty (IDs::id,             "A",      nullptr);
        deck.setProperty (IDs::playbackStatus, "playing", nullptr);
        deck.setProperty (IDs::isMaster,       false,    nullptr);
        deck.setProperty (IDs::isSynced,       false,    nullptr);
        deck.setProperty (IDs::speedMultiplier, 1.0f,    nullptr);
        juce::ValueTree grid (IDs::BeatGrid);
        grid.setProperty (IDs::bpm,          100.0,       nullptr);
        grid.setProperty (IDs::anchorSample, (int64_t) 0, nullptr);
        deck.addChild (grid, -1, nullptr);
        decks.addChild (deck, -1, nullptr);
        h.root.addChild (decks, -1, nullptr);

        MasterClockPublisher publisher;
        MasterClockManager   clock { h.root, publisher };

        // The applier's tempo sink is bound to the clock override (production wiring).
        AutomationApplier applier {
            h.model, h.mixer, h.transport,
            [&clock] (double bpm) { clock.setAutomationTempoOverride (bpm); },
            [] (int, const juce::String&, bool) {}
        };

        // Baseline: master deck drives the published BPM to its effective value.
        // (setMaster promotes the deck and publishes; without it the listener path
        // hasn't fired because we built the tree before constructing the clock.)
        clock.setMaster (0);
        const auto baseline = publisher.read();
        expectWithinAbsoluteError (baseline.masterBPM, 100.0, 1.0e-6);
        const double baseNativeBpm = baseline.masterNativeBPM;
        const auto   basePhase      = baseline.masterPhaseOriginSample;
        const bool   basePlaying    = baseline.masterIsPlaying;

        // Drive an automated tempo of 128 BPM.
        auto tempo = h.model.getOrCreateContinuousLane ("master", "tempo");
        tempo.addBreakpoint (0, 128.0, Interpolation::Step);

        h.transport.play();
        h.transport.seek (0);
        applier.tick();

        const auto snap = publisher.read();
        expectWithinAbsoluteError (snap.masterBPM, 128.0, 1.0e-6);

        // No OTHER tempo / clock field changed: native BPM, phase origin and the
        // playing flag are untouched by the override.
        expectWithinAbsoluteError (snap.masterNativeBPM, baseNativeBpm, 1.0e-9);
        expect (snap.masterPhaseOriginSample == basePhase, "phase origin unchanged");
        expect (snap.masterIsPlaying == basePlaying,        "playing flag unchanged");

        // Clearing the override reverts the published BPM to the derived value.
        clock.clearAutomationTempoOverride();
        expectWithinAbsoluteError (publisher.read().masterBPM, 100.0, 1.0e-6);
    }

    //==========================================================================
    void booleanLaneWritesOnChangeOnly()
    {
        beginTest ("Boolean lane writes the sink only on change; keyLock state routed by (channel, param)");

        Harness h;
        auto keyLock = h.boolean ("A", "keyLock");
        keyLock.addStep (0,    false);
        keyLock.addStep (1000, true);
        keyLock.addStep (2000, false);

        h.transport.play();

        // Before first toggle: false. First tick establishes false (state change
        // from the applier's "unknown" baseline).
        h.transport.seek (500);
        h.applier.tick();
        expectEquals ((int) h.boolWrites.size(), 1);
        expect (h.boolWrites.back().state == false);
        expectEquals (h.boolWrites.back().channel, 0);
        expect (h.boolWrites.back().param == juce::String ("keyLock"));

        // Tick again still false -> no redundant write.
        h.applier.tick();
        expectEquals ((int) h.boolWrites.size(), 1);

        // Move past the on-step: one write (true).
        h.transport.seek (1500);
        h.applier.tick();
        expectEquals ((int) h.boolWrites.size(), 2);
        expect (h.boolWrites.back().state == true);

        // Holding inside the same step -> no further writes across several ticks.
        h.transport.seek (1600);
        h.applier.tick();
        h.transport.seek (1700);
        h.applier.tick();
        expectEquals ((int) h.boolWrites.size(), 2);

        // Past the off-step: one more write (false).
        h.transport.seek (2500);
        h.applier.tick();
        expectEquals ((int) h.boolWrites.size(), 3);
        expect (h.boolWrites.back().state == false);
    }

    //==========================================================================
    void bypassedLaneProducesNoWrite()
    {
        beginTest ("Bypassed lane is not evaluated/written; re-enabling resumes on the next tick");

        Harness h;
        auto filter = h.continuous ("A", "filter");
        filter.addBreakpoint (0,   0.2, Interpolation::Step);
        filter.addBreakpoint (1000, 0.8, Interpolation::Step);

        // Bypass the lane.
        h.model.setLaneEnabled (filter.getState(), false);

        h.transport.play();
        h.transport.seek (500);
        h.applier.tick();

        auto chA = h.mixer.getChannelTree (0);
        // Untouched: still the schema default.
        expectWithinAbsoluteError ((double) chA.getProperty (MixerIDs::filter),
                                   (double) MixerStateSchema::kDefaultFilter, 1.0e-9);

        // Re-enable -> next tick applies (0.2 held in the first step segment).
        h.model.setLaneEnabled (filter.getState(), true);
        h.applier.tick();
        expectWithinAbsoluteError ((double) chA.getProperty (MixerIDs::filter), 0.2, 1.0e-6);
    }

    //==========================================================================
    void transportStoppedIsInert()
    {
        beginTest ("Transport stopped -> applier is fully inert: no ValueTree mutation, no sink calls");

        Harness h;
        auto filter = h.continuous ("A", "filter");
        filter.addBreakpoint (0,    0.5, Interpolation::Linear);
        filter.addBreakpoint (1000, 0.9, Interpolation::Linear);
        auto tempo = h.continuous ("master", "tempo");
        tempo.addBreakpoint (0, 130.0, Interpolation::Linear);
        auto keyLock = h.boolean ("A", "keyLock");
        keyLock.addStep (0, true);

        // Never call play(): transport remains Stopped.
        h.applier.tick();

        auto chA = h.mixer.getChannelTree (0);
        expectWithinAbsoluteError ((double) chA.getProperty (MixerIDs::filter),
                                   (double) MixerStateSchema::kDefaultFilter, 1.0e-9);
        expectEquals ((int) h.tempoWrites.size(), 0);
        expectEquals ((int) h.boolWrites.size(),  0);
        expect (! h.applier.isApplying());
    }

    //==========================================================================
    void reentrancyGuardPreventsCaptureFeedback()
    {
        beginTest ("Re-entrancy guard: applier writes are not re-captured into the lanes (no feedback loop)");

        Harness h;

        // A continuous capture tap stack observing the SAME mixer trees the
        // applier writes, sharing the applier's guard. While the applier is
        // applying, the tap must record nothing — otherwise its appends would
        // grow the very lane the applier is reading.
        ModelAutomationAppendSink captureSink { h.model, nullptr };

        bool         capArmed    = true;            // armed throughout
        std::int64_t capPlayhead = 0;

        AutomationCaptureTaps taps {
            [&capArmed]    { return capArmed; },
            [&capPlayhead] { return capPlayhead; },
            captureSink
        };
        ChannelContinuousAutomationCapture::registerTaps (taps, h.mixer);
        taps.setApplyingAutomationGuard (h.applier.makeApplyingGuard());

        // Seed all twenty lanes with one initial breakpoint each.
        taps.captureInitialValues (0);

        // The filter lane on A that the applier will drive.
        auto filter = h.model.getOrCreateContinuousLane ("A", "filter");
        filter.addBreakpoint (0,    -0.5, Interpolation::Linear);
        filter.addBreakpoint (2000,  0.5, Interpolation::Linear);

        const int seededFilterBreakpoints = filter.getNumBreakpoints();

        // Capture counts BEFORE the applier runs.
        auto gainLane = h.model.getContinuousLane ("A", "gain");
        const int gainBefore = gainLane.getNumBreakpoints();

        h.transport.play();
        for (int s = 100; s <= 1900; s += 100)
        {
            h.transport.seek (s);
            capPlayhead = s; // capture playhead follows (it would be ignored anyway)
            h.applier.tick();
        }

        // The applier moved the filter property many times, but because the guard
        // suppressed capture, the captured filter lane gained NO breakpoints
        // beyond the two the test author added plus the seed.
        auto filterLane = h.model.getContinuousLane ("A", "filter");
        expectEquals (filterLane.getNumBreakpoints(), seededFilterBreakpoints,
                      "captured filter lane must not grow from applier writes");

        // The gain lane (also tapped) likewise gained nothing.
        expectEquals (h.model.getContinuousLane ("A", "gain").getNumBreakpoints(), gainBefore,
                      "captured gain lane must not grow from applier writes");

        // Sanity: with the guard REMOVED, a direct mixer write WOULD be captured,
        // proving the tap is live and it was the guard doing the suppression.
        taps.setApplyingAutomationGuard (nullptr);
        capPlayhead = 5000;
        h.mixer.getChannelTree (0).setProperty (MixerIDs::filter, 0.123, nullptr);
        expect (h.model.getContinuousLane ("A", "filter").getNumBreakpoints() > seededFilterBreakpoints,
                "tap is live once the guard is cleared");
    }
};

static AutomationApplierTests automationApplierTests;
