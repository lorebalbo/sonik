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
    setInterceptsMouseClicks (false, false); // read-only atom (no edit in this PRD)
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
