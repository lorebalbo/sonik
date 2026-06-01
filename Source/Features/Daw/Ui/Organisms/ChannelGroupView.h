#pragma once
//==============================================================================
// PRD-0067: ChannelGroupView organism.
//
// Renders one per-deck channel group: a ChannelGroupHeader plus three stacked
// LaneView molecules in fixed order (Original, Instrumental, Vocal). It observes
// the owning deck's source-mode state (PRD-0062 via SourceModeReader) and marks
// each lane active/inactive accordingly (§1.5.1 — lanes are dimmed, never
// hidden, so the vertical footprint is stable). Group-level collapse folds the
// three lanes to just the header row (§1.5.2).
//
// The group reads the deck tree purely on the message thread via a
// juce::ValueTree::Listener (Observer pattern); it never polls deck or
// audio-thread state. No DSP, no audio-thread code.
//==============================================================================

#include <array>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Molecules/ChannelGroupHeader.h"
#include "../Molecules/LaneView.h"
#include "../DawLayoutMetrics.h"
#include "../../Model/ChannelGroup.h"
#include "../../Transform/TimelineTransform.h"

namespace Daw
{

class ChannelGroupView final : public juce::Component,
                               private juce::ValueTree::Listener
{
public:
    // trackTree  — the daw.tracks[i] node (carries deckIndex + lanes/clips).
    // deckTree   — the owning deck node (source-mode + stem mutes), may be invalid.
    // transform  — shared horizontal time axis (PRD-0065/0066).
    // waveformSource — read-only cache accessor passed to each lane's clips.
    ChannelGroupView (juce::ValueTree trackTree,
                      juce::ValueTree deckTree,
                      const TimelineTransform& transform,
                      ClipBlock::WaveformSource waveformSource = {});

    ~ChannelGroupView() override;

    int getDeckIndex() const noexcept { return deckIndex_; }

    bool isCollapsed() const noexcept { return collapsed_; }
    void setCollapsed (bool shouldBeCollapsed);

    int getPreferredHeight() const noexcept
    {
        return collapsed_ ? DawLayout::kCollapsedGroupHeight
                          : DawLayout::kExpandedGroupHeight;
    }

    // Re-reads the deck source mode and updates each lane's active state.
    void refreshLaneActiveness();

    // PRD-0070: re-place clips in every lane after a transform change.
    void refreshClipLayout();

    void resized() override;

    // Exposed for tests: which lanes are currently active.
    bool isLaneActive (ChannelGroup::LaneKind kind) const;

    // PRD-0083/0084/0085/0086: Wire edit dispatcher into all lanes.
    void setEditDispatcher (Daw::EditCommandDispatcher* dispatcher);

    std::function<void()> onPreferredHeightChanged;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    LaneView& laneFor (ChannelGroup::LaneKind kind);

    juce::ValueTree trackTree_;
    juce::ValueTree deckTree_;
    int             deckIndex_ { 0 };

    ChannelGroupHeader header_;
    std::array<std::unique_ptr<LaneView>, ChannelGroup::kLaneCount> lanes_;

    bool collapsed_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupView)
};

} // namespace Daw
