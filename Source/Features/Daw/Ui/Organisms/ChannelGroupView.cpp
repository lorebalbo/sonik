//==============================================================================
// PRD-0067: ChannelGroupView organism implementation.
//==============================================================================

#include "ChannelGroupView.h"

#include "Features/Deck/SourceModeReader.h"

namespace Daw
{

ChannelGroupView::ChannelGroupView (juce::ValueTree trackTree,
                                    juce::ValueTree deckTree,
                                    const TimelineTransform& transform,
                                    ClipBlock::WaveformSource waveformSource)
    : trackTree_ (std::move (trackTree)),
      deckTree_  (std::move (deckTree)),
      header_ (static_cast<int> (trackTree_.getProperty (DawIDs::deckIndex)))
{
    deckIndex_ = static_cast<int> (trackTree_.getProperty (DawIDs::deckIndex));

    lanes_[0] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Original, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Original), waveformSource);
    lanes_[1] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Instrumental, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Instrumental), waveformSource);
    lanes_[2] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Vocal, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Vocal), waveformSource);

    addAndMakeVisible (header_);
    for (auto& lane : lanes_)
        addAndMakeVisible (*lane);

    header_.onToggleCollapsed = [this]() { setCollapsed (! collapsed_); };

    if (deckTree_.isValid())
        deckTree_.addListener (this);

    refreshLaneActiveness();
}

ChannelGroupView::~ChannelGroupView()
{
    if (deckTree_.isValid())
        deckTree_.removeListener (this);
}

void ChannelGroupView::setCollapsed (bool shouldBeCollapsed)
{
    if (collapsed_ == shouldBeCollapsed)
        return;

    collapsed_ = shouldBeCollapsed;
    header_.setCollapsed (collapsed_);

    for (auto& lane : lanes_)
        lane->setVisible (! collapsed_);

    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

LaneView& ChannelGroupView::laneFor (ChannelGroup::LaneKind kind)
{
    return *lanes_[static_cast<int> (kind)];
}

bool ChannelGroupView::isLaneActive (ChannelGroup::LaneKind kind) const
{
    return lanes_[static_cast<int> (kind)]->isActive();
}

void ChannelGroupView::refreshClipLayout()
{
    for (auto& lane : lanes_)
        if (lane != nullptr)
            lane->refreshClipLayout();
}

void ChannelGroupView::refreshLaneActiveness()
{
    // Default (no deck tree): treat the deck as in original mode.
    bool originalActive = true;
    bool instActive     = false;
    bool vocalActive    = false;

    if (deckTree_.isValid())
    {
        SourceModeReader reader (deckTree_);
        const auto active = reader.getPublishedLanes();

        originalActive = false;
        for (auto lane : active)
        {
            switch (lane)
            {
                case SourceModeReader::Lane::Original:     originalActive = true; break;
                case SourceModeReader::Lane::Instrumental: instActive     = true; break;
                case SourceModeReader::Lane::Vocal:        vocalActive    = true; break;
            }
        }
    }

    laneFor (ChannelGroup::LaneKind::Original).setActive     (originalActive);
    laneFor (ChannelGroup::LaneKind::Instrumental).setActive (instActive);
    laneFor (ChannelGroup::LaneKind::Vocal).setActive        (vocalActive);
}

void ChannelGroupView::valueTreePropertyChanged (juce::ValueTree& tree,
                                                 const juce::Identifier&)
{
    // Any change on the deck tree or its Stems child can affect lane audibility.
    if (tree == deckTree_ || tree.getParent() == deckTree_)
        refreshLaneActiveness();
}

void ChannelGroupView::resized()
{
    auto bounds = getLocalBounds();
    header_.setBounds (bounds.removeFromTop (DawLayout::kGroupHeaderHeight));

    if (collapsed_)
        return;

    for (auto& lane : lanes_)
        lane->setBounds (bounds.removeFromTop (DawLayout::kLaneHeight));
}

} // namespace Daw
