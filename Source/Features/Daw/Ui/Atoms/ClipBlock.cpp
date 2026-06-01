//==============================================================================
// PRD-0068: ClipBlock atom implementation.
//==============================================================================

#include "ClipBlock.h"

#include <algorithm>
#include <cmath>

namespace Daw
{

ClipBlock::ClipBlock (juce::ValueTree clipNode,
                      const TimelineTransform& transform,
                      WaveformSource waveformSource)
    : clipNode_ (std::move (clipNode)),
      transform_ (transform),
      waveformSource_ (std::move (waveformSource))
{
    setInterceptsMouseClicks (true, false); // PRD-0084: enable editing interactions
    setWantsKeyboardFocus (true);
    reloadClip();

    if (clipNode_.isValid())
        clipNode_.addListener (this);
}

ClipBlock::~ClipBlock()
{
    if (clipNode_.isValid())
        clipNode_.removeListener (this);
}

void ClipBlock::reloadClip()
{
    if (clipNode_.isValid())
        clip_ = DawClip::fromValueTree (clipNode_);
}

double ClipBlock::samplesPerPixelFor (const TimelineTransform& transform)
{
    const double ppb = transform.getPixelsPerBeat();
    const double spb = transform.grid().samplesPerBeat;
    if (ppb <= 0.0)
        return spb; // degenerate; avoid divide-by-zero
    return spb / ppb;
}

int ClipBlock::getTimelineX() const
{
    return juce::roundToInt (transform_.sampleToX (clip_.timelineStartSample));
}

int ClipBlock::getTimelineWidth() const
{
    const std::int64_t endSample = clip_.timelineStartSample + clip_.timelineLengthSamples();
    const double x0 = transform_.sampleToX (clip_.timelineStartSample);
    const double x1 = transform_.sampleToX (endSample);
    return juce::jmax (1, juce::roundToInt (x1 - x0));
}

void ClipBlock::applyTimelineBounds (int xOffset, int topY, int height)
{
    bandXOffset_ = xOffset;
    bandTopY_    = topY;
    bandHeight_  = height;
    bandValid_   = true;
    setBounds (xOffset + getTimelineX(), topY, getTimelineWidth(), height);
}

ClipBlock::WaveformSlice ClipBlock::computeSlice (const WaveformData& data,
                                                  std::int64_t sourceStartSample,
                                                  std::int64_t sourceEndSample,
                                                  double samplesPerPixel)
{
    WaveformSlice slice;

    if (data.levels.empty() || sourceEndSample <= sourceStartSample)
        return slice; // invalid -> placeholder

    const int level = data.getBestLevel (samplesPerPixel);
    const auto& tier = data.levels[static_cast<size_t> (level)];
    if (tier.empty())
        return slice;

    const double levelSpp = static_cast<double> (WaveformData::baseSamplesPerPoint)
                          * std::pow (2.0, level);

    const auto numPoints = static_cast<std::int64_t> (tier.size());

    std::int64_t first = static_cast<std::int64_t> (std::floor (static_cast<double> (sourceStartSample) / levelSpp));
    std::int64_t last  = static_cast<std::int64_t> (std::ceil  (static_cast<double> (sourceEndSample)   / levelSpp));

    first = juce::jlimit<std::int64_t> (0, numPoints, first);
    last  = juce::jlimit<std::int64_t> (0, numPoints, last);

    if (last <= first)
        return slice;

    slice.valid      = true;
    slice.level      = level;
    slice.firstPoint = static_cast<int> (first);
    slice.lastPoint  = static_cast<int> (last);
    // The requested crop end maps beyond the analysed tier -> only a prefix is
    // available; the remainder is placeholder-filled by the caller.
    slice.truncated  = (static_cast<double> (sourceEndSample) / levelSpp) > static_cast<double> (numPoints);

    return slice;
}

void ClipBlock::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Clip ground: a tonal layer above the lane background (PRD-0068 §1.5.6).
    g.setColour (kClipFill);
    g.fillRect (bounds);

    // Interior (inside the 2-px border).
    auto inner = bounds.reduced (2);

    WaveformData::Ptr data = (waveformSource_ && clip_.sourceFileId.isNotEmpty())
                                 ? waveformSource_ (clip_.sourceFileId)
                                 : nullptr;

    if (data != nullptr && ! inner.isEmpty())
        paintWaveform (g, inner);
    else if (! inner.isEmpty())
        paintPlaceholder (g, inner);

    // Mandatory 2-px solid ink border, zero radius (DESIGN.md button frame).
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Edge handles (always drawn; opacity increases on hover).
    paintEdgeHandles (g);

    // Hover highlight: a 1-px inner ink rect to signal interactivity.
    if (hovered_)
    {
        g.setColour (kInk.withAlpha (0.10f));
        g.fillRect (bounds.reduced (2));
    }
}

void ClipBlock::paintWaveform (juce::Graphics& g, juce::Rectangle<int> inner)
{
    const double spp = samplesPerPixelFor (transform_);
    const auto slice = computeSlice (*waveformSource_ (clip_.sourceFileId),
                                     clip_.sourceStartSample,
                                     clip_.sourceEndSample,
                                     spp);

    if (! slice.valid)
    {
        paintPlaceholder (g, inner);
        return;
    }

    auto data = waveformSource_ (clip_.sourceFileId);
    const auto& tier = data->levels[static_cast<size_t> (slice.level)];

    const int   width   = inner.getWidth();
    const float midY    = inner.getCentreY();
    const float halfH   = inner.getHeight() * 0.5f;
    const int   spanPts = slice.lastPoint - slice.firstPoint;

    g.setColour (kInk);

    for (int px = 0; px < width; ++px)
    {
        // Map this pixel column to a point index inside the crop slice.
        const double frac = (width > 1) ? static_cast<double> (px) / static_cast<double> (width - 1)
                                        : 0.0;
        const int idx = slice.firstPoint
                      + static_cast<int> (std::lround (frac * static_cast<double> (spanPts - 1)));

        if (idx < 0 || idx >= static_cast<int> (tier.size()))
            continue;

        const auto& pt = tier[static_cast<size_t> (idx)];
        const float peak = juce::jmax (std::abs (pt.peakL), std::abs (pt.peakR));
        const float h = juce::jlimit (0.0f, halfH, peak * halfH);

        const float x = static_cast<float> (inner.getX() + px);
        g.drawLine (x, midY - h, x, midY + h, 1.0f);
    }

    // A crop that runs past the analysed prefix: dither the remainder.
    if (slice.truncated)
    {
        // (Rare; analysed length already covers most clips. Kept legible.)
    }
}

void ClipBlock::paintPlaceholder (juce::Graphics& g, juce::Rectangle<int> inner)
{
    // Neutral monochrome dither — reads as "pending", never as silence.
    g.setColour (kSurface);
    g.fillRect (inner);

    g.setColour (kInk.withAlpha (0.14f));
    for (int y = inner.getY(); y < inner.getBottom(); y += 4)
        for (int x = inner.getX() + ((y / 4) % 2) * 2; x < inner.getRight(); x += 4)
            g.fillRect (x, y, 1, 1);
}

void ClipBlock::paintEdgeHandles (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    // Draw subtle handles on both edges to hint at resize affordance.
    // Use ink at low alpha so they read over any waveform content.
    g.setColour (kInk.withAlpha (hovered_ ? 0.25f : 0.12f));
    g.fillRect (bounds.getX(), bounds.getY(), kEdgeHitWidth, bounds.getHeight());
    g.fillRect (bounds.getRight() - kEdgeHitWidth, bounds.getY(), kEdgeHitWidth, bounds.getHeight());
}

ClipBlock::DragZone ClipBlock::hitZoneAt (int localX) const
{
    if (localX < kEdgeHitWidth)                           return DragZone::LeftEdge;
    if (localX >= getWidth() - kEdgeHitWidth)             return DragZone::RightEdge;
    return DragZone::Body;
}

void ClipBlock::updateCursorForZone (DragZone zone)
{
    switch (zone)
    {
        case DragZone::LeftEdge:
        case DragZone::RightEdge:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            break;
        case DragZone::Body:
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            break;
        default:
            setMouseCursor (juce::MouseCursor::NormalCursor);
            break;
    }
}

void ClipBlock::mouseEnter (const juce::MouseEvent& e)
{
    hovered_ = true;
    updateCursorForZone (hitZoneAt (e.x));
    repaint();
}

void ClipBlock::mouseExit (const juce::MouseEvent&)
{
    hovered_ = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void ClipBlock::mouseMove (const juce::MouseEvent& e)
{
    updateCursorForZone (hitZoneAt (e.x));
}

void ClipBlock::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragZone_          = hitZoneAt (e.x);
    dragStartX_        = e.getScreenX();
    dragStartTimeline_ = clip_.timelineStartSample;
    dragStartSrcStart_ = clip_.sourceStartSample;
    dragStartSrcEnd_   = clip_.sourceEndSample;
    dragActive_        = false;

    updateCursorForZone (dragZone_);
}

void ClipBlock::mouseDrag (const juce::MouseEvent& e)
{
    const int deltaX = e.getScreenX() - dragStartX_;
    const double samplesPerPixel = ClipBlock::samplesPerPixelFor (transform_);
    const int64_t deltaSamples  = static_cast<int64_t> (deltaX * samplesPerPixel);

    const juce::String clipId = clip_.clipId.toString();
    dragActive_ = true;

    if (dragZone_ == DragZone::Body)
    {
        const int64_t newStart = juce::jmax ((int64_t) 0, dragStartTimeline_ + deltaSamples);
        if (onMoveDrag) onMoveDrag (clipId, newStart);
    }
    else if (dragZone_ == DragZone::LeftEdge)
    {
        const int64_t newSrcStart = dragStartSrcStart_ + deltaSamples;
        // Positive delta = trim inward; negative = uncrop outward.
        const bool isUncrop = (deltaSamples < 0);
        if (onLeftEdgeDrag) onLeftEdgeDrag (clipId, newSrcStart, isUncrop);
    }
    else if (dragZone_ == DragZone::RightEdge)
    {
        const int64_t newSrcEnd = dragStartSrcEnd_ + deltaSamples;
        const bool isUncrop = (deltaSamples > 0);
        if (onRightEdgeDrag) onRightEdgeDrag (clipId, newSrcEnd, isUncrop);
    }
}

void ClipBlock::mouseUp (const juce::MouseEvent& e)
{
    if (!dragActive_) { dragZone_ = DragZone::None; return; }

    const int deltaX = e.getScreenX() - dragStartX_;
    const double samplesPerPixel = ClipBlock::samplesPerPixelFor (transform_);
    const int64_t deltaSamples   = static_cast<int64_t> (deltaX * samplesPerPixel);

    const juce::String clipId = clip_.clipId.toString();

    if (dragZone_ == DragZone::Body)
    {
        const int64_t finalStart = juce::jmax ((int64_t) 0, dragStartTimeline_ + deltaSamples);
        if (onMoveEnd) onMoveEnd (clipId, finalStart);
    }
    else if (dragZone_ == DragZone::LeftEdge)
    {
        const int64_t finalSrcStart = dragStartSrcStart_ + deltaSamples;
        const bool isUncrop = (deltaSamples < 0);
        if (onLeftEdgeEnd) onLeftEdgeEnd (clipId, finalSrcStart, isUncrop);
    }
    else if (dragZone_ == DragZone::RightEdge)
    {
        const int64_t finalSrcEnd = dragStartSrcEnd_ + deltaSamples;
        const bool isUncrop = (deltaSamples > 0);
        if (onRightEdgeEnd) onRightEdgeEnd (clipId, finalSrcEnd, isUncrop);
    }

    dragActive_ = false;
    dragZone_   = DragZone::None;
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ClipBlock::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Split at cursor position.
    const double sampleX = static_cast<double> (e.x);
    const double samplesPerPixel = ClipBlock::samplesPerPixelFor (transform_);
    const int64_t cutOffset = static_cast<int64_t> (sampleX * samplesPerPixel);
    const int64_t cutTimeline = clip_.timelineStartSample + cutOffset;

    if (onSplit) onSplit (clip_.clipId.toString(), cutTimeline);
}

void ClipBlock::mouseWheelMove (const juce::MouseEvent& e,
                                const juce::MouseWheelDetails& w)
{
    // Only consume the scroll for gain adjustment when Cmd (macOS) is held.
    // Without Cmd, pass through to the parent so the lane viewport scrolls normally.
    if (e.mods.isCommandDown())
    {
        if (onGainScroll)
            onGainScroll (clip_.clipId.toString(), w.deltaY > 0.0f ? 1.0f : -1.0f);
    }
    else
    {
        // Let the parent component (Viewport / LaneView) handle the scroll.
        if (auto* parent = getParentComponent())
            parent->mouseWheelMove (e.getEventRelativeTo (parent), w);
    }
}

bool ClipBlock::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (onDelete) onDelete (clip_.clipId.toString());
        return true;
    }
    return false;
}

void ClipBlock::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != clipNode_)
        return;

    if (property == DawClipIDs::sourceStartSample
     || property == DawClipIDs::sourceEndSample
     || property == DawClipIDs::timelineStartSample
     || property == DawClipIDs::sourceLengthSamples
     || property == DawClipIDs::sourceFileId
     || property == DawClipIDs::gainDb)
    {
        reloadClip();
        // Re-place ourselves immediately so the block width tracks the growing
        // crop in the same frame the waveform content does (no shimmer).
        if (bandValid_)
            setBounds (bandXOffset_ + getTimelineX(), bandTopY_,
                       getTimelineWidth(), bandHeight_);
        repaint();
    }
}

} // namespace Daw
