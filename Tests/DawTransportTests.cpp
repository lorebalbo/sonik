//==============================================================================
// PRD-0082: DawTransportTests — unit tests for DawTransport.
//
// Tests:
//   - Initial state is Stopped
//   - play() transitions to Playing
//   - pause() transitions to Paused
//   - stop() transitions to Stopped and resets playhead
//   - advancePlayhead increments position when Playing
//   - advancePlayhead is a no-op when Stopped/Paused
//   - seek() updates position
//   - Loop region wrap works correctly
//   - onStateChanged callback fires
//
// JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>

#include "Features/Daw/Playback/DawTransport.h"

class DawTransportTests final : public juce::UnitTest
{
public:
    DawTransportTests() : juce::UnitTest ("DAW Transport (PRD-0082)", "Sonik") {}

    void runTest() override
    {
        testInitialState();
        testPlayTransition();
        testPauseTransition();
        testStopTransition();
        testAdvancePlayhead();
        testAdvancePlayheadNoOpWhenNotPlaying();
        testSeek();
        testLoopWrap();
        testStateChangedCallback();
    }

private:
    // ─── Initial state ─────────────────────────────────────────────────────

    void testInitialState()
    {
        beginTest ("Initial state is Stopped");

        Daw::DawTransport t;
        expect (t.isStopped(), "should be stopped");
        expect (!t.isPlaying(), "should not be playing");
        expect (!t.isPaused(), "should not be paused");
        expectEquals (t.getPlayheadSample(), (int64_t) -1, "stopped playhead = -1");
    }

    // ─── play() ─────────────────────────────────────────────────────────────

    void testPlayTransition()
    {
        beginTest ("play() transitions to Playing");

        Daw::DawTransport t;
        t.play();
        expect (t.isPlaying());
        expect (!t.isStopped());
    }

    // ─── pause() ────────────────────────────────────────────────────────────

    void testPauseTransition()
    {
        beginTest ("pause() transitions to Paused");

        Daw::DawTransport t;
        t.play();
        t.advancePlayhead (100);
        const int64_t pos = t.getPlayheadSample();

        t.pause();
        expect (t.isPaused());
        expectEquals (t.getPlayheadSample(), pos, "position retained on pause");
    }

    // ─── stop() ─────────────────────────────────────────────────────────────

    void testStopTransition()
    {
        beginTest ("stop() transitions to Stopped and resets playhead");

        Daw::DawTransport t;
        t.play();
        t.advancePlayhead (1000);
        t.stop();

        expect (t.isStopped());
        expectEquals (t.getPlayheadSample(), (int64_t) -1, "playhead reset to -1 on stop");
    }

    // ─── advancePlayhead ─────────────────────────────────────────────────────

    void testAdvancePlayhead()
    {
        beginTest ("advancePlayhead increments position while Playing");

        Daw::DawTransport t;
        t.play();

        const int64_t p0 = t.getPlayheadSample();
        t.advancePlayhead (512);
        const int64_t p1 = t.getPlayheadSample();
        expectEquals (p1 - p0, (int64_t) 512);

        t.advancePlayhead (512);
        const int64_t p2 = t.getPlayheadSample();
        expectEquals (p2 - p0, (int64_t) 1024);
    }

    // ─── advancePlayhead no-op when not Playing ───────────────────────────

    void testAdvancePlayheadNoOpWhenNotPlaying()
    {
        beginTest ("advancePlayhead is no-op when Stopped or Paused");

        // Stopped.
        {
            Daw::DawTransport t;
            t.advancePlayhead (512);
            expectEquals (t.getPlayheadSample(), (int64_t) -1, "stopped");
        }

        // Paused.
        {
            Daw::DawTransport t;
            t.play();
            t.advancePlayhead (100);
            const int64_t pos = t.getPlayheadSample();
            t.pause();
            t.advancePlayhead (512);
            expectEquals (t.getPlayheadSample(), pos, "paused");
        }
    }

    // ─── seek() ──────────────────────────────────────────────────────────────

    void testSeek()
    {
        beginTest ("seek() sets playhead to requested position");

        Daw::DawTransport t;
        t.play();
        t.seek (10000);
        expectEquals (t.getPlayheadSample(), (int64_t) 10000);

        // Negative clamp.
        t.seek (-500);
        expectEquals (t.getPlayheadSample(), (int64_t) 0, "negative seek clamped to 0");
    }

    // ─── Loop wrap ────────────────────────────────────────────────────────────

    void testLoopWrap()
    {
        beginTest ("Loop region wraps playhead at loopEnd");

        Daw::DawTransport t;
        t.setLoopRegion (100, 200);
        t.setLoopEnabled (true);
        t.play();

        // Manually seek near loop end.
        t.seek (190);
        // Advance 20 samples → should wrap to 100.
        t.advancePlayhead (20);

        expectEquals (t.getPlayheadSample(), (int64_t) 100, "loop should wrap to start");
    }

    // ─── onStateChanged callback ──────────────────────────────────────────────

    void testStateChangedCallback()
    {
        beginTest ("onStateChanged callback fires on transitions");

        Daw::DawTransport t;
        int callCount = 0;
        Daw::DawTransport::State lastState = Daw::DawTransport::State::Stopped;

        t.onStateChanged = [&] (Daw::DawTransport::State s)
        {
            ++callCount;
            lastState = s;
        };

        t.play();
        expectEquals (callCount, 1);
        expect (lastState == Daw::DawTransport::State::Playing);

        t.pause();
        expectEquals (callCount, 2);
        expect (lastState == Daw::DawTransport::State::Paused);

        t.stop();
        expectEquals (callCount, 3);
        expect (lastState == Daw::DawTransport::State::Stopped);
    }
};

static DawTransportTests dawTransportTestsInstance;
