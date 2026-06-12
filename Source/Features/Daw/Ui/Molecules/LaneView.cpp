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
                    ClipBlock::WaveformSource waveformSource,
                    ClipBlock::NameSource nameSource)
    : kind_ (kind),
      transform_ (transform),
      laneTree_ (std::move (laneTree)),
      waveformSource_ (std::move (waveformSource)),
      nameSource_ (std::move (nameSource))
{
    setInterceptsMouseClicks (false, true); // Children (ClipBlocks) receive mouse events.

    // Content layer hosts the clip blocks and clips them to the timeline area
    // (right of the header gutter), so clips never paint over the lane header.
    clipLayer_.setInterceptsMouseClicks (false, true); // Pass through to ClipBlock children.
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

        addClipBlockFor (clipNode);
    }

    layoutClips();
}

ClipBlock* LaneView::addClipBlockFor (juce::ValueTree clipNode)
{
    auto* block = clipBlocks_.add (
        new ClipBlock (clipNode, transform_, waveformSource_, nameSource_));

    // Wire editing callbacks if a dispatcher is available.
    if (dispatcher_ != nullptr)
        wireClipCallbacks (*block);

    // PRD-0102: hand the new block the shared snap settings + selection model.
    block->setClipInteraction (snap_, selection_);

    clipLayer_.addAndMakeVisible (block);

    // Place just this block (the content layer bounds are kept current here so the
    // incremental add path does not need a full layoutClips() sweep).
    const int gutter = DawLayout::kTrackHeaderWidth;
    clipLayer_.setBounds (getLocalBounds().withTrimmedLeft (gutter));
    block->applyTimelineBounds (0, 0, clipLayer_.getHeight());

    return block;
}

void LaneView::setEditDispatcher (Daw::EditCommandDispatcher* dispatcher)
{
    dispatcher_ = dispatcher;
    for (auto* block : clipBlocks_)
        wireClipCallbacks (*block);
}

void LaneView::setClipInteraction (const SnapSettings* snap, ClipSelection* selection)
{
    snap_      = snap;
    selection_ = selection;
    for (auto* block : clipBlocks_)
        block->setClipInteraction (snap_, selection_);
}

void LaneView::wireClipCallbacks (ClipBlock& block)
{
    if (dispatcher_ == nullptr)
        return;

    // PRD-0102: open ONE undo transaction at drag start. Every in-drag update
    // below mutates with the same UndoManager and so coalesces into this single
    // transaction (one drag = one undo step) — replacing the previous
    // begin-transaction-per-mouse-move which produced many undo entries.
    block.onDragBegin = [this] (const juce::String&) {
        dispatcher_->undoManager().beginNewTransaction ("Edit Clip");
    };

    // Move (body drag)
    block.onMoveDrag = [this] (const juce::String& id, int64_t newStart) {
        dispatcher_->moveClip (id, newStart);
    };
    block.onMoveEnd = [this] (const juce::String& id, int64_t finalStart) {
        dispatcher_->moveClip (id, finalStart);
    };

    // Trim (inward) / uncrop (outward) — same edge handles, direction chosen by
    // the block. The single drag transaction is already open (onDragBegin).
    block.onLeftEdgeDrag = [this] (const juce::String& id, int64_t newSrcStart, bool isUncrop) {
        if (isUncrop) dispatcher_->uncropClipStart (id, newSrcStart);
        else          dispatcher_->trimClipStart (id, newSrcStart);
    };
    block.onLeftEdgeEnd = [this] (const juce::String& id, int64_t finalSrcStart, bool isUncrop) {
        if (isUncrop) dispatcher_->uncropClipStart (id, finalSrcStart);
        else          dispatcher_->trimClipStart (id, finalSrcStart);
    };

    block.onRightEdgeDrag = [this] (const juce::String& id, int64_t newSrcEnd, bool isUncrop) {
        if (isUncrop) dispatcher_->uncropClipEnd (id, newSrcEnd);
        else          dispatcher_->trimClipEnd (id, newSrcEnd);
    };
    block.onRightEdgeEnd = [this] (const juce::String& id, int64_t finalSrcEnd, bool isUncrop) {
        if (isUncrop) dispatcher_->uncropClipEnd (id, finalSrcEnd);
        else          dispatcher_->trimClipEnd (id, finalSrcEnd);
    };

    // Split (double-click)
    block.onSplit = [this] (const juce::String& id, int64_t cutTimeline) {
        dispatcher_->splitClip (id, cutTimeline);
    };

    // Delete (Delete / Backspace key)
    block.onDelete = [this] (const juce::String& id) {
        dispatcher_->deleteClip (id);
    };

    // Gain (scroll wheel — Cmd+scroll reserved; plain scroll adjusts gain 1 dB)
    block.onGainScroll = [this] (const juce::String& id, float delta) {
        dispatcher_->setClipGain (id, delta);
    };
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
    // Incremental: host only the newly-added clip. The live-projection bridge
    // (PRD-0069) appends clips continuously while recording — especially during a
    // loop, which spawns a fresh clip per pass — so a full rebuildClips() here would
    // be O(n^2) and starve the UI thread (the cause of the recording-mode lag).
    if (parent == clipsContainer_ && child.hasType (DawIDs::clip))
        addClipBlockFor (child);
}

void LaneView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    // Incremental: drop only the block backing the removed clip node.
    if (parent != clipsContainer_ || ! child.hasType (DawIDs::clip))
        return;

    for (int i = 0; i < clipBlocks_.size(); ++i)
    {
        if (clipBlocks_[i]->getClipNode() == child)
        {
            clipBlocks_.remove (i); // juce::OwnedArray deletes the block
            break;
        }
    }
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

    // ---- Lane header cell: flat, Logic-style — the row's tonal step does the
    // separating (DESIGN.md "no-line" rule); the only ink is the track-header
    // column's continuous 2-px right edge.
    g.setColour (kHeaderFill);
    g.fillRect (headerCell);

    g.setColour (kInk);
    g.fillRect (gutter - 2, bounds.getY(), 2, bounds.getHeight());

    g.setColour (active_ ? kInk : kInkDim);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::bold));
    g.drawText (labelForKind (kind_),
                headerCell.withTrimmedLeft (20).withTrimmedRight (6),
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
