#include "../Source/Features/Daw/Automation/AutomationModel.h"
#include "../Source/Features/Daw/Automation/ContinuousLane.h"
#include "../Source/Features/Daw/Automation/AutomationCaptureTaps.h"
#include "../Source/Features/Daw/Automation/ChannelContinuousAutomationCapture.h"
#include "../Source/Features/Daw/State/DawState.h"
#include "../Source/Features/Mixer/State/MixerStateSchema.h"
#include "../Source/Features/Mixer/State/MixerIdentifiers.h"

#include <juce_data_structures/juce_data_structures.h>

using namespace Daw;

namespace
{

struct Harness
{
    juce::ValueTree           root  { "SonikState" };
    MixerStateSchema          mixer { root };
    juce::ValueTree           daw   { DawState::createDawBranch() };
    AutomationModel           model { daw, nullptr };
    ModelAutomationAppendSink sink  { model, nullptr };

    bool         armed    { false };
    std::int64_t playhead { 0 };

    AutomationCaptureTaps taps { [this] { return armed; },
                                 [this] { return playhead; },
                                 sink };

    Harness()
    {
        ChannelContinuousAutomationCapture::registerTaps (taps, mixer);
    }

    ContinuousLane lane (const juce::String& owner, const juce::String& paramId)
    {
        return model.getContinuousLane (owner, paramId);
    }
};

} // namespace

class ContinuousLaneCaptureTests final : public juce::UnitTest
{
public:
    ContinuousLaneCaptureTests()
        : juce::UnitTest ("Per-Channel Continuous Lane Capture (PRD-0090)", "Sonik") {}

    void runTest() override
    {
        twentyLanesWithCorrectKeys();
        initialBreakpointPerLaneAtRecordStart();
        armedFilterSweepIsDecimatedIntoChannelALane();
        disarmedSweepAppendsNothing();
        eqRideCapturedInCorrectBandInDecibels();
        filterDetentValueCapturedVerbatim();
    }

private:
    //==========================================================================
    void twentyLanesWithCorrectKeys()
    {
        beginTest ("Twenty continuous lanes exist with the correct (channel, parameter) keys");

        Harness h;
        h.taps.captureInitialValues (0); // lazily creates all twenty lanes

        expectEquals (h.taps.getNumTaps(), 20);
        expectEquals (h.model.getNumLanes(), 20);

        const char* owners[4] = { "A", "B", "C", "D" };
        const char* params[5] = { "filter", "gain", "eq.high", "eq.mid", "eq.low" };
        for (auto* o : owners)
            for (auto* p : params)
                expect (h.model.hasLane (o, p), juce::String ("missing lane ") + o + "." + p);
    }

    //==========================================================================
    void initialBreakpointPerLaneAtRecordStart()
    {
        beginTest ("Record-start seeds one initial breakpoint per lane at the current value");

        Harness h;
        const std::int64_t start = 1000;
        h.taps.captureInitialValues (start);

        auto filterLane = h.lane ("A", "filter");
        expectEquals (filterLane.getNumBreakpoints(), 1);
        expectEquals ((int) ContinuousLane::sampleOfNode (filterLane.getBreakpoint (0)), 1000);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (filterLane.getBreakpoint (0)),
                                   (double) MixerStateSchema::kDefaultFilter, 1.0e-9);

        auto gainLane = h.lane ("A", "gain");
        expectEquals (gainLane.getNumBreakpoints(), 1);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (gainLane.getBreakpoint (0)),
                                   (double) MixerStateSchema::kDefaultGainDb, 1.0e-9);

        auto eqLane = h.lane ("C", "eq.mid");
        expectEquals (eqLane.getNumBreakpoints(), 1);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (eqLane.getBreakpoint (0)),
                                   (double) MixerStateSchema::kDefaultEqDb, 1.0e-9);
    }

    //==========================================================================
    void armedFilterSweepIsDecimatedIntoChannelALane()
    {
        beginTest ("Armed filter sweep on channel A is decimated into channel A's filter lane only");

        Harness h;
        h.taps.captureInitialValues (0);
        h.armed = true;

        auto channelA = h.mixer.getChannelTree (0);

        // A fine 200-step sweep 0 -> 1.0 (0.005 per callback) — far finer than the
        // 0.01 filter decimation threshold, so most callbacks are thinned away.
        const int steps = 200;
        for (int i = 1; i <= steps; ++i)
        {
            h.playhead = i;
            channelA.setProperty (MixerIDs::filter, (double) i * (1.0 / (double) steps), nullptr);
        }

        auto filterLane = h.lane ("A", "filter");
        const int n = filterLane.getNumBreakpoints();

        // Decimated: far fewer breakpoints than callbacks, but enough to preserve
        // the gesture's shape.
        expect (n < steps, "expected decimation below one breakpoint per callback");
        expect (n > 10, "expected enough breakpoints to preserve the sweep shape");

        // Reconstructs the ramp: midpoint ~0.5 within the decimation tolerance.
        auto mid = filterLane.evaluateAt (h.playhead / 2);
        expect (mid.has_value());
        expectWithinAbsoluteError (*mid, 0.5, 0.05);

        // No other lane received anything beyond its seed.
        expectEquals (h.lane ("A", "gain").getNumBreakpoints(), 1);
        expectEquals (h.lane ("B", "filter").getNumBreakpoints(), 1);
        expectEquals (h.lane ("A", "eq.high").getNumBreakpoints(), 1);
    }

    //==========================================================================
    void disarmedSweepAppendsNothing()
    {
        beginTest ("A parameter change while not armed appends no breakpoints");

        Harness h;
        h.taps.captureInitialValues (0);
        const int before = h.lane ("A", "filter").getNumBreakpoints();

        h.armed = false;
        auto channelA = h.mixer.getChannelTree (0);
        for (int i = 1; i <= 10; ++i)
        {
            h.playhead = 5000 + i;
            channelA.setProperty (MixerIDs::filter, (double) i * 0.1, nullptr);
        }

        expectEquals (h.lane ("A", "filter").getNumBreakpoints(), before);
    }

    //==========================================================================
    void eqRideCapturedInCorrectBandInDecibels()
    {
        beginTest ("EQ ride is captured into the correct per-band lane in dB");

        Harness h;
        h.taps.captureInitialValues (0);
        h.armed = true;

        auto eqB = h.mixer.getChannelEqTree (1); // channel B

        // Ride low band down to a deep cut, in dB.
        for (int i = 1; i <= 24; ++i)
        {
            h.playhead = 100 + i;
            eqB.setProperty (MixerIDs::low, -0.5 * (double) i, nullptr); // 0 -> -12 dB
        }

        auto lowLane = h.lane ("B", "eq.low");
        expect (lowLane.getNumBreakpoints() > 1);
        const double last = ContinuousLane::valueOfNode (
            lowLane.getBreakpoint (lowLane.getNumBreakpoints() - 1));
        expect (last < -6.0, "expected a deep dB cut captured in native dB units");

        // The other EQ bands of channel B and the low band of other channels were
        // untouched (seed only).
        expectEquals (h.lane ("B", "eq.high").getNumBreakpoints(), 1);
        expectEquals (h.lane ("B", "eq.mid").getNumBreakpoints(), 1);
        expectEquals (h.lane ("A", "eq.low").getNumBreakpoints(), 1);
    }

    //==========================================================================
    void filterDetentValueCapturedVerbatim()
    {
        beginTest ("Filter lane records the authoritative (post-detent) value verbatim, including 0.0");

        Harness h;
        h.taps.captureInitialValues (0);
        h.armed = true;

        auto channelA = h.mixer.getChannelTree (0);

        h.playhead = 10;
        channelA.setProperty (MixerIDs::filter, 0.6, nullptr);

        // A sweep landing back in the detent is authoritatively snapped to 0.0 by
        // the mixer setter (PRD-0056); capture reads the ValueTree value verbatim.
        h.playhead = 20;
        channelA.setProperty (MixerIDs::filter, 0.0, nullptr);

        // Terminate the lane on the resting value.
        h.taps.flush (30);

        auto filterLane = h.lane ("A", "filter");
        const double resting = ContinuousLane::valueOfNode (
            filterLane.getBreakpoint (filterLane.getNumBreakpoints() - 1));
        expectWithinAbsoluteError (resting, 0.0, 1.0e-9);
    }
};

static ContinuousLaneCaptureTests continuousLaneCaptureTests;
