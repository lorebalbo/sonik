//==============================================================================
// PRD-0091: Per-Channel Boolean Lane Capture tests.
//
// Drives synthetic recording sessions over the authoritative deck ValueTree
// parameters (keyLockEnabled, keyShift) and asserts the EXACT ordered step
// events captured into each derived boolean lane (keyLock / pitchStretch /
// keyStepper), including the record-start seed, neutral-crossing semantics for
// the key-stepper, the derived pitch-stretch condition, debounce coalescing,
// the disarmed gate, and per-deck independence.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Daw/Automation/AutomationModel.h"
#include "../Source/Features/Daw/Automation/BooleanLane.h"
#include "../Source/Features/Daw/Automation/AutomationCaptureTaps.h"
#include "../Source/Features/Daw/Automation/ChannelBooleanAutomationCapture.h"
#include "../Source/Features/Daw/State/DawState.h"
#include "../Source/Features/Deck/DeckIdentifiers.h"

#include <cstdint>
#include <vector>

using namespace Daw;

namespace
{

// A recording session: a root tree with Decks/Deck nodes for A..D carrying
// keyLockEnabled/keyShift, a `daw` branch + AutomationModel + ModelAppendSink,
// and a ChannelBooleanAutomationCapture observing the decks. `armed` and
// `playhead` are read by the injected predicates. Deck nodes are populated in
// the constructor BEFORE the capture is built so its resolver returns valid
// trees and primes correctly.
struct Session
{
    juce::ValueTree           root  { IDs::SonikState };
    juce::ValueTree           decks { IDs::Decks };
    juce::ValueTree           daw   { DawState::createDawBranch() };
    AutomationModel           model { daw, nullptr };
    ModelAutomationAppendSink sink  { model, nullptr };

    bool         armed    { false };
    std::int64_t playhead { 0 };

    juce::ValueTree deckNodes[4];

    std::unique_ptr<ChannelBooleanAutomationCapture> capture;

    Session()
    {
        static const char* const letters[4] = { "A", "B", "C", "D" };
        root.appendChild (decks, nullptr);
        for (int ch = 0; ch < 4; ++ch)
        {
            juce::ValueTree d (IDs::Deck);
            d.setProperty (IDs::id, juce::String (letters[ch]), nullptr);
            d.setProperty (IDs::keyLockEnabled, false, nullptr);
            d.setProperty (IDs::keyShift, 0, nullptr);
            decks.appendChild (d, nullptr);
            deckNodes[ch] = d;
        }

        capture = std::make_unique<ChannelBooleanAutomationCapture> (
            [this] (int ch) { return ch >= 0 && ch < 4 ? deckNodes[ch] : juce::ValueTree {}; },
            [this] { return armed; },
            [this] { return playhead; },
            sink);
    }

    void setKeyLock (int ch, bool on)  { deckNodes[ch].setProperty (IDs::keyLockEnabled, on, nullptr); }
    void setKeyShift (int ch, int v)   { deckNodes[ch].setProperty (IDs::keyShift, v, nullptr); }

    BooleanLane lane (const juce::String& owner, const juce::String& paramId)
    {
        return model.getBooleanLane (owner, paramId);
    }
};

struct Step { std::int64_t sample; bool state; };

std::vector<Step> stepsOf (const BooleanLane& lane)
{
    std::vector<Step> out;
    for (int i = 0; i < lane.getNumSteps(); ++i)
    {
        auto s = lane.getStep (i);
        out.push_back ({ BooleanLane::sampleOfNode (s), BooleanLane::valueOfNode (s) });
    }
    return out;
}

} // namespace

class ChannelBooleanLaneCaptureTests final : public juce::UnitTest
{
public:
    ChannelBooleanLaneCaptureTests()
        : juce::UnitTest ("Per-Channel Boolean Lane Capture (PRD-0091)", "Sonik") {}

    void runTest() override
    {
        twelveLanesWithCorrectKeys();
        recordStartSeedsDerivedInitialState();
        disarmedAppendsNothing();
        armedKeyLockOnThenOff();
        armedKeyStepperAcrossNeutralPositive();
        armedKeyStepperAcrossNeutralNegative();
        armedPitchStretchDerivedSequence();
        debounceCoalescesZeroDurationPair();
        debounceOutsideWindowKeepsBoth();
        perDeckIndependence();
    }

private:
    //==========================================================================
    void expectSteps (const std::vector<Step>& got,
                      const std::vector<Step>& want,
                      const juce::String&      what)
    {
        expectEquals ((int) got.size(), (int) want.size(), what + " (count)");
        const int n = juce::jmin ((int) got.size(), (int) want.size());
        for (int i = 0; i < n; ++i)
        {
            expectEquals ((int) got[(size_t) i].sample, (int) want[(size_t) i].sample,
                          what + " step " + juce::String (i) + " sample");
            expect (got[(size_t) i].state == want[(size_t) i].state,
                    what + " step " + juce::String (i) + " state");
        }
    }

    //==========================================================================
    void twelveLanesWithCorrectKeys()
    {
        beginTest ("Twelve boolean lanes exist (4 channels x 3 params) with correct keys after seeding");

        Session s;
        s.capture->captureInitialValues (0);

        expectEquals (s.model.getNumLanes(), 12);

        const char* owners[4] = { "A", "B", "C", "D" };
        const char* params[3] = { "keyLock", "pitchStretch", "keyStepper" };
        for (auto* o : owners)
            for (auto* p : params)
                expect (s.model.hasLane (o, p), juce::String ("missing lane ") + o + "." + p);
    }

    //==========================================================================
    void recordStartSeedsDerivedInitialState()
    {
        beginTest ("Record start seeds one initial step per lane reflecting the derived value");

        Session s;
        // Deck B: keyLock on, keyShift +2 -> all three derived booleans on.
        s.setKeyLock (1, true);
        s.setKeyShift (1, 2);

        const std::int64_t start = 1000;
        s.capture->captureInitialValues (start);

        // Deck B: all on at recordStart.
        expectSteps (stepsOf (s.lane ("B", "keyLock")),      { { start, true } }, "B.keyLock seed");
        expectSteps (stepsOf (s.lane ("B", "pitchStretch")), { { start, true } }, "B.pitchStretch seed");
        expectSteps (stepsOf (s.lane ("B", "keyStepper")),   { { start, true } }, "B.keyStepper seed");

        // Deck A: defaults -> all off at recordStart.
        expectSteps (stepsOf (s.lane ("A", "keyLock")),      { { start, false } }, "A.keyLock seed");
        expectSteps (stepsOf (s.lane ("A", "pitchStretch")), { { start, false } }, "A.pitchStretch seed");
        expectSteps (stepsOf (s.lane ("A", "keyStepper")),   { { start, false } }, "A.keyStepper seed");
    }

    //==========================================================================
    void disarmedAppendsNothing()
    {
        beginTest ("While disarmed, no steps are appended on any deck-property change");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = false;

        s.playhead = 500;
        s.setKeyLock (0, true);
        s.playhead = 600;
        s.setKeyShift (0, 3);
        s.playhead = 700;
        s.setKeyLock (0, false);

        // Only the single seed remains in each lane.
        expectEquals (s.lane ("A", "keyLock").getNumSteps(), 1);
        expectEquals (s.lane ("A", "pitchStretch").getNumSteps(), 1);
        expectEquals (s.lane ("A", "keyStepper").getNumSteps(), 1);
    }

    //==========================================================================
    void armedKeyLockOnThenOff()
    {
        beginTest ("Armed keyLock on then off appends on-step then off-step at the right samples");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;

        s.playhead = 4000;
        s.setKeyLock (0, true);
        s.playhead = 9000;
        s.setKeyLock (0, false);

        expectSteps (stepsOf (s.lane ("A", "keyLock")),
                     { { 0, false }, { 4000, true }, { 9000, false } },
                     "A.keyLock on/off");

        // pitchStretch follows keyLock here (keyShift stayed 0).
        expectSteps (stepsOf (s.lane ("A", "pitchStretch")),
                     { { 0, false }, { 4000, true }, { 9000, false } },
                     "A.pitchStretch follows keyLock");

        // keyStepper untouched (only the seed).
        expectSteps (stepsOf (s.lane ("A", "keyStepper")),
                     { { 0, false } }, "A.keyStepper untouched");
    }

    //==========================================================================
    void armedKeyStepperAcrossNeutralPositive()
    {
        beginTest ("Armed keyStepper across neutral (positive): 0->+1 on, +1->+2->+3 nothing, +3->0 off");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;

        s.playhead = 1000; s.setKeyShift (0, 1);  // engage -> on
        s.playhead = 2000; s.setKeyShift (0, 2);  // stay engaged -> nothing
        s.playhead = 3000; s.setKeyShift (0, 3);  // stay engaged -> nothing
        s.playhead = 4000; s.setKeyShift (0, 0);  // return to neutral -> off

        expectSteps (stepsOf (s.lane ("A", "keyStepper")),
                     { { 0, false }, { 1000, true }, { 4000, false } },
                     "A.keyStepper +ve crossing");

        // pitchStretch (keyLock off) tracks keySh!=0: on at 1000, off at 4000.
        expectSteps (stepsOf (s.lane ("A", "pitchStretch")),
                     { { 0, false }, { 1000, true }, { 4000, false } },
                     "A.pitchStretch +ve crossing");
    }

    //==========================================================================
    void armedKeyStepperAcrossNeutralNegative()
    {
        beginTest ("Armed keyStepper across neutral (negative): 0->-1 on, -1->-3 nothing, -3->0 off");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;

        s.playhead = 1000; s.setKeyShift (0, -1); // engage -> on
        s.playhead = 2000; s.setKeyShift (0, -3); // stay engaged -> nothing
        s.playhead = 3000; s.setKeyShift (0, 0);  // return to neutral -> off

        expectSteps (stepsOf (s.lane ("A", "keyStepper")),
                     { { 0, false }, { 1000, true }, { 3000, false } },
                     "A.keyStepper -ve crossing");
    }

    //==========================================================================
    void armedPitchStretchDerivedSequence()
    {
        beginTest ("Armed pitchStretch derived: keyShift and keyLock combine into stretcher-engaged");

        Session s;
        s.capture->captureInitialValues (0); // keyLock off, keyShift 0 -> pitchStretch off
        s.armed = true;

        // keyShift 0 -> +1 : pitchStretch on (keyStepper on too).
        s.playhead = 1000; s.setKeyShift (0, 1);
        // keyLock false -> true : pitchStretch stays on (no new step).
        s.playhead = 2000; s.setKeyLock (0, true);
        // keyShift +1 -> 0 : pitchStretch STILL on because keyLock true (no step).
        s.playhead = 3000; s.setKeyShift (0, 0);
        // keyLock true -> false : pitchStretch off.
        s.playhead = 4000; s.setKeyLock (0, false);

        expectSteps (stepsOf (s.lane ("A", "pitchStretch")),
                     { { 0, false }, { 1000, true }, { 4000, false } },
                     "A.pitchStretch derived sequence");

        // Sanity: keyLock lane has its own on/off; keyStepper its own on/off.
        expectSteps (stepsOf (s.lane ("A", "keyLock")),
                     { { 0, false }, { 2000, true }, { 4000, false } },
                     "A.keyLock sequence");
        expectSteps (stepsOf (s.lane ("A", "keyStepper")),
                     { { 0, false }, { 1000, true }, { 3000, false } },
                     "A.keyStepper sequence");
    }

    //==========================================================================
    void debounceCoalescesZeroDurationPair()
    {
        beginTest ("Debounce: on then off within the window coalesces to nothing (lane returns to pre-pair state)");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;
        s.capture->setDebounceSamples (220);

        // Double-click: on at 10000, off at 10100 (100 samples < 220 window).
        s.playhead = 10000; s.setKeyLock (0, true);
        s.playhead = 10100; s.setKeyLock (0, false);

        // The degenerate on/off pair is dropped — only the seed remains.
        expectSteps (stepsOf (s.lane ("A", "keyLock")),
                     { { 0, false } }, "A.keyLock coalesced to seed only");

        // pitchStretch (which also toggled on then off here) likewise coalesces.
        expectSteps (stepsOf (s.lane ("A", "pitchStretch")),
                     { { 0, false } }, "A.pitchStretch coalesced to seed only");
    }

    //==========================================================================
    void debounceOutsideWindowKeepsBoth()
    {
        beginTest ("Debounce: on then off OUTSIDE the window keeps both steps");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;
        s.capture->setDebounceSamples (220);

        s.playhead = 10000; s.setKeyLock (0, true);
        s.playhead = 10500; s.setKeyLock (0, false); // 500 > 220 window

        expectSteps (stepsOf (s.lane ("A", "keyLock")),
                     { { 0, false }, { 10000, true }, { 10500, false } },
                     "A.keyLock keeps both outside window");
    }

    //==========================================================================
    void perDeckIndependence()
    {
        beginTest ("A toggle on deck A does not touch B/C/D lanes");

        Session s;
        s.capture->captureInitialValues (0);
        s.armed = true;

        s.playhead = 5000; s.setKeyLock (0, true);
        s.playhead = 6000; s.setKeyShift (0, 4);

        // Deck A captured.
        expect (s.lane ("A", "keyLock").getNumSteps() > 1);
        expect (s.lane ("A", "keyStepper").getNumSteps() > 1);

        // Decks B, C, D each only have their single seed in every lane.
        for (auto* o : { "B", "C", "D" })
            for (auto* p : { "keyLock", "pitchStretch", "keyStepper" })
                expectEquals (s.lane (o, p).getNumSteps(), 1,
                              juce::String ("untouched ") + o + "." + p);
    }
};

static ChannelBooleanLaneCaptureTests channelBooleanLaneCaptureTests;
