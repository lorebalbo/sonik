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
#include "../Molecules/GroupOverviewStrip.h"
#include "../Molecules/LaneView.h"
#include "../DawLayoutMetrics.h"
#include "../../Model/ChannelGroup.h"
#include "../../Transform/TimelineTransform.h"
#include "../../Automation/AutomationModel.h"
#include "../../Automation/Ui/AutomationLaneStackView.h"

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
    // model — optional AutomationModel (PRD-0093). When non-null the group can
    //         reveal its automation lanes beneath the three source lanes. When
    //         null the AUTO disclosure is inert and the group behaves exactly as
    //         PRD-0067 (default layout unchanged).
    ChannelGroupView (juce::ValueTree trackTree,
                      juce::ValueTree deckTree,
                      const TimelineTransform& transform,
                      ClipBlock::WaveformSource waveformSource = {},
                      AutomationModel* model = nullptr,
                      ClipBlock::NameSource nameSource = {});

    ~ChannelGroupView() override;

    int getDeckIndex() const noexcept { return deckIndex_; }

    bool isCollapsed() const noexcept { return collapsed_; }
    void setCollapsed (bool shouldBeCollapsed);

    // PRD-0093 (revised): automation-lane disclosure, Logic-style. The track
    // header dropdown selects ONE automatable parameter; only that lane is shown
    // beneath the source lanes. Revealing without an explicit selection shows
    // the default parameter (volume).
    bool isAutomationRevealed() const noexcept { return automationRevealed_; }
    void setAutomationRevealed (bool shouldBeRevealed);

    // Select the automation parameter shown for this track (empty = hide). Also
    // updates the header dropdown label.
    void setAutomationParameter (const juce::String& parameterId);
    const juce::String& getAutomationParameter() const noexcept { return selectedAutoParam_; }

    // The mixer channel node backing the header volume fader (deck N -> channel N).
    void setMixerChannelTree (juce::ValueTree channelTree);

    // Fader level meter: maps a channel index to its current linear peak level.
    using ChannelLevelProvider = std::function<float (int channelIndex)>;
    void setChannelLevelProvider (const ChannelLevelProvider& provider)
    {
        if (provider)
            header_.setLevelProvider ([provider, idx = deckIndex_]() { return provider (idx); });
        else
            header_.setLevelProvider ({});
    }

    int getPreferredHeight() const noexcept
    {
        // The revealed automation lane survives a group collapse (it sits
        // directly below the DECK header in both states), so its height is
        // added regardless of collapse.
        int h = collapsed_ ? DawLayout::kCollapsedGroupHeight
                           : DawLayout::kExpandedGroupHeight;
        if (automationRevealed_ && automationStack_ != nullptr)
            h += automationStack_->getPreferredHeight();
        return h;
    }

    // PRD-0093: inject the shared read-only playhead-sample provider.
    void setAutomationPlayheadProvider (AutomationLaneStackView::PlayheadProvider provider);

    // PRD-0093: reposition automation lanes after a transform change.
    void refreshAutomationTransform();

    // Re-reads the deck source mode and updates each lane's active state.
    void refreshLaneActiveness();

    // Grouped-tracks mute/solo: re-reads the group/lane mute-solo flags (global
    // solo scope via the tracks container) and updates lane dimming plus the
    // collapsed overview strip. Called by the owning stack on any flag flip.
    void refreshAudibility();

    // Test access to the collapsed-group ghost overview.
    GroupOverviewStrip& getOverviewStrip() noexcept { return *overviewStrip_; }

    // Test access to the automation stack (null when no model was injected).
    AutomationLaneStackView* getAutomationStack() noexcept { return automationStack_.get(); }

    // PRD-0070: re-place clips in every lane after a transform change.
    void refreshClipLayout();

    void resized() override;

    // Grouped-tracks: the vertical ink bracket descending from the DECK header
    // along the indented child rows (drawn above the lanes so it stays visible
    // over their header fills).
    void paintOverChildren (juce::Graphics& g) override;

    // Exposed for tests: which lanes are currently active.
    bool isLaneActive (ChannelGroup::LaneKind kind) const;

    // Exposed for tests: which lanes currently sound under mute/solo.
    bool isLaneAudibleForTests (ChannelGroup::LaneKind kind) const
    {
        return lanes_[static_cast<size_t> (kind)]->isAudible();
    }

    // Test access to the header (fader meter state).
    ChannelGroupHeader& getHeaderForTests() noexcept { return header_; }

    // PRD-0098: the lane ValueTree whose content row contains `pointInGroup`
    // (group-local coordinates), or an invalid tree when the point is over the
    // header / automation area / no lane. Used for file-drop lane targeting.
    juce::ValueTree laneTreeAt (juce::Point<int> pointInGroup) const;

    // PRD-0098: the first source lane's tree (for menu-import fallback target).
    juce::ValueTree firstLaneTree() const;

    // PRD-0083/0084/0085/0086: Wire edit dispatcher into all lanes.
    void setEditDispatcher (Daw::EditCommandDispatcher* dispatcher);

    // PRD-0102: forward the shared snap settings + selection model to all lanes.
    void setClipInteraction (const SnapSettings* snap, ClipSelection* selection);

    std::function<void()> onPreferredHeightChanged;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    LaneView& laneFor (ChannelGroup::LaneKind kind);

    void showAutomationMenu();
    void updateHeaderAutomationDisplay();
    static juce::String labelForParameter (const juce::String& parameterId, bool isBoolean);

    juce::ValueTree trackTree_;
    juce::ValueTree deckTree_;
    int             deckIndex_ { 0 };

    ChannelGroupHeader header_;
    std::array<std::unique_ptr<LaneView>, ChannelGroup::kLaneCount> lanes_;

    // Grouped-tracks collapsed overview: the "ghost clip" lines shown over the
    // header's timeline area while the group is folded.
    std::unique_ptr<GroupOverviewStrip> overviewStrip_;

    // PRD-0093: automation lanes (created lazily on first reveal; hidden by
    // default so the group's default footprint is unchanged from PRD-0067).
    AutomationModel*                          automationModel_ { nullptr };
    std::unique_ptr<AutomationLaneStackView>  automationStack_;
    AutomationLaneStackView::PlayheadProvider automationPlayheadProvider_;

    bool collapsed_ { false };
    bool automationRevealed_ { false };

    // The parameter the header dropdown currently targets ("volume" by default
    // so a bare reveal shows the channel-fader lane, like Logic's default
    // Volume display — and it matches the volume fader sitting in the header).
    juce::String selectedAutoParam_ { "volume" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupView)
};

} // namespace Daw
