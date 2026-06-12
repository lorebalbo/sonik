#pragma once
//==============================================================================
// PRD-0067: ChannelGroupStack organism.
//
// The vertical stack of per-deck channel groups inside the DAW panel body. It
// observes the daw.tracks[] container (PRD-0063) via a juce::ValueTree::Listener
// and creates/destroys one ChannelGroupView per track node — a pure projection
// of the tree (group <-> track node). Groups are ordered top-to-bottom by deck
// id ascending, stable across load/eject churn regardless of child insertion
// order (§1.5.4). When the stacked height exceeds the panel body, the owning
// Viewport scrolls (§1.5.7); this component just reports its full content height.
//
// The owning deck node for each track (for source-mode lane activeness) is
// resolved through an injected callback (deckIndex -> deck ValueTree), keeping
// this view decoupled from the deck-tree layout and fully testable with a
// synthetic tree. Message/UI thread only; no audio-thread code.
//==============================================================================

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChannelGroupView.h"
#include "../DawLayoutMetrics.h"
#include "../../State/DawState.h"
#include "../../Transform/TimelineTransform.h"
#include "../../Automation/AutomationModel.h"
#include "../../Automation/Ui/AutomationLaneStackView.h"

namespace Daw
{

class ChannelGroupStack final : public juce::Component,
                                private juce::ValueTree::Listener
{
public:
    using DeckResolver    = std::function<juce::ValueTree (int deckIndex)>;
    using ChannelResolver = std::function<juce::ValueTree (int channelIndex)>;

    // dawBranch    — the "Daw" branch (holds the "tracks" container).
    // transform    — shared horizontal time axis (PRD-0065/0066).
    // deckResolver — maps a track's deckIndex to its deck ValueTree (may return
    //                an invalid tree, in which case the group renders original-mode).
    // waveformSource — read-only waveform cache accessor passed to each clip.
    // model — optional AutomationModel (PRD-0093) passed to each group so it can
    //         reveal automation lanes. Null keeps the PRD-0067 behaviour.
    ChannelGroupStack (juce::ValueTree dawBranch,
                       const TimelineTransform& transform,
                       DeckResolver deckResolver,
                       ClipBlock::WaveformSource waveformSource = {},
                       AutomationModel* model = nullptr,
                       ClipBlock::NameSource nameSource = {});

    ~ChannelGroupStack() override;

    // PRD-0093: inject the shared read-only playhead-sample provider into every
    // group's automation lanes.
    void setAutomationPlayheadProvider (AutomationLaneStackView::PlayheadProvider provider);

    // Maps a track's deck index to its mixer channel ValueTree (identity
    // deck N -> channel N) so each track header's volume fader drives the
    // authoritative channel fader. Retained for groups rebuilt later.
    void setMixerChannelResolver (ChannelResolver resolver);

    // PRD-0093: reposition automation lanes after a transform change.
    void refreshAutomationTransform();

    int  getNumGroups() const noexcept { return static_cast<int> (groups_.size()); }
    int  getContentHeight() const;

    // Lays the stack out to its full content height (caller sets the width).
    void layoutToContentHeight (int width);

    // PRD-0070: re-place clips in every group after a transform change.
    void refreshClipLayout();

    // PRD-0083/0084/0085/0086: Wire edit dispatcher into every group/lane.
    void setEditDispatcher (Daw::EditCommandDispatcher* dispatcher);

    // PRD-0102: forward the shared snap settings + selection model into every
    // group (and thereby every lane/clip). Re-applied after group rebuilds.
    void setClipInteraction (const SnapSettings* snap, ClipSelection* selection);

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Fired whenever the stack's content height changes (group add/remove/collapse)
    // so the owning panel/viewport can re-flow.
    std::function<void()> onContentHeightChanged;

    // PRD-0098: the lane ValueTree whose content row contains `pointInStack`
    // (stack-local coordinates), or invalid when over no lane. Used for file-
    // drop lane targeting.
    juce::ValueTree laneTreeAt (juce::Point<int> pointInStack) const;

    // PRD-0098: the first lane of the first (topmost) group, used as the menu-
    // import fallback target when no lane is focused (§1.5.7).
    juce::ValueTree firstLaneTree() const;

    // Test access.
    ChannelGroupView* getGroupByDeckIndex (int deckIndex) const;
    const std::vector<std::unique_ptr<ChannelGroupView>>& getGroups() const { return groups_; }

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildAdded   (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void rebuildGroups();
    void notifyContentHeightChanged();

    juce::ValueTree   dawBranch_;
    juce::ValueTree   tracks_;
    const TimelineTransform& transform_;
    DeckResolver      deckResolver_;
    ChannelResolver   channelResolver_;
    ClipBlock::WaveformSource waveformSource_;
    ClipBlock::NameSource     nameSource_;
    AutomationModel*  automationModel_ { nullptr };
    AutomationLaneStackView::PlayheadProvider automationPlayheadProvider_;

    std::vector<std::unique_ptr<ChannelGroupView>> groups_;

    // Wiring retained so groups created by a later rebuildGroups() (track add /
    // remove) inherit it too — previously a rebuilt group lost its editing
    // dispatcher entirely (PRD-0102 fix; mirrors the snap/selection wiring).
    Daw::EditCommandDispatcher* dispatcher_ { nullptr };
    const SnapSettings*         snap_       { nullptr };
    ClipSelection*              selection_  { nullptr };

    static inline const juce::Colour kBodyBg { 0xFFF3F3F4 }; // surface-container-low

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupStack)
};

} // namespace Daw
