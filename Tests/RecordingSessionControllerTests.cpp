#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Recording/RecordingSessionController.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Sync/MasterClockManager.h"
#include "Features/Sync/MasterClockPublisher.h"

namespace
{

//==============================================================================
// Fully scripted clock so tests control mode, master position, and wall time.
struct FakeRecordingClock final : public Daw::RecordingClock
{
    bool         running    = false;
    std::int64_t master     = 0;
    double       seconds    = 0.0;
    double       sampleRate = DawState::kProjectSampleRate;

    bool         isMasterRunning() const override      { return running; }
    std::int64_t masterTimelineSample() const override { return master; }
    double       wallClockSeconds() const override     { return seconds; }
    double       projectSampleRate() const override    { return sampleRate; }
};

//==============================================================================
// Minimal real dependencies the controller ctor requires (unused when a fake
// clock override is supplied).
struct Harness
{
    juce::ValueTree            root { "SonikState" };
    MasterClockPublisher       publisher;
    MasterClockManager         clockManager { root, publisher };
    Daw::MasterGridService     grid { publisher, [] { return DawState::kProjectSampleRate; } };
    juce::ValueTree            daw { DawIDs::Daw };
    FakeRecordingClock         clock;

    std::unique_ptr<Daw::RecordingSessionController> makeController()
    {
        return std::make_unique<Daw::RecordingSessionController> (daw, grid, clockManager, &clock);
    }
};

// Listener that counts property changes on the recording subtree.
struct RecordingTreeListener final : public juce::ValueTree::Listener
{
    int changeCount = 0;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++changeCount; }
};

} // namespace

//==============================================================================
class RecordingSessionControllerTests : public juce::UnitTest
{
public:
    RecordingSessionControllerTests()
        : juce::UnitTest ("Recording Session Controller", "Sonik") {}

    void runTest() override
    {
        using namespace Daw;

        beginTest ("Starts in Stopped with playhead 0");
        {
            Harness h;
            auto c = h.makeController();
            expect (c->state() == RecordingState::Stopped);
            expectEquals (c->currentTimelinePosition(), (std::int64_t) 0);
            expectEquals (c->timelineOrigin(), (std::int64_t) 0);
        }

        beginTest ("arm(): Stopped -> Armed, playhead reset to 0");
        {
            Harness h;
            auto c = h.makeController();
            c->arm();
            expect (c->state() == RecordingState::Armed);
            expectEquals (c->currentTimelinePosition(), (std::int64_t) 0);
        }

        beginTest ("arm() is a no-op when not Stopped");
        {
            Harness h;
            auto c = h.makeController();
            c->arm();
            c->beginCapture();              // -> Recording
            const auto before = c->state();
            c->arm();                       // must not corrupt state
            expect (before == RecordingState::Recording);
            expect (c->state() == RecordingState::Recording);
        }

        beginTest ("beginCapture(): Armed -> Recording, stamps timelineOrigin");
        {
            Harness h;
            auto c = h.makeController();

            // Dormant pre-roll: arm at t=0, then 30s of silence before audio.
            h.clock.running = false;
            h.clock.seconds = 0.0;
            c->arm();

            h.clock.seconds = 30.0;
            c->tick();

            const auto playheadAt30 = c->currentTimelinePosition();
            c->beginCapture();
            expect (c->state() == RecordingState::Recording);
            // First audio lands at its true offset (~30s), not at 0.
            expectEquals (c->timelineOrigin(), playheadAt30);
            expect (c->timelineOrigin() > (std::int64_t) 0);
        }

        beginTest ("beginCapture() is a no-op from Stopped");
        {
            Harness h;
            auto c = h.makeController();
            c->beginCapture();
            expect (c->state() == RecordingState::Stopped);
        }

        beginTest ("Dormant clock advances at the project sample rate");
        {
            Harness h;
            auto c = h.makeController();

            h.clock.running = false;
            h.clock.seconds = 0.0;
            c->arm();

            h.clock.seconds = 30.0; // 30 seconds of dormant silence
            c->tick();

            const auto expected = (std::int64_t) std::llround (30.0 * DawState::kProjectSampleRate);
            const auto tolerance = (std::int64_t) (DawState::kProjectSampleRate / 60.0); // one 60Hz tick
            expect (std::llabs (c->currentTimelinePosition() - expected) <= tolerance);
        }

        beginTest ("Running master clock anchors the playhead to musical time");
        {
            Harness h;
            auto c = h.makeController();

            h.clock.running = true;
            h.clock.master  = 0;
            c->arm();

            // Advance the master timeline by a known sample delta (e.g. 4 beats
            // @120 BPM / 44.1k = 88200 samples) and tick.
            h.clock.master = 88200;
            c->tick();
            expectEquals (c->currentTimelinePosition(), (std::int64_t) 88200);

            // A second anchored advance accumulates.
            h.clock.master = 132300; // +44100
            c->tick();
            expectEquals (c->currentTimelinePosition(), (std::int64_t) 132300);
        }

        beginTest ("Playhead is monotonic across a dormant->running transition");
        {
            Harness h;
            auto c = h.makeController();

            h.clock.running = false;
            h.clock.seconds = 0.0;
            c->arm();

            h.clock.seconds = 10.0;
            c->tick();
            const auto afterDormant = c->currentTimelinePosition();
            expect (afterDormant > 0);

            // Switch to running: the mode-change tick re-baselines (no jump back).
            h.clock.running = true;
            h.clock.master  = 500000;
            c->tick(); // mode switch, no advance
            expectEquals (c->currentTimelinePosition(), afterDormant);

            h.clock.master = 522050; // +22050
            c->tick();
            expectEquals (c->currentTimelinePosition(), afterDormant + 22050);
        }

        beginTest ("stop(): Recording -> Stopped raises exactly one stop signal");
        {
            Harness h;
            auto c = h.makeController();
            c->arm();
            c->beginCapture();
            const auto before = c->stopSignalCount();
            c->stop();
            expect (c->state() == RecordingState::Stopped);
            expectEquals ((int) (c->stopSignalCount() - before), 1);
        }

        beginTest ("stop(): Armed -> Stopped raises no stop signal");
        {
            Harness h;
            auto c = h.makeController();
            c->arm();
            const auto before = c->stopSignalCount();
            c->stop();
            expect (c->state() == RecordingState::Stopped);
            expectEquals ((int) (c->stopSignalCount() - before), 0);
        }

        beginTest ("stop() is a no-op from Stopped");
        {
            Harness h;
            auto c = h.makeController();
            const auto before = c->stopSignalCount();
            c->stop();
            expect (c->state() == RecordingState::Stopped);
            expectEquals ((int) (c->stopSignalCount() - before), 0);
        }

        beginTest ("Re-arm starts a fresh take from 0");
        {
            Harness h;
            auto c = h.makeController();

            h.clock.running = false;
            h.clock.seconds = 0.0;
            c->arm();
            h.clock.seconds = 5.0;
            c->tick();
            c->beginCapture();
            c->stop();
            expect (c->currentTimelinePosition() > 0);

            h.clock.seconds = 50.0;
            c->arm(); // fresh take
            expect (c->state() == RecordingState::Armed);
            expectEquals (c->currentTimelinePosition(), (std::int64_t) 0);
            expectEquals (c->timelineOrigin(), (std::int64_t) 0);
        }

        beginTest ("State and playhead are mirrored into the daw ValueTree");
        {
            Harness h;
            auto c = h.makeController();

            auto recordingNode = h.daw.getChildWithName (RecordingIDs::recording);
            expect (recordingNode.isValid());
            expectEquals (recordingNode[RecordingIDs::state].toString(),
                          juce::String (RecordingIDs::kStateStopped));

            c->arm();
            recordingNode = h.daw.getChildWithName (RecordingIDs::recording);
            expectEquals (recordingNode[RecordingIDs::state].toString(),
                          juce::String (RecordingIDs::kStateArmed));

            h.clock.running = false;
            h.clock.seconds = 2.0;
            c->tick();
            const auto mirrored = (std::int64_t) recordingNode[RecordingIDs::playheadSample];
            expectEquals (mirrored, c->currentTimelinePosition());

            c->beginCapture();
            recordingNode = h.daw.getChildWithName (RecordingIDs::recording);
            expectEquals (recordingNode[RecordingIDs::state].toString(),
                          juce::String (RecordingIDs::kStateRecording));
            expectEquals ((std::int64_t) recordingNode[RecordingIDs::timelineOrigin],
                          c->timelineOrigin());
        }

        beginTest ("A ValueTree listener observes recording changes without polling");
        {
            Harness h;
            auto c = h.makeController();

            RecordingTreeListener listener;
            h.daw.addListener (&listener);

            c->arm();         // state + playhead writes
            h.clock.seconds = 1.0;
            c->tick();        // playhead write

            expect (listener.changeCount > 0);
            h.daw.removeListener (&listener);
        }
    }
};

static RecordingSessionControllerTests recordingSessionControllerTests;
