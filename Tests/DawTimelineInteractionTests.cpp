//==============================================================================
// PRD-0070: Live playhead / now-line + zoom-scroll-follow interaction tests.
//
// Exercises the pure, component-free pieces of PRD-0070 so the suite stays
// deterministic and graphics-context free:
//   * Playhead visibility / clamping helper.
//   * FollowController auto-scroll state machine.
//   * Now-line x derivation + focal-zoom invariance + scroll clamping via the
//     PRD-0065 TimelineTransform (the same maths DawPanel drives at runtime).
//==============================================================================

#include <juce_core/juce_core.h>

#include "Features/Daw/Ui/Atoms/Playhead.h"
#include "Features/Daw/Ui/FollowController.h"
#include "Features/Daw/Transform/TimelineTransform.h"

class DawTimelineInteractionTests : public juce::UnitTest
{
public:
    DawTimelineInteractionTests()
        : juce::UnitTest ("DAW Timeline Interaction (PRD-0070)", "Sonik") {}

    void runTest() override
    {
        //----------------------------------------------------------------------
        beginTest ("Playhead is visible only within the content width");
        {
            expect (! Daw::Playhead::isLineVisible (-1, 800));
            expect (  Daw::Playhead::isLineVisible (0, 800));
            expect (  Daw::Playhead::isLineVisible (799, 800));
            expect (! Daw::Playhead::isLineVisible (800, 800));
            expect (! Daw::Playhead::isLineVisible (5000, 800));
        }

        //----------------------------------------------------------------------
        beginTest ("FollowController defaults to OFF and never auto-scrolls");
        {
            Daw::FollowController fc;
            expect (! fc.isEnabled());
            // Even with the now-line well past the trigger fraction, disabled =>
            // no follow.
            expect (! fc.shouldFollow (/*nowLineX*/ 999.0, /*width*/ 1000.0));
        }

        //----------------------------------------------------------------------
        beginTest ("FollowController engages / disengages explicitly");
        {
            Daw::FollowController fc;
            fc.setEnabled (true);
            expect (fc.isEnabled());

            // Below the trigger fraction (0.85): no scroll yet.
            expect (! fc.shouldFollow (800.0, 1000.0));
            // Past the trigger fraction: scroll.
            expect (  fc.shouldFollow (900.0, 1000.0));

            // A manual scroll disengages until re-enabled.
            fc.notifyManualScroll();
            expect (! fc.isEnabled());
            expect (! fc.shouldFollow (900.0, 1000.0));

            fc.toggle();
            expect (fc.isEnabled());
        }

        //----------------------------------------------------------------------
        beginTest ("FollowController re-anchors to the re-anchor fraction");
        {
            expectWithinAbsoluteError (Daw::FollowController::reanchorTargetX (1000.0),
                                       500.0, 1.0e-9);
        }

        //----------------------------------------------------------------------
        beginTest ("Now-line x is the affine sample->pixel mapping");
        {
            // 120 BPM => 22050 samples/beat; phase origin 0; 100 px/beat.
            Daw::TimelineTransform::GridSnapshot grid; // defaults to 22050 spb
            Daw::TimelineTransform t (grid, /*ppb*/ 100.0, /*leftEdge*/ 0,
                                      /*viewportWidth*/ 1000.0,
                                      /*contentEnd*/ 44'100'000);

            // Sample at the left edge maps to x = 0.
            expectWithinAbsoluteError (t.sampleToX (0), 0.0, 1.0e-6);
            // One beat in => one ppb to the right.
            expectWithinAbsoluteError (t.sampleToX (22050), 100.0, 1.0e-6);
            // Four beats (one bar) => 400 px.
            expectWithinAbsoluteError (t.sampleToX (88200), 400.0, 1.0e-6);
        }

        //----------------------------------------------------------------------
        beginTest ("Focal zoom keeps the sample under the cursor fixed");
        {
            Daw::TimelineTransform::GridSnapshot grid;
            Daw::TimelineTransform t (grid, 100.0, 0, 1000.0, /*contentEnd*/ 44'100'000);

            const double focusPx = 600.0;
            const auto   anchored = t.xToSample (focusPx);

            t.zoomAroundX (focusPx, 2.0);   // zoom in 2x around the cursor

            // The sample that was under the cursor is still under the cursor.
            const double afterX = t.sampleToX (anchored);
            expectWithinAbsoluteError (afterX, focusPx, 1.0); // within 1 px

            // Zoom actually changed the scale.
            expectGreaterThan (t.getPixelsPerBeat(), 100.0);
        }

        //----------------------------------------------------------------------
        beginTest ("Scroll clamps at the left margin");
        {
            Daw::TimelineTransform::GridSnapshot grid;
            Daw::TimelineTransform t (grid, 100.0, 0, 1000.0, /*contentEnd*/ 44'100'000);

            // Scroll far left: clamps to the minimum left edge, never beyond.
            t.scrollByX (-100000.0);
            expect (t.getLeftEdgeSample() >= t.minLeftEdgeSample());
            expectEquals ((int) t.getLeftEdgeSample(), (int) t.minLeftEdgeSample());
        }

        //----------------------------------------------------------------------
        beginTest ("Zoom is bounded by the documented min / max");
        {
            Daw::TimelineTransform::GridSnapshot grid;
            Daw::TimelineTransform t (grid, 100.0, 0, 1000.0, 44'100'000);

            for (int i = 0; i < 50; ++i)
                t.zoomAroundX (500.0, 2.0);
            expect (t.getPixelsPerBeat() <= Daw::TimelineTransform::kMaxPixelsPerBeat);

            for (int i = 0; i < 50; ++i)
                t.zoomAroundX (500.0, 0.5);
            expect (t.getPixelsPerBeat() >= Daw::TimelineTransform::kMinPixelsPerBeat);
        }
    }
};

static DawTimelineInteractionTests dawTimelineInteractionTests;
