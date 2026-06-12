#pragma once
//==============================================================================
// PRD-0067 / PRD-0068: LaneView molecule.
//
// One horizontal lane of a channel group: a left header cell carrying the
// all-caps Space Mono lane label (ORIGINAL / INSTRUMENTAL / VOCAL) and a content
// row to the right that spans the timeline and HOSTS the lane's clip blocks
// (PRD-0068). The lane observes its "clips" container (Observer pattern) and
// creates/removes ClipBlock atoms as clips appear/vanish; each block is placed
// horizontally by the shared PRD-0065 TimelineTransform (offset past the header
// gutter) so a sample S lands at the same x as the ruler tick for S.
//
// The three lanes of a group are distinguished by tonal layering (a tonal step
// per lane kind), never by colour (DESIGN.md §1.3.7). A lane that is not
// currently audible for the deck's source mode (PRD-0062) is rendered "inactive"
// via a sparse monochrome dither overlay — never hidden — so the group's
// vertical footprint is stable across source-mode toggles (PRD-0067 §1.5.1).
//
// Message/UI thread only; no audio-thread code.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DawLayoutMetrics.h"
#include "../Atoms/ClipBlock.h"
#include "../Atoms/MuteSoloButton.h"
#include "../../Model/ChannelGroup.h"
#include "../../Transform/TimelineTransform.h"
#include "../../Editing/EditCommands.h"

namespace Daw
{

class LaneView final : public juce::Component,
                       private juce::ValueTree::Listener
{
public:
    LaneView (ChannelGroup::LaneKind kind,
              const TimelineTransform& transform,
              juce::ValueTree laneTree = {},
              ClipBlock::WaveformSource waveformSource = {},
              ClipBlock::NameSource nameSource = {});

    ~LaneView() override;

    ChannelGroup::LaneKind getLaneKind() const noexcept { return kind_; }

    // PRD-0098: the backing lane ValueTree (daw...lanes[i]) so a file drop can
    // target the lane node under the cursor. May be invalid in tests.
    juce::ValueTree getLaneTree() const noexcept { return laneTree_; }

    bool isActive() const noexcept { return active_; }
    void setActive (bool shouldBeActive);

    // Grouped-tracks mute/solo: whether this lane currently SOUNDS under the
    // combined group/lane mute/solo flags (MuteSolo). An inaudible lane
    // is dimmed with the same dither treatment as a source-mode-inactive one.
    // The owning ChannelGroupView computes this (it sees the global solo state).
    bool isAudible() const noexcept { return audible_; }
    void setAudible (bool shouldBeAudible);

    int getNumClipBlocks() const noexcept { return clipBlocks_.size(); }

    // Re-reads the clips container and rebuilds the hosted ClipBlock atoms.
    void rebuildClips();

    // All-caps Space Mono label for the lane header cell.
    static juce::String labelForKind (ChannelGroup::LaneKind kind);

    // Tonal base fill for the content row by lane kind (monochrome tonal step).
    static juce::Colour contentToneForKind (ChannelGroup::LaneKind kind);

    void resized() override;
    void paint (juce::Graphics& g) override;

    // PRD-0083/0084/0085/0086: Wire the edit dispatcher after construction.
    // All ClipBlocks created before this call will be re-wired in rebuildClips();
    // blocks created after inherit the dispatcher automatically.
    void setEditDispatcher (Daw::EditCommandDispatcher* dispatcher);

    // PRD-0102: inject the shared snap settings + selection model (owned by the
    // DawPanel). Applied to all existing and future ClipBlocks on this lane.
    void setClipInteraction (const SnapSettings* snap, ClipSelection* selection);

    // PRD-0070: re-place hosted ClipBlocks after a zoom/scroll transform change
    // (the lane bounds don't change on zoom, so resized() won't fire).
    void refreshClipLayout();

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildAdded   (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void layoutClips();
    // Create + host ONE ClipBlock for the given clip node (incremental path used by
    // both the full rebuild and the single-child-added listener, avoiding an O(n^2)
    // full rebuild as live-projection appends clips during recording).
    ClipBlock* addClipBlockFor (juce::ValueTree clipNode);
    void wireClipCallbacks (ClipBlock& block);
    static void paintInactiveDither (juce::Graphics& g, juce::Rectangle<int> area);

    static inline const juce::Colour kInk        { 0xFF2D2D2D };
    static inline const juce::Colour kInkDim     { 0xFF9A9A9A };
    static inline const juce::Colour kHeaderFill { 0xFFE5E5E5 }; // deck-header tone

    ChannelGroup::LaneKind     kind_;
    const TimelineTransform&   transform_;
    juce::ValueTree            laneTree_;
    juce::ValueTree            clipsContainer_;
    ClipBlock::WaveformSource  waveformSource_;
    ClipBlock::NameSource      nameSource_;

    // Clip blocks live in this child layer, which spans ONLY the content area
    // (right of the header gutter). Because JUCE clips a component's children to
    // its bounds, clips scrolled past the left edge are cropped here instead of
    // bleeding over the lane header / channel names (PRD-0070 fix).
    juce::Component             clipLayer_;

    juce::OwnedArray<ClipBlock> clipBlocks_;
    bool                        active_ { true };
    bool                        audible_ { true };
    Daw::EditCommandDispatcher* dispatcher_ { nullptr };

    // Grouped-tracks mute/solo: per-lane M / S toggles in the header cell,
    // writing the lane node's flags (the ValueTree single source of truth).
    MuteSoloButton muteButton_ { "M", DawIDs::muted };
    MuteSoloButton soloButton_ { "S", DawIDs::solo };

    // PRD-0102: shared snap settings + selection (owned by DawPanel), forwarded
    // to each ClipBlock at creation and on setClipInteraction().
    const SnapSettings* snap_      { nullptr };
    ClipSelection*      selection_ { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LaneView)
};

} // namespace Daw
