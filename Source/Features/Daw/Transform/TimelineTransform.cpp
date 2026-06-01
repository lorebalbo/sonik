//==============================================================================
// PRD-0065: Timeline Coordinate Transform implementation.
//==============================================================================

#include "TimelineTransform.h"

#include <algorithm>
#include <cmath>

namespace Daw
{

//==============================================================================
// Construction
//==============================================================================

TimelineTransform::TimelineTransform (GridSnapshot  grid,
                                      double        pixelsPerBeat,
                                      std::int64_t  leftEdgeSample,
                                      double        viewportWidthPx,
                                      std::int64_t  contentEndSample)
    : grid_             (grid),
      pixelsPerBeat_    (clampZoom (pixelsPerBeat)),
      viewportWidthPx_  (viewportWidthPx > 0.0 ? viewportWidthPx : 0.0),
      contentEndSample_ (contentEndSample > 0 ? contentEndSample : 0)
{
    if (grid_.samplesPerBeat <= 0.0 || ! std::isfinite (grid_.samplesPerBeat))
        grid_.samplesPerBeat = DawState::kProjectSampleRate * 60.0 / 120.0;

    leftEdgeSample_ = clampLeftEdge (leftEdgeSample);
}

//==============================================================================
// Internal helpers
//==============================================================================

double TimelineTransform::clampZoom (double pixelsPerBeat)
{
    if (! std::isfinite (pixelsPerBeat) || pixelsPerBeat <= 0.0)
        return kDefaultPixelsPerBeat;
    return juce::jlimit (kMinPixelsPerBeat, kMaxPixelsPerBeat, pixelsPerBeat);
}

double TimelineTransform::samplesPerPixel() const
{
    // samplesPerBeat / pixelsPerBeat; both guaranteed positive.
    return grid_.samplesPerBeat / pixelsPerBeat_;
}

std::int64_t TimelineTransform::minLeftEdgeSample() const
{
    // A small negative margin (kLeftMarginBeats before bar 0), measured from
    // the phase origin so bar 0 is not jammed against the left edge.
    const std::int64_t marginSamples =
        static_cast<std::int64_t> (std::llround (kLeftMarginBeats * grid_.samplesPerBeat));
    return grid_.phaseOriginSample - marginSamples;
}

std::int64_t TimelineTransform::maxLeftEdgeSample() const
{
    const std::int64_t lo = minLeftEdgeSample();

    if (viewportWidthPx_ <= 0.0)
        return lo;

    const std::int64_t viewportSamples =
        static_cast<std::int64_t> (std::llround (viewportWidthPx_ * samplesPerPixel()));

    // Keep the last content visible with a little room past it: allow scrolling
    // until content end sits a fraction (half) of the viewport from the left.
    const std::int64_t hi = contentEndSample_ - viewportSamples / 2;

    // When content is shorter than the viewport, collapse to lo (anchor left).
    return std::max (lo, hi);
}

std::int64_t TimelineTransform::clampLeftEdge (std::int64_t candidate) const
{
    const std::int64_t lo = minLeftEdgeSample();
    const std::int64_t hi = maxLeftEdgeSample();
    if (hi < lo)
        return lo;
    return std::clamp (candidate, lo, hi);
}

//==============================================================================
// View-state setters
//==============================================================================

void TimelineTransform::setPixelsPerBeat (double newPixelsPerBeat)
{
    pixelsPerBeat_  = clampZoom (newPixelsPerBeat);
    leftEdgeSample_ = clampLeftEdge (leftEdgeSample_);
}

void TimelineTransform::setLeftEdgeSample (std::int64_t newLeftEdgeSample)
{
    leftEdgeSample_ = clampLeftEdge (newLeftEdgeSample);
}

void TimelineTransform::setViewportWidth (double newWidthPx)
{
    viewportWidthPx_ = newWidthPx > 0.0 ? newWidthPx : 0.0;
    leftEdgeSample_  = clampLeftEdge (leftEdgeSample_);
}

void TimelineTransform::setContentEndSample (std::int64_t newContentEndSample)
{
    contentEndSample_ = newContentEndSample > 0 ? newContentEndSample : 0;
    leftEdgeSample_   = clampLeftEdge (leftEdgeSample_);
}

double TimelineTransform::barsPerScreenToPixelsPerBeat (double barsPerScreen) const
{
    if (barsPerScreen <= 0.0 || viewportWidthPx_ <= 0.0)
        return pixelsPerBeat_;
    return viewportWidthPx_ / (barsPerScreen * static_cast<double> (DawState::kBeatsPerBar));
}

double TimelineTransform::pixelsPerBeatToBarsPerScreen() const
{
    if (viewportWidthPx_ <= 0.0)
        return 0.0;
    const double beatsOnScreen = viewportWidthPx_ / pixelsPerBeat_;
    return beatsOnScreen / static_cast<double> (DawState::kBeatsPerBar);
}

//==============================================================================
// Forward mappings
//==============================================================================

double TimelineTransform::sampleToX (std::int64_t sample) const
{
    // Pure affine map of the sample (§1.5.7); never routes through a beat index.
    return static_cast<double> (sample - leftEdgeSample_) * pixelsPerBeat_ / grid_.samplesPerBeat;
}

double TimelineTransform::beatToX (double beat) const
{
    return sampleToX (grid_.beatToSample (beat));
}

double TimelineTransform::barToX (double bar) const
{
    return beatToX (bar * static_cast<double> (DawState::kBeatsPerBar));
}

//==============================================================================
// Inverse mappings
//==============================================================================

std::int64_t TimelineTransform::xToSample (double px) const
{
    if (! std::isfinite (px))
        px = 0.0;

    // sample = leftEdge + round(px * samplesPerBeat / pixelsPerBeat).
    const double sampleD =
        static_cast<double> (leftEdgeSample_) + px * grid_.samplesPerBeat / pixelsPerBeat_;

    // Round-half-up, once.
    const std::int64_t sample =
        static_cast<std::int64_t> (std::floor (sampleD + 0.5));

    // Clamp to scroll/content bounds.
    const std::int64_t lo = minLeftEdgeSample();
    const std::int64_t hi = std::max (lo, contentEndSample_);
    return std::clamp (sample, lo, hi);
}

double TimelineTransform::xToBeat (double px) const
{
    return grid_.sampleToBeat (xToSample (px));
}

//==============================================================================
// Focal-point zoom
//==============================================================================

void TimelineTransform::zoomAroundX (double focusPx, double zoomFactor)
{
    if (! std::isfinite (focusPx))
        focusPx = 0.0;
    if (! std::isfinite (zoomFactor) || zoomFactor <= 0.0)
        return;

    // Sample currently under focusPx (use the un-clamped affine inverse so the
    // anchor is exact, independent of the scroll clamp).
    const double anchorSampleD =
        static_cast<double> (leftEdgeSample_) + focusPx * grid_.samplesPerBeat / pixelsPerBeat_;

    // Apply and clamp the new zoom.
    pixelsPerBeat_ = clampZoom (pixelsPerBeat_ * zoomFactor);

    // Solve for the new leftEdge so the anchor stays under focusPx:
    //   focusPx = (anchor - newLeft) * pixelsPerBeat / samplesPerBeat
    //   newLeft = anchor - focusPx * samplesPerBeat / pixelsPerBeat
    const double newLeftD =
        anchorSampleD - focusPx * grid_.samplesPerBeat / pixelsPerBeat_;

    leftEdgeSample_ = clampLeftEdge (static_cast<std::int64_t> (std::llround (newLeftD)));
}

//==============================================================================
// Scroll
//==============================================================================

void TimelineTransform::scrollByX (double deltaPx)
{
    if (! std::isfinite (deltaPx))
        return;
    const std::int64_t deltaSamples =
        static_cast<std::int64_t> (std::llround (deltaPx * samplesPerPixel()));
    leftEdgeSample_ = clampLeftEdge (leftEdgeSample_ + deltaSamples);
}

//==============================================================================
// Snapping
//==============================================================================

std::int64_t TimelineTransform::snapSampleToGrid (std::int64_t sample) const
{
    const double beat        = grid_.sampleToBeat (sample);
    const double nearestBeat = std::floor (beat + 0.5); // round-half-up
    return grid_.beatToSample (nearestBeat);
}

//==============================================================================
// Pixel alignment helper
//==============================================================================

double TimelineTransform::alignToPixelGrid (double x, double displayScale)
{
    if (! std::isfinite (x))
        return 0.0;
    if (displayScale <= 0.0 || ! std::isfinite (displayScale))
        displayScale = 1.0;
    // Round to the nearest device pixel, then back to logical units.
    return std::floor (x * displayScale + 0.5) / displayScale;
}

} // namespace Daw
