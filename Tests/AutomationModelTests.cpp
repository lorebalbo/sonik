//==============================================================================
// PRD-0087: AutomationModel tests — lane keying/uniqueness, continuous
// breakpoint ordering + evaluateAt (linear/step), boolean stepAt, empty-vs-
// absent distinction, kind consistency, novel-id acceptance, raw-unit storage,
// and ValueTree::Listener observability.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Daw/State/DawState.h"
#include "../Source/Features/Daw/Automation/AutomationModel.h"

using namespace Daw;

namespace
{
    // Minimal listener spy counting the callbacks we care about.
    struct ListenerSpy final : public juce::ValueTree::Listener
    {
        int propertyChanges = 0;
        int childrenAdded   = 0;
        int childrenRemoved = 0;

        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++propertyChanges; }
        void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override           { ++childrenAdded; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { ++childrenRemoved; }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
        void valueTreeParentChanged (juce::ValueTree&) override {}
    };
}

class AutomationModelTests final : public juce::UnitTest
{
public:
    AutomationModelTests() : juce::UnitTest ("Automation Data Model", "Sonik") {}

    void runTest() override
    {
        testContainerAndKeying();
        testKindConsistency();
        testNovelParameterId();
        testContinuousOrderingAndEval();
        testStepInterpolation();
        testBooleanLane();
        testEmptyVsAbsent();
        testRawUnitsRoundTrip();
        testInScopeParameterSet();
        testObservability();
        testEnableFlag();
    }

private:
    juce::ValueTree makeDaw() { return DawState::createDawBranch(); }

    void testContainerAndKeying()
    {
        beginTest ("automation container + (owner,parameterId) keying / uniqueness");

        auto daw = makeDaw();
        AutomationModel model (daw);

        expect (daw.getChildWithName (AutomationIDs::automation).isValid(),
                "automation container node created under daw");

        auto a = model.getOrCreateContinuousLane ("A", "filter");
        expect (a.isValid());
        expectEquals (model.getNumLanes(), 1);

        // Second get-or-create for the same pair returns the existing lane.
        auto a2 = model.getOrCreateContinuousLane ("A", "filter");
        expect (a2.isValid());
        expectEquals (model.getNumLanes(), 1, "no duplicate lane for the same pair");
        expect (a2.getState() == a.getState());

        // Different owner / different parameter make distinct lanes.
        model.getOrCreateContinuousLane ("B", "filter");
        model.getOrCreateContinuousLane ("A", "gain");
        expectEquals (model.getNumLanes(), 3);

        // Master owner is a first-class value.
        model.getOrCreateContinuousLane ("master", "tempo");
        expect (model.hasLane ("master", "tempo"));
        expectEquals (model.getNumLanes(), 4);
    }

    void testKindConsistency()
    {
        beginTest ("kind consistency for in-scope ids");

        auto daw = makeDaw();
        AutomationModel model (daw);

        // Requesting a boolean lane for a continuous id is rejected.
        auto bad = model.getOrCreateBooleanLane ("A", "filter");
        expect (! bad.isValid(), "boolean lane for continuous id rejected");
        expectEquals (model.getNumLanes(), 0);

        // Requesting a continuous lane for a boolean id is rejected.
        auto bad2 = model.getOrCreateContinuousLane ("A", "keyStepper");
        expect (! bad2.isValid(), "continuous lane for boolean id rejected");
        expectEquals (model.getNumLanes(), 0);

        // Correct kinds succeed.
        expect (model.getOrCreateBooleanLane ("A", "keyLock").isValid());
        expect (model.getOrCreateContinuousLane ("A", "tempo").isValid());
    }

    void testNovelParameterId()
    {
        beginTest ("novel parameter id accepted without migration");

        auto daw = makeDaw();
        AutomationModel model (daw);

        auto cont = model.getOrCreateContinuousLane ("A", "crossfader");
        expect (cont.isValid(), "novel id usable as continuous");
        cont.addBreakpoint (100, 0.25);
        expectEquals (cont.getNumBreakpoints(), 1);

        auto boolLane = model.getOrCreateBooleanLane ("master", "someFutureToggle");
        expect (boolLane.isValid(), "novel id usable as boolean");
    }

    void testContinuousOrderingAndEval()
    {
        beginTest ("continuous breakpoint ordering + linear evaluateAt");

        auto daw = makeDaw();
        AutomationModel model (daw);
        auto lane = model.getOrCreateContinuousLane ("A", "filter");

        // Insert out of order; expect ascending order maintained.
        lane.addBreakpoint (1000, 1.0);
        lane.addBreakpoint (0,    0.0);
        lane.addBreakpoint (500,  0.5);

        expectEquals (lane.getNumBreakpoints(), 3);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (0)), 0);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (1)), 500);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (2)), 1000);

        // Linear interpolation halfway between 0 and 500.
        auto v = lane.evaluateAt (250);
        expect (v.has_value());
        expectWithinAbsoluteError (*v, 0.25, 1.0e-9);

        // Before first / after last hold the endpoints.
        expectWithinAbsoluteError (*lane.evaluateAt (-100), 0.0, 1.0e-9);
        expectWithinAbsoluteError (*lane.evaluateAt (5000), 1.0, 1.0e-9);

        // Exact breakpoint hit.
        expectWithinAbsoluteError (*lane.evaluateAt (500), 0.5, 1.0e-9);
    }

    void testStepInterpolation()
    {
        beginTest ("continuous step/hold interpolation");

        auto daw = makeDaw();
        AutomationModel model (daw);
        auto lane = model.getOrCreateContinuousLane ("A", "gain");

        lane.addBreakpoint (0,    0.0, Interpolation::Step);
        lane.addBreakpoint (1000, 6.0, Interpolation::Linear);

        // Inside the step segment the leading value is held.
        expectWithinAbsoluteError (*lane.evaluateAt (500), 0.0, 1.0e-9);
        // The trailing breakpoint value is reached at its sample.
        expectWithinAbsoluteError (*lane.evaluateAt (1000), 6.0, 1.0e-9);
    }

    void testBooleanLane()
    {
        beginTest ("boolean lane stepAt + ordering");

        auto daw = makeDaw();
        AutomationModel model (daw);
        auto lane = model.getOrCreateBooleanLane ("A", "keyLock");

        lane.addStep (2000, false);
        lane.addStep (0,    false);
        lane.addStep (1000, true);

        // Ordering ascending.
        expectEquals ((int) BooleanLane::sampleOfNode (lane.getStep (0)), 0);
        expectEquals ((int) BooleanLane::sampleOfNode (lane.getStep (1)), 1000);
        expectEquals ((int) BooleanLane::sampleOfNode (lane.getStep (2)), 2000);

        expect (! lane.stateAt (-5), "false before first step");
        expect (! lane.stateAt (500), "held false");
        expect (lane.stateAt (1000), "true at the on step");
        expect (lane.stateAt (1500), "held true");
        expect (! lane.stateAt (2500), "held false after off");
    }

    void testEmptyVsAbsent()
    {
        beginTest ("empty lane distinct from absent lane");

        auto daw = makeDaw();
        AutomationModel model (daw);

        // Absent.
        expect (! model.hasLane ("A", "filter"));
        expect (! model.getContinuousLane ("A", "filter").isValid());

        // Empty (created, no breakpoints).
        auto lane = model.getOrCreateContinuousLane ("A", "filter");
        expect (model.hasLane ("A", "filter"));
        expect (lane.isEmpty());
        expect (! lane.evaluateAt (123).has_value(), "empty continuous lane returns no value");

        auto boolLane = model.getOrCreateBooleanLane ("A", "keyLock");
        expect (boolLane.isEmpty());
        expect (! boolLane.stateAt (123), "empty boolean lane defaults false");
    }

    void testRawUnitsRoundTrip()
    {
        beginTest ("raw parameter units round-trip (tempo BPM + EQ dB)");

        auto daw = makeDaw();
        AutomationModel model (daw);

        auto tempo = model.getOrCreateContinuousLane ("master", "tempo");
        auto bp = tempo.addBreakpoint (0, 128.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (bp), 128.0, 1.0e-9);

        auto eq = model.getOrCreateContinuousLane ("B", "eq.high");
        auto eqBp = eq.addBreakpoint (0, -6.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (eqBp), -6.0, 1.0e-9);
    }

    void testInScopeParameterSet()
    {
        beginTest ("in-scope parameter id set round-trips");

        auto daw = makeDaw();
        AutomationModel model (daw);

        expect (model.getOrCreateContinuousLane ("master", "tempo").isValid());
        for (auto* ch : { "A", "B", "C", "D" })
        {
            expect (model.getOrCreateContinuousLane (ch, "filter").isValid());
            expect (model.getOrCreateContinuousLane (ch, "high").isValid());
            expect (model.getOrCreateContinuousLane (ch, "mid").isValid());
            expect (model.getOrCreateContinuousLane (ch, "low").isValid());
            expect (model.getOrCreateContinuousLane (ch, "gain").isValid());
            expect (model.getOrCreateBooleanLane (ch, "keyLock").isValid());
            expect (model.getOrCreateBooleanLane (ch, "pitchStretch").isValid());
            expect (model.getOrCreateBooleanLane (ch, "keyStepper").isValid());
        }
        // 1 master + 4*(5 continuous + 3 boolean) = 33 lanes.
        expectEquals (model.getNumLanes(), 33);
    }

    void testObservability()
    {
        beginTest ("ValueTree::Listener fires on lane/breakpoint changes");

        auto daw = makeDaw();
        AutomationModel model (daw);

        ListenerSpy spy;
        // The listener registers on this specific ValueTree instance, so it must
        // outlive the observation window (a temporary would deregister at once).
        auto automationTree = model.getAutomationTree();
        automationTree.addListener (&spy);

        auto lane = model.getOrCreateContinuousLane ("A", "filter");
        expect (spy.childrenAdded >= 1, "lane add observed");

        auto bp = lane.addBreakpoint (0, 0.0);
        expect (spy.childrenAdded >= 2, "breakpoint add observed");

        const int beforeMove = spy.propertyChanges;
        lane.moveBreakpoint (bp, 1000, 0.5);
        expect (spy.propertyChanges > beforeMove, "breakpoint move observed");

        const int beforeInterp = spy.propertyChanges;
        lane.setInterpolation (bp, Interpolation::Step);
        expect (spy.propertyChanges > beforeInterp, "interpolation change observed");

        automationTree.removeListener (&spy);
    }

    void testEnableFlag()
    {
        beginTest ("per-lane enable flag defaults true and toggles");

        auto daw = makeDaw();
        AutomationModel model (daw);
        auto lane = model.getOrCreateContinuousLane ("A", "filter");

        expect (AutomationModel::isLaneEnabled (lane.getState()), "enabled by default");
        model.setLaneEnabled (lane.getState(), false);
        expect (! AutomationModel::isLaneEnabled (lane.getState()), "bypass toggles flag");
    }
};

static AutomationModelTests automationModelTests;
