//==============================================================================
// PRD-0067 / PRD-0068: LaneView molecule implementation.
//==============================================================================

#include "LaneView.h"

#include "../../State/DawState.h"   // DawIDs::clips / clip

namespace Daw
{

LaneView::LaneView (ChannelGroup::LaneKind kind,
                    const TimelineTransform& transform,
                    juce::ValueTree laneTree,
                    ClipBlock::WaveformSource waveformSource)
    : kind_ (kind),
      transform_ (transform),
      laneTree_ (std::move (laneTree)),
      waveformSource_ (std::move (waveformSource))
{
    setInterceptsMouseClicks (false, false);

    // Content layer hosts the clip blocks and clips them to the timeline area
    // (right of the header gutter), so clips never paint over the lane header.
    clipLayer_.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (clipLayer_);

    if (laneTree_.isValid())
    {
        clipsContainer_ = laneTree_.getChildWithName (DawIDs::clips);
        laneTree_.addListener (this);
        rebuildClips();
    }
}

LaneView::~LaneView()
{
    if (laneTree_.isValid())
        laneTree_.removeListener (this);
}

void LaneView::setActive (bool shouldBeActive)
{
    if (active_ == shouldBeActive)
        return;
    active_ = shouldBeActive;
    repaint();
}

juce::String LaneView::labelForKind (ChannelGroup::LaneKind kind)
{
    switch (kind)
    {
        case ChannelGroup::LaneKind::Original:     return "ORIGINAL";
        case ChannelGroup::LaneKind::Instrumental: return "INSTRUMENTAL";
        case ChannelGroup::LaneKind::Vocal:        return "VOCAL";
    }
    return "ORIGINAL";
}

juce::Colour LaneView::contentToneForKind (ChannelGroup::LaneKind kind)
{
    switch (kind)
    {
        case ChannelGroup::LaneKind::Original:     return juce::Colour (0xFFFDFDFD);
        case ChannelGroup::LaneKind::Instrumental: return juce::Colour (0xFFF3F3F4);
        case ChannelGroup::LaneKind::Vocal:        return juce::Colour (0xFFEAEAEB);
    }
    return juce::Colour (0xFFFDFDFD);
}

void LaneView::rebuildClips()
{
    clipBlocks_.clear();

    if (! clipsContainer_.isValid())
        return;

    for (int i = 0; i < clipsContainer_.getNumChildren(); ++i)
    {
        auto clipNode = clipsContainer_.getChild (i);
        if (! clipNode.hasType (DawIDs::clip))
            continue;

        auto* block = clipBlocks_.add (
            new ClipBlock (clipNode, transform_, waveformSource_));
        clipLayer_.addAndMakeVisible (block);
    }

    layoutClips();
}

void LaneView::refreshClipLayout()
{
    layoutClips();
}

void LaneView::layoutClips()
{
    const int gutter = DawLayout::kTrackHeaderWidth;
    clipLayer_.setBounds (getLocalBounds().withTrimmedLeft (gutter));

    // Inside the content layer the timeline origin is x = 0 (the layer already
    // starts at the gutter); the layer clips any block scrolled past its edges.
    for (auto* block : clipBlocks_)
        block->applyTimelineBounds (0, 0, clipLayer_.getHeight());
}

void LaneView::resized()
{
    layoutClips();
}

void LaneView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == clipsContainer_ && child.hasType (DawIDs::clip))
        rebuildClips();
}

void LaneView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == clipsContainer_ && child.hasType (DawIDs::clip))
        rebuildClips();
}

void LaneView::paint (juce::Graphics& g)
{
    const auto bounds  = getLocalBounds();
    const int  gutter  = DawLayout::kTrackHeaderWidth;

    auto headerCell  = bounds.withWidth (gutter);
    auto contentCell = bounds.withTrimmedLeft (gutter);

    // ---- Content row: tonal base by kind --------------------------------
    g.setColour (contentToneForKind (kind_));
    g.fillRect (contentCell);

    // Inactive lanes: sparse monochrome dither overlay (no colour, no hide).
    if (! active_)
        paintInactiveDither (g, contentCell);

    // ---- Lane header cell -----------------------------------------------
    g.setColour (kHeaderFill);
    g.fillRect (headerCell);

    g.setColour (kInk);
    g.drawRect (headerCell, 2);                 // 2-px solid border
    g.drawLine ((float) contentCell.getX(), (float) bounds.getY(),
                (float) contentCell.getX(), (float) bounds.getBottom(), 2.0f);

    g.setColour (active_ ? kInk : kInkDim);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.drawText (labelForKind (kind_),
                headerCell.withTrimmedLeft (6).withTrimmedRight (4),
                juce::Justification::centredLeft, false);
}

void LaneView::paintInactiveDither (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Sparse checkerboard at low density: a 1-px dot every 4 px on a shifted
    // grid. Pure ink at low coverage reads as "muted" without colour.
    g.setColour (juce::Colour (0xFF2D2D2D).withAlpha (0.10f));
    for (int y = area.getY(); y < area.getBottom(); y += 4)
        for (int x = area.getX() + ((y / 4) % 2) * 2; x < area.getRight(); x += 4)
            g.fillRect (x, y, 1, 1);
}

} // namespace Daw
