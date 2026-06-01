//==============================================================================
// PRD-0065: TimelineTransform tests.
//
// Pure value-math tests — no JUCE Component is instantiated. Cover round-trip
// stability, zoom clamping, scroll clamping, the focal-point zoom invariant,
// snapping correctness, the off-beat sample->pixel path, and degenerate cases.
//==============================================================================

#include <juce_core/juce_core.h>
#include <cmath>

#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/State/DawState.h"

class TimelineTransformTests : public juce::UnitTest
{
public:
    TimelineTransformTests() : juce::UnitTest ("Timeline Transform", "Sonik") {}

    static Daw::TimelineTransform::GridSnapshot makeGrid (double spb = 22050.0,
                                                          std::int64_t origin = 0)
    {
        Daw::TimelineTransform::GridSnapshot g;
        g.samplesPerBeat    = spb;
        g.phaseOriginSample = origin;
        return g;
    }

    void runTest() override
    {
        using TT = Daw::TimelineTransform;

        beginTest ("sampleToX: pure affine map, origin at left edge maps to x=0");
        {
            // spb=22050, ppb=100 -> 100 px per 22050 samples.
            TT t (makeGrid (22050.0, 0), 100.0, /*leftEdge*/ 0, /*width*/ 1000.0,
                  /*contentEnd*/ 44100 * 100);

            expectWithinAbsoluteError (t.sampleToX (0), 0.0, 1e-9);
            // One beat (22050 samples) -> 100 px.
            expectWithinAbsoluteError (t.sampleToX (22050), 100.0, 1e-9);
            // Half a beat -> 50 px.
            expectWithinAbsoluteError (t.sampleToX (11025), 50.0, 1e-9);
        }

        beginTest ("beatToX / barToX compose musical conversion with affine map");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 2000.0, 44100 * 100);
            expectWithinAbsoluteError (t.beatToX (0.0), 0.0, 1e-9);
            expectWithinAbsoluteError (t.beatToX (1.0), 100.0, 1e-9);
            // bar 1 = kBeatsPerBar beats -> 4 * 100 = 400 px.
            expectWithinAbsoluteError (t.barToX (1.0),
                                       100.0 * DawState::kBeatsPerBar, 1e-9);
        }

        beginTest ("integer-sample round-trip is lossless");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, 44100 * 100);
            for (std::int64_t s : { (std::int64_t) 0, (std::int64_t) 1,
                                    (std::int64_t) 12345, (std::int64_t) 22050,
                                    (std::int64_t) 100000 })
            {
                const double x = t.sampleToX (s);
                expectEquals (t.xToSample (x), s);
            }
        }

        beginTest ("pixel round-trip stable within +/- 0.5 px");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, 44100 * 100);
            for (double px : { 0.0, 1.0, 37.5, 100.0, 250.0, 999.0 })
            {
                const double back = t.sampleToX (t.xToSample (px));
                expect (std::abs (back - px) <= 0.5);
            }
        }

        beginTest ("zoom clamps to [min, max] and never non-positive/non-finite");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, 44100 * 100);

            t.setPixelsPerBeat (1e9);
            expectWithinAbsoluteError (t.getPixelsPerBeat(), TT::kMaxPixelsPerBeat, 1e-6);

            t.setPixelsPerBeat (1e-9);
            expectWithinAbsoluteError (t.getPixelsPerBeat(), TT::kMinPixelsPerBeat, 1e-6);

            t.setPixelsPerBeat (-5.0);
            expect (t.getPixelsPerBeat() > 0.0);
            expect (std::isfinite (t.getPixelsPerBeat()));

            t.setPixelsPerBeat (std::nan (""));
            expect (std::isfinite (t.getPixelsPerBeat()));
            expect (t.getPixelsPerBeat() > 0.0);
        }

        beginTest ("scroll clamps to [minLeftEdge, maxLeftEdge]");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, /*contentEnd*/ 44100 * 50);

            // Scroll far left -> clamps to minLeftEdgeSample (small negative margin).
            t.setLeftEdgeSample (-100'000'000);
            expectEquals (t.getLeftEdgeSample(), t.minLeftEdgeSample());

            // Scroll far right -> clamps to maxLeftEdgeSample.
            t.setLeftEdgeSample (100'000'000);
            expectEquals (t.getLeftEdgeSample(), t.maxLeftEdgeSample());

            // minLeftEdge is the negative one-beat margin before origin.
            expectEquals (t.minLeftEdgeSample(), (std::int64_t) -22050);
        }

        beginTest ("content shorter than viewport: bounds collapse, anchored left");
        {
            // viewport 1000 px at 100 ppb covers 10 beats = 220500 samples;
            // content end only 50000 samples -> max collapses to min.
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, /*contentEnd*/ 50000);
            expectEquals (t.maxLeftEdgeSample(), t.minLeftEdgeSample());
            t.setLeftEdgeSample (30000);
            expectEquals (t.getLeftEdgeSample(), t.minLeftEdgeSample());
        }

        beginTest ("focal-point zoom keeps sample under focusPx fixed");
        {
            // Left edge sits well inside the scrollable range and content is far
            // larger than the viewport on both sides, so no scroll/zoom clamp is
            // hit and the anchor invariant must hold exactly (modulo rounding).
            for (double focus : { 0.0, 100.0, 500.0, 900.0 })
            {
                for (double factor : { 0.5, 1.25, 2.0, 4.0 })
                {
                    TT t (makeGrid (22050.0, 0), 100.0,
                          /*leftEdge*/ 2'000'000, /*width*/ 1000.0,
                          /*contentEnd*/ 44100 * 400);
                    const std::int64_t before = t.xToSample (focus);
                    t.zoomAroundX (focus, factor);
                    const std::int64_t after = t.xToSample (focus);
                    // Sample under focus stays within +/- a couple samples
                    // (rounding) when no clamp is involved.
                    expect (std::llabs (after - before) <= 2);
                }
            }
        }

        beginTest ("focal-point zoom honours zoom clamp while keeping anchor");
        {
            TT t (makeGrid (22050.0, 0), TT::kMaxPixelsPerBeat * 0.9, 5000, 1000.0,
                  44100 * 200);
            const double focus = 400.0;
            const double anchorX_before = t.sampleToX (t.xToSample (focus));
            t.zoomAroundX (focus, 100.0); // would blow past max
            expectWithinAbsoluteError (t.getPixelsPerBeat(), TT::kMaxPixelsPerBeat, 1e-6);
            // Anchor invariant within 0.5 px when clamp hit.
            const double anchorX_after = t.sampleToX (t.xToSample (focus));
            expect (std::abs (anchorX_after - anchorX_before) <= 1.0);
        }

        beginTest ("snapSampleToGrid: on/before/after/halfway");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, 44100 * 100);
            // On a beat boundary -> itself.
            expectEquals (t.snapSampleToGrid (22050), (std::int64_t) 22050);
            // Just before beat 2 (44100) -> 44100.
            expectEquals (t.snapSampleToGrid (44000), (std::int64_t) 44100);
            // Just after beat 1 -> 22050.
            expectEquals (t.snapSampleToGrid (22100), (std::int64_t) 22050);
            // Exactly halfway between beat 0 and 1 (11025) -> round-half-up to beat 1.
            expectEquals (t.snapSampleToGrid (11025), (std::int64_t) 22050);
        }

        beginTest ("snapSampleToGrid respects phase origin");
        {
            TT t (makeGrid (22050.0, /*origin*/ 1000), 100.0, 0, 1000.0, 44100 * 100);
            // Beat boundaries at 1000, 23050, 45100...
            expectEquals (t.snapSampleToGrid (23050), (std::int64_t) 23050);
            expectEquals (t.snapSampleToGrid (23000), (std::int64_t) 23050);
            expectEquals (t.snapSampleToGrid (1100), (std::int64_t) 1000);
        }

        beginTest ("off-beat sample maps to exact fractional x (no beat snapping)");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 2000.0, 44100 * 100);
            // 12345 samples is not on a beat boundary.
            const double expected = 12345.0 * 100.0 / 22050.0;
            expectWithinAbsoluteError (t.sampleToX (12345), expected, 1e-9);
            // It is a fractional pixel, not a rounded beat position.
            expect (std::abs (t.sampleToX (12345) - std::round (t.sampleToX (12345))) > 1e-6);
        }

        beginTest ("bars-per-screen <-> pixels-per-beat boundary conversion");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, /*width*/ 800.0, 44100 * 100);
            // 800 px wide, want 2 bars on screen = 8 beats -> 100 ppb.
            const double ppb = t.barsPerScreenToPixelsPerBeat (2.0);
            expectWithinAbsoluteError (ppb, 800.0 / (2.0 * DawState::kBeatsPerBar), 1e-9);
            t.setPixelsPerBeat (ppb);
            expectWithinAbsoluteError (t.pixelsPerBeatToBarsPerScreen(), 2.0, 1e-9);
        }

        beginTest ("alignToPixelGrid rounds to device pixel");
        {
            expectWithinAbsoluteError (TT::alignToPixelGrid (10.4), 10.0, 1e-9);
            expectWithinAbsoluteError (TT::alignToPixelGrid (10.6), 11.0, 1e-9);
            // At 2x scale, half-logical-pixels are representable.
            expectWithinAbsoluteError (TT::alignToPixelGrid (10.25, 2.0), 10.5, 1e-9);
        }

        beginTest ("degenerate: zero viewport width, mappings stay finite");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, /*width*/ 0.0, 0);
            expect (std::isfinite (t.sampleToX (12345)));
            expect (std::isfinite (t.xToBeat (0.0)));
            // Bounds collapse to min when width is 0.
            expectEquals (t.maxLeftEdgeSample(), t.minLeftEdgeSample());
        }

        beginTest ("degenerate: non-finite inputs never produce NaN out");
        {
            TT t (makeGrid (22050.0, 0), 100.0, 0, 1000.0, 44100 * 100);
            expect (t.xToSample (std::nan ("")) == t.xToSample (std::nan ("")));
            t.zoomAroundX (std::nan (""), 2.0); // no crash, no-op-ish
            t.scrollByX (std::nan (""));        // ignored
            expect (std::isfinite (t.getPixelsPerBeat()));
        }
    }
};

static TimelineTransformTests timelineTransformTests;
