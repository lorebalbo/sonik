//==============================================================================
// PRD-0088: AutomationCaptureTaps tests — disarmed no-op, armed continuous
// breakpoint at correct sample/value, armed boolean step, same-sample
// coalescing, value thinning of a rapid sweep, and use of the injected bridge.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Daw/Automation/AutomationCaptureTaps.h"
#include "../Source/Features/Daw/Automation/AutomationModel.h"
#include "../Source/Features/Daw/State/DawState.h"

using namespace Daw;

namespace
{
    // A spy bridge recording every append, plus delegating to a real model so we
    // can assert the resulting lane contents.
    struct SpySink final : public AutomationAppendSink
    {
        explicit SpySink (AutomationModel& m) : model (m) {}

        int breakpointAppends = 0;
        int breakpointUpdates = 0;
        int stepAppends       = 0;

        juce::ValueTree appendBreakpoint (const juce::String& owner,
                                          const juce::String& parameterId,
                                          std::int64_t        timelineSample,
                                          double              value,
                                          Interpolation       interpolation) override
        {
            ++breakpointAppends;
            auto lane = model.getOrCreateContinuousLane (owner, parameterId);
            return lane.addBreakpoint (timelineSample, value, interpolation);
        }

        void updateBreakpoint (juce::ValueTree bp, std::int64_t s, double v) override
        {
            ++breakpointUpdates;
            bp.setProperty (AutomationIDs::timelineSample, (juce::int64) s, nullptr);
            bp.setProperty (AutomationIDs::value, v, nullptr);
        }

        void setBreakpointInterpolation (juce::ValueTree bp, Interpolation interp) override
        {
            bp.setProperty (AutomationIDs::interpolation, interpolationToString (interp), nullptr);
        }

        juce::ValueTree appendStep (const juce::String& owner,
                                    const juce::String& parameterId,
                                    std::int64_t        timelineSample,
                                    bool                value) override
        {
            ++stepAppends;
            auto lane = model.getOrCreateBooleanLane (owner, parameterId);
            return lane.addStep (timelineSample, value);
        }

        AutomationModel& model;
    };
}

class AutomationCaptureTapsTests final : public juce::UnitTest
{
public:
    AutomationCaptureTapsTests() : juce::UnitTest ("Automation Capture Taps", "Sonik") {}

    void runTest() override
    {
        testDisarmedIsNoOp();
        testArmedContinuousAppend();
        testArmedBooleanStep();
        testSameSampleCoalescing();
        testRapidSweepThinning();
    }

private:
    // A synthetic authoritative source tree carrying a continuous "gain" and a
    // boolean "keyLock" property, mirroring how mixer / deck nodes hold params.
    juce::ValueTree makeSource()
    {
        juce::ValueTree src ("source");
        src.setProperty ("gain", 0.0, nullptr);
        src.setProperty ("keyLock", false, nullptr);
        return src;
    }

    void testDisarmedIsNoOp()
    {
        beginTest ("disarmed: parameter change produces zero appends");

        auto daw = DawState::createDawBranch();
        AutomationModel model (daw);
        SpySink sink (model);

        bool armed = false;
        std::int64_t playhead = 1000;
        AutomationCaptureTaps taps ([&] { return armed; }, [&] { return playhead; }, sink);
        taps.registerContinuousTap (makeSourceMember(), "gain", "A", "gain");

        source_.setProperty ("gain", 0.5, nullptr);
        expectEquals (sink.breakpointAppends, 0, "no append while disarmed");
        expect (! model.hasLane ("A", "gain"), "no lane created while disarmed");
    }

    void testArmedContinuousAppend()
    {
        beginTest ("armed: continuous change appends a breakpoint at playhead/value");

        auto daw = DawState::createDawBranch();
        AutomationModel model (daw);
        SpySink sink (model);

        bool armed = true;
        std::int64_t playhead = 4410;
        AutomationCaptureTaps taps ([&] { return armed; }, [&] { return playhead; }, sink);
        taps.registerContinuousTap (makeSourceMember(), "gain", "A", "gain");

        source_.setProperty ("gain", 0.75, nullptr);

        expectEquals (sink.breakpointAppends, 1);
        auto lane = model.getContinuousLane ("A", "gain");
        expect (lane.isValid());
        expectEquals (lane.getNumBreakpoints(), 1);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (0)), 4410);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (0)), 0.75, 1.0e-9);
    }

    void testArmedBooleanStep()
    {
        beginTest ("armed: boolean toggle appends a step");

        auto daw = DawState::createDawBranch();
        AutomationModel model (daw);
        SpySink sink (model);

        bool armed = true;
        std::int64_t playhead = 100;
        AutomationCaptureTaps taps ([&] { return armed; }, [&] { return playhead; }, sink);
        taps.registerBooleanTap (makeSourceMember(), "keyLock", "A", "keyLock");

        playhead = 200;
        source_.setProperty ("keyLock", true, nullptr);
        playhead = 800;
        source_.setProperty ("keyLock", false, nullptr);

        expectEquals (sink.stepAppends, 2, "every toggle captured, never thinned");
        auto lane = model.getBooleanLane ("A", "keyLock");
        expect (lane.isValid());
        expectEquals (lane.getNumSteps(), 2);
        expect (lane.stateAt (500), "true held between toggles");
        expect (! lane.stateAt (900), "false after the off toggle");
    }

    void testSameSampleCoalescing()
    {
        beginTest ("armed: same-playhead burst coalesces to one breakpoint");

        auto daw = DawState::createDawBranch();
        AutomationModel model (daw);
        SpySink sink (model);

        bool armed = true;
        std::int64_t playhead = 5000; // does not advance across the burst
        AutomationCaptureTaps taps ([&] { return armed; }, [&] { return playhead; }, sink);
        taps.registerContinuousTap (makeSourceMember(), "gain", "A", "gain");

        source_.setProperty ("gain", 0.1, nullptr);
        source_.setProperty ("gain", 0.4, nullptr);
        source_.setProperty ("gain", 0.9, nullptr);

        expectEquals (sink.breakpointAppends, 1, "only the first append creates a node");
        expect (sink.breakpointUpdates >= 1, "subsequent same-sample values update in place");

        auto lane = model.getContinuousLane ("A", "gain");
        expectEquals (lane.getNumBreakpoints(), 1, "no duplicate breakpoints at the same sample");
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (0)), 0.9, 1.0e-9);
    }

    void testRapidSweepThinning()
    {
        beginTest ("armed: rapid sweep is thinned by the value deadband");

        auto daw = DawState::createDawBranch();
        AutomationModel model (daw);
        SpySink sink (model);

        bool armed = true;
        std::int64_t playhead = 0;
        AutomationCaptureTaps taps ([&] { return armed; }, [&] { return playhead; }, sink);

        ThinningPolicy policy;
        policy.valueDeadband = 0.05; // collapse sub-5% jitter
        taps.registerContinuousTap (makeSourceMember(), "gain", "A", "gain",
                                    Interpolation::Linear, policy);

        // 100 raw changes advancing the playhead, ramping 0 -> 1 in 0.01 steps.
        for (int i = 1; i <= 100; ++i)
        {
            playhead = i * 441; // advance so coalescing does not apply
            source_.setProperty ("gain", i * 0.01, nullptr);
        }

        // With a 0.05 deadband a 0..1 ramp yields roughly 20 breakpoints, far
        // fewer than the 100 raw changes, and bounded well under it.
        const int appended = sink.breakpointAppends;
        expect (appended < 100, "thinned below the raw change count");
        expect (appended <= 30, "bounded breakpoint count for a smooth sweep");
        expect (appended >= 10, "still a faithful number of points");
    }

    // Helper: each test uses a fresh source_ member kept alive for the test body.
    juce::ValueTree makeSourceMember()
    {
        source_ = makeSource();
        return source_;
    }

    juce::ValueTree source_;
};

static AutomationCaptureTapsTests automationCaptureTapsTests;
