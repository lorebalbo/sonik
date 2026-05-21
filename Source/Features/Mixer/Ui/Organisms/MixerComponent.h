#pragma once
//==============================================================================
// PRD-0060: MixerComponent — the top-level mixer organism.
//
// Layout:
//   ┌────────────────────────────────────────────────────────┬─────────┐
//   │  ChannelStrip A   B   C   D                             │ MASTER │
//   │                                                          │        │
//   ├────────────────────────────────────────────────────────┴─────────┤
//   │                       CROSSFADER                                  │
//   └───────────────────────────────────────────────────────────────────┘
//
// Channel-strip count tracks the deck count: by listening to the "Decks"
// child of the supplied root state (PRD-0019). The mixer always renders
// the channels A and B; C and D appear when a 3rd/4th deck is created.
//
// This organism owns no audio state of its own — every interactive widget
// is bound directly to the MixerStateSchema's ValueTree.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ChannelStrip.h"
#include "MasterSection.h"
#include "../Molecules/CrossfaderRail.h"

#include <array>
#include <memory>

class MixerStateSchema;
struct MixerMeterSnapshot;

class MixerComponent final : public juce::Component,
                              private juce::ValueTree::Listener
{
public:
    MixerComponent (MixerStateSchema& schema,
                    MixerMeterSnapshot& meters,
                    juce::ValueTree   decksNode);

    ~MixerComponent() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    int  getActiveChannelCount() const noexcept { return activeChannelCount; }

    // Testing accessors.
    ChannelStrip*   getChannelStrip (int idx) noexcept;
    MasterSection&  getMasterSection() noexcept { return masterSection; }
    CrossfaderRail& getCrossfaderRail() noexcept { return crossfaderRail; }

    static constexpr int kCrossfaderRailH = 56;

private:
    void valueTreeChildAdded   (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int idx) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIdx, int newIdx) override;

    void recomputeActiveChannelCount();

    MixerStateSchema&   schema;
    MixerMeterSnapshot& meters;
    juce::ValueTree     decksNode;

    std::array<std::unique_ptr<ChannelStrip>, 4> channelStrips;
    MasterSection                                masterSection;
    CrossfaderRail                               crossfaderRail;

    int activeChannelCount { 2 };   // minimum 2, max 4

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};
