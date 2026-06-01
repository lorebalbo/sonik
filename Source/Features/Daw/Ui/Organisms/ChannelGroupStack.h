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

namespace Daw
{

class ChannelGroupStack final : public juce::Component,
                                private juce::ValueTree::Listener
{
public:
    using DeckResolver = std::function<juce::ValueTree (int deckIndex)>;

    // dawBranch    — the "Daw" branch (holds the "tracks" container).
    // transform    — shared horizontal time axis (PRD-0065/0066).
    // deckResolver — maps a track's deckIndex to its deck ValueTree (may return
    //                an invalid tree, in which case the group renders original-mode).
    // waveformSource — read-only waveform cache accessor passed to each clip.
    ChannelGroupStack (juce::ValueTree dawBranch,
                       const TimelineTransform& transform,
                       DeckResolver deckResolver,
                       ClipBlock::WaveformSource waveformSource = {});

    ~ChannelGroupStack() override;

    int  getNumGroups() const noexcept { return static_cast<int> (groups_.size()); }
    int  getContentHeight() const;

    // Lays the stack out to its full content height (caller sets the width).
    void layoutToContentHeight (int width);

    // PRD-0070: re-place clips in every group after a transform change.
    void refreshClipLayout();

    // PRD-0083/0084/0085/0086: Wire edit dispatcher into every group/lane.
    void setEditDispatcher (Daw::EditCommandDispatcher* dispatcher);

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Fired whenever the stack's content height changes (group add/remove/collapse)
    // so the owning panel/viewport can re-flow.
    std::function<void()> onContentHeightChanged;

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
    ClipBlock::WaveformSource waveformSource_;

    std::vector<std::unique_ptr<ChannelGroupView>> groups_;

    static inline const juce::Colour kBodyBg { 0xFFF3F3F4 }; // surface-container-low

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupStack)
};

} // namespace Daw
