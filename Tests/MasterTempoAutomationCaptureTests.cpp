#include "../Source/Features/Daw/Automation/AutomationModel.h"
#include "../Source/Features/Daw/Automation/ContinuousLane.h"
#include "../Source/Features/Daw/Automation/AutomationCaptureTaps.h"
#include "../Source/Features/Daw/Automation/MasterTempoAutomationCapture.h"
#include "../Source/Features/Daw/State/DawState.h"

#include <juce_data_structures/juce_data_structures.h>

using namespace Daw;

namespace
{

// Builds a real AutomationModel over a fresh daw branch and exposes a production
// append sink so capture results land in inspectable lane nodes.
struct Harness
{
    juce::ValueTree           daw   { DawState::createDawBranch() };
    AutomationModel           model { daw, nullptr };
    ModelAutomationAppendSink sink  { model, nullptr };

    ContinuousLane tempoLane()
    {
        return model.getContinuousLane (AutomationStrings::kOwnerMaster, "tempo");
    }
};

} // namespace

class MasterTempoAutomationCaptureTests final : public juce::UnitTest
{
public:
    MasterTempoAutomationCaptureTests()
        : juce::UnitTest ("Master Tempo Automation Capture (PRD-0089)", "Sonik") {}

    void runTest() override
    {
        seedDefinesLaneFromStart();
        rampThinnedToEndpoints();
        masterHandoverIsStep();
        disarmedDoesNothing();
        unchangedBpmAppendsNothing();
        dormantSeedUsesLastKnownNonZero();
    }

private:
    //==========================================================================
    void seedDefinesLaneFromStart()
    {
        beginTest ("Seed at record start defines the lane from its first sample");

        Harness h;
        double  bpm      = 124.0;
        std::int64_t head = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return true; },
            [&] { return head; },
            h.sink);

        cap.seedAtRecordStart();

        auto lane = h.tempoLane();
        expect (lane.isValid());
        expectEquals (lane.getNumBreakpoints(), 1);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (0)), 124.0, 1.0e-9);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (0)), 0);
    }

    //==========================================================================
    void rampThinnedToEndpoints()
    {
        beginTest ("Linear ramp is thinned to its endpoints and reconstructs faithfully");

        Harness h;
        double       bpm  = 124.0;
        std::int64_t head = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return true; },
            [&] { return head; },
            h.sink);

        cap.seedAtRecordStart(); // (0, 124)

        // Feed a perfectly linear ramp 124 -> 126 over 10 ticks.
        for (int i = 1; i <= 10; ++i)
        {
            head = (std::int64_t) i * 100;
            bpm  = 124.0 + 2.0 * (double) i / 10.0;
            cap.captureTick();
        }

        auto lane = h.tempoLane();
        expect (lane.isValid());

        // Collinear interior samples carry no shape, so the ramp collapses to two
        // retained breakpoints (start + end).
        expectEquals (lane.getNumBreakpoints(), 2);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (0)), 124.0, 1.0e-9);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (1)), 126.0, 1.0e-9);

        // Linear interpolation between the retained endpoints reconstructs the ramp.
        auto mid = lane.evaluateAt (500);
        expect (mid.has_value());
        expectWithinAbsoluteError (*mid, 125.0, 0.05);
    }

    //==========================================================================
    void masterHandoverIsStep()
    {
        beginTest ("Master handover records a single step breakpoint");

        Harness h;
        double       bpm  = 124.0;
        std::int64_t head = 0;
        int          deck = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return true; },
            [&] { return head; },
            h.sink,
            [&] { return deck; });

        cap.seedAtRecordStart(); // (0, 124) deck 0

        // A different deck becomes master with a discontinuous BPM jump.
        head = 500;
        deck = 1;
        bpm  = 128.0;
        cap.captureTick();

        auto lane = h.tempoLane();
        expect (lane.isValid());
        expectEquals (lane.getNumBreakpoints(), 2);

        // The predecessor segment holds (step) until the handover sample.
        expect (ContinuousLane::interpolationOfNode (lane.getBreakpoint (0)) == Interpolation::Step);
        expect (ContinuousLane::interpolationOfNode (lane.getBreakpoint (1)) == Interpolation::Step);

        // Step/hold: prior tempo holds across the segment, then jumps.
        auto before = lane.evaluateAt (250);
        expect (before.has_value());
        expectWithinAbsoluteError (*before, 124.0, 1.0e-9);

        auto at = lane.evaluateAt (500);
        expect (at.has_value());
        expectWithinAbsoluteError (*at, 128.0, 1.0e-9);
    }

    //==========================================================================
    void disarmedDoesNothing()
    {
        beginTest ("Disarmed capture appends nothing");

        Harness h;
        double       bpm  = 124.0;
        std::int64_t head = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return false; },
            [&] { return head; },
            h.sink);

        for (int i = 0; i < 5; ++i)
        {
            head = (std::int64_t) i * 100;
            bpm  = 124.0 + (double) i;
            cap.captureTick();
        }

        expect (! h.model.hasLane (AutomationStrings::kOwnerMaster, "tempo"));
    }

    //==========================================================================
    void unchangedBpmAppendsNothing()
    {
        beginTest ("Unchanged BPM (pause / resume churn) appends no breakpoints");

        Harness h;
        double       bpm  = 124.0;
        std::int64_t head = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return true; },
            [&] { return head; },
            h.sink);

        cap.seedAtRecordStart();

        // Many ticks at increasing playhead but identical BPM (pause / resume).
        for (int i = 1; i <= 20; ++i)
        {
            head = (std::int64_t) i * 100;
            cap.captureTick();
        }

        auto lane = h.tempoLane();
        expectEquals (lane.getNumBreakpoints(), 1);
    }

    //==========================================================================
    void dormantSeedUsesLastKnownNonZero()
    {
        beginTest ("Dormant seed uses the last-known non-zero BPM, never 0.0");

        Harness h;
        double       bpm  = 126.5; // retained last-known BPM while dormant
        std::int64_t head = 0;

        MasterTempoAutomationCapture cap (
            [&] { return bpm; },
            [] { return true; },
            [&] { return head; },
            h.sink);

        cap.seedAtRecordStart();

        auto lane = h.tempoLane();
        expectEquals (lane.getNumBreakpoints(), 1);
        const double seeded = ContinuousLane::valueOfNode (lane.getBreakpoint (0));
        expect (seeded > 0.0);
        expectWithinAbsoluteError (seeded, 126.5, 1.0e-9);
    }
};

static MasterTempoAutomationCaptureTests masterTempoAutomationCaptureTests;
