//==============================================================================
// PRD-0066: TimeRuler molecule implementation.
//==============================================================================

#include "TimeRuler.h"

#include <cmath>

namespace Daw
{

TimeRuler::TimeRuler (MasterGridService& grid, const TimelineTransform& transform)
    : grid_ (grid), transform_ (transform)
{
    setInterceptsMouseClicks (false, false);
}

std::vector<TimeRuler::TickInfo> TimeRuler::computeTicks() const
{
    std::vector<TickInfo> out;

    const double width = transform_.getViewportWidth();
    if (width <= 0.0)
        return out;

    const double pixelsPerBeat = transform_.getPixelsPerBeat();
    if (pixelsPerBeat <= 0.0)
        return out;

    // Visible sample range: from the left edge to one pixel past the right edge.
    const auto   ctx             = grid_.snapshotGrid();
    const double samplesPerBeat  = ctx.samplesPerBeat;
    if (samplesPerBeat <= 0.0)
        return out;

    const std::int64_t firstSample = transform_.getLeftEdgeSample();
    const double       samplesPerPixel = samplesPerBeat / pixelsPerBeat;
    const std::int64_t lastSample  = firstSample
        + static_cast<std::int64_t> (std::ceil (width * samplesPerPixel)) + 1;

    const auto lines = grid_.sampleGrid (firstSample, lastSample, /*subBeatDivision*/ 1);

    // At coarse zooms beat ticks would crowd; show bars only below the guard.
    const bool dropBeats = pixelsPerBeat < kMinBeatSpacingPx;

    out.reserve (lines.size());
    for (const auto& line : lines)
    {
        const bool isBar = (line.kind == MasterGridService::GridLineKind::Bar);
        if (! isBar && dropBeats)
            continue;

        const double x = transform_.sampleToX (line.sample);
        if (x < -2.0 || x > width + 2.0)
            continue;

        TickInfo tick;
        tick.x        = x;
        tick.kind     = isBar ? RulerTick::Kind::Bar : RulerTick::Kind::Beat;
        tick.hasLabel = isBar;
        tick.barNumber = isBar
            ? static_cast<int> (std::llround (line.beat / DawState::kBeatsPerBar)) + 1
            : 0;
        out.push_back (tick);
    }

    return out;
}

void TimeRuler::refresh()
{
    ticks_ = computeTicks();

    // Re-pool the child tick atoms to the required count (no per-frame alloc
    // churn once the pool has grown to its working size).
    while (tickComponents_.size() < static_cast<int> (ticks_.size()))
    {
        auto* t = tickComponents_.add (new RulerTick());
        addAndMakeVisible (t);
    }
    for (int i = 0; i < tickComponents_.size(); ++i)
        tickComponents_.getUnchecked (i)->setVisible (i < static_cast<int> (ticks_.size()));

    const int tickBandTop = kHeaderBandHeight;
    for (int i = 0; i < static_cast<int> (ticks_.size()); ++i)
    {
        const auto& info = ticks_[static_cast<size_t> (i)];
        auto*       comp = tickComponents_.getUnchecked (i);

        comp->setKind (info.kind);
        const int alignedX = juce::roundToInt (TimelineTransform::alignToPixelGrid (info.x));
        const int lineW    = RulerTick::lineWidthForKind (info.kind);
        comp->setBounds (alignedX, tickBandTop, lineW, kTickBandHeight);
    }

    repaint();
}

void TimeRuler::resized()
{
    refresh();
}

void TimeRuler::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Light ruler, Logic-style: a quiet header band for the bar numbers over the
    // tick band, separated from the lanes by a single 2-px ink baseline. The
    // heavy solid-ink band read as a wall; the timeline should recede behind
    // the clips, not compete with them.
    auto headerBand = bounds.removeFromTop (kHeaderBandHeight);
    g.setColour (kHeaderBandBg);
    g.fillRect (headerBand);

    // Tick band — light container tone.
    g.setColour (kTickBandBg);
    g.fillRect (bounds);

    // Bar-number labels (Space Mono, ink, small) next to each bar tick.
    g.setColour (kInk);
    g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain)));

    for (const auto& tick : ticks_)
    {
        if (! tick.hasLabel)
            continue;

        const int labelX = juce::roundToInt (TimelineTransform::alignToPixelGrid (tick.x)) + 4;
        const juce::Rectangle<int> labelArea (labelX, headerBand.getY(),
                                              48, headerBand.getHeight());
        g.drawText (juce::String (tick.barNumber), labelArea,
                    juce::Justification::centredLeft, false);
    }

    // Baseline under the ruler — the timeline's top edge.
    g.setColour (kInk);
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
}

} // namespace Daw
