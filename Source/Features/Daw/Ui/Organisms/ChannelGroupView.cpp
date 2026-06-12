//==============================================================================
// PRD-0067: ChannelGroupView organism implementation.
//==============================================================================

#include "ChannelGroupView.h"

#include "Features/Deck/SourceModeReader.h"

namespace Daw
{

namespace
{
    // PRD-0090/0093: channel owner letter from deck index via the identity map
    // A->0 .. D->3 (§1.5.4). Out-of-range deck indices clamp to the first owner.
    juce::String ownerForDeckIndex (int deckIndex)
    {
        static const char* const kOwners[4] = { "A", "B", "C", "D" };
        return juce::String (kOwners[juce::jlimit (0, 3, deckIndex)]);
    }
}

ChannelGroupView::ChannelGroupView (juce::ValueTree trackTree,
                                    juce::ValueTree deckTree,
                                    const TimelineTransform& transform,
                                    ClipBlock::WaveformSource waveformSource,
                                    AutomationModel* model,
                                    ClipBlock::NameSource nameSource)
    : trackTree_ (std::move (trackTree)),
      deckTree_  (std::move (deckTree)),
      header_ (static_cast<int> (trackTree_.getProperty (DawIDs::deckIndex))),
      automationModel_ (model)
{
    deckIndex_ = static_cast<int> (trackTree_.getProperty (DawIDs::deckIndex));

    lanes_[0] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Original, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Original), waveformSource, nameSource);
    lanes_[1] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Instrumental, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Instrumental), waveformSource, nameSource);
    lanes_[2] = std::make_unique<LaneView> (
        ChannelGroup::LaneKind::Vocal, transform,
        ChannelGroup::findLane (trackTree_, ChannelGroup::LaneKind::Vocal), waveformSource, nameSource);

    addAndMakeVisible (header_);
    for (auto& lane : lanes_)
        addAndMakeVisible (*lane);

    // PRD-0093 (revised): build the automation stack now (so populated/empty
    // ordering and preferred height are correct) but keep it HIDDEN until a
    // parameter is selected — the default group footprint is therefore
    // unchanged from PRD-0067. The stack displays ONE parameter at a time
    // (Logic-style track automation), driven by the header dropdown.
    if (automationModel_ != nullptr)
    {
        automationStack_ = std::make_unique<AutomationLaneStackView> (
            ownerForDeckIndex (deckIndex_), *automationModel_, transform);
        automationStack_->setVisibleParameter (selectedAutoParam_);
        automationStack_->setVisible (false);
        addChildComponent (*automationStack_);
        header_.onAutomationDropdown = [this]() { showAutomationMenu(); };
    }

    header_.onToggleCollapsed = [this]() { setCollapsed (! collapsed_); };
    updateHeaderAutomationDisplay();

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

    // Collapsing the whole group also hides any revealed automation lanes.
    if (automationStack_ != nullptr)
        automationStack_->setVisible (! collapsed_ && automationRevealed_);

    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void ChannelGroupView::setAutomationRevealed (bool shouldBeRevealed)
{
    if (automationRevealed_ == shouldBeRevealed || automationStack_ == nullptr)
        return;

    automationRevealed_ = shouldBeRevealed;
    automationStack_->setVisibleParameter (selectedAutoParam_);
    automationStack_->setVisible (! collapsed_ && automationRevealed_);
    updateHeaderAutomationDisplay();

    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void ChannelGroupView::setAutomationParameter (const juce::String& parameterId)
{
    if (automationStack_ == nullptr)
        return;

    if (parameterId.isEmpty())
    {
        setAutomationRevealed (false);
        return;
    }

    selectedAutoParam_ = parameterId;
    automationStack_->setVisibleParameter (selectedAutoParam_);

    if (! automationRevealed_)
    {
        setAutomationRevealed (true);   // lays out + notifies height change
        return;
    }

    updateHeaderAutomationDisplay();
    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void ChannelGroupView::setMixerChannelTree (juce::ValueTree channelTree)
{
    header_.setMixerChannelTree (std::move (channelTree));
}

juce::String ChannelGroupView::labelForParameter (const juce::String& parameterId,
                                                  bool isBoolean)
{
    return isBoolean
        ? AutomationValueRange::booleanParamLabel (parameterId)
        : AutomationValueRange::forContinuousParameter (parameterId).paramLabel;
}

void ChannelGroupView::updateHeaderAutomationDisplay()
{
    // The dropdown reads "OFF" when no lane is shown; otherwise the selected
    // parameter's hardware-style label (GAIN / FILTER / HIGH / ... ).
    juce::String label ("OFF");
    if (automationRevealed_)
    {
        bool isBool = false;
        for (const auto& p : AutomationLaneStackView::getAvailableParameters())
            if (p.parameterId == selectedAutoParam_)
                isBool = p.isBoolean;
        label = labelForParameter (selectedAutoParam_, isBool);
    }
    header_.setAutomationDisplay (label, automationRevealed_);
}

void ChannelGroupView::showAutomationMenu()
{
    // Logic-style parameter picker: Off, then every automatable parameter for
    // this channel, ticked at the current selection.
    juce::PopupMenu menu;
    menu.addItem (1, "Off", true, ! automationRevealed_);
    menu.addSeparator();

    const auto params = AutomationLaneStackView::getAvailableParameters();
    for (size_t i = 0; i < params.size(); ++i)
        menu.addItem (static_cast<int> (i) + 2,
                      labelForParameter (params[i].parameterId, params[i].isBoolean),
                      true,
                      automationRevealed_ && params[i].parameterId == selectedAutoParam_);

    juce::Component::SafePointer<ChannelGroupView> safe (this);
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetComponent (&header_)
            .withTargetScreenArea (
                header_.localAreaToGlobal (header_.automationDropdownBounds()))
            .withMinimumWidth (header_.automationDropdownBounds().getWidth()),
        [safe, params] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            if (result == 1)
                safe->setAutomationParameter ({});
            else if (result - 2 < static_cast<int> (params.size()))
                safe->setAutomationParameter (params[static_cast<size_t> (result - 2)].parameterId);
        });
}

void ChannelGroupView::setAutomationPlayheadProvider (
    AutomationLaneStackView::PlayheadProvider provider)
{
    automationPlayheadProvider_ = provider;
    if (automationStack_ != nullptr)
        automationStack_->setPlayheadProvider (std::move (provider));
}

void ChannelGroupView::refreshAutomationTransform()
{
    if (automationStack_ != nullptr)
        automationStack_->refreshTransform();
}

LaneView& ChannelGroupView::laneFor (ChannelGroup::LaneKind kind)
{
    return *lanes_[static_cast<size_t> (kind)];
}

bool ChannelGroupView::isLaneActive (ChannelGroup::LaneKind kind) const
{
    return lanes_[static_cast<size_t> (kind)]->isActive();
}

void ChannelGroupView::refreshClipLayout()
{
    for (auto& lane : lanes_)
        if (lane != nullptr)
            lane->refreshClipLayout();
}

void ChannelGroupView::setEditDispatcher (Daw::EditCommandDispatcher* dispatcher)
{
    for (auto& lane : lanes_)
        if (lane != nullptr)
            lane->setEditDispatcher (dispatcher);

    // PRD-0094: automation lane editing flows through the same shared command
    // layer, so the automation stack must receive the dispatcher too.
    if (automationStack_ != nullptr)
        automationStack_->setEditDispatcher (dispatcher);
}

void ChannelGroupView::setClipInteraction (const SnapSettings* snap, ClipSelection* selection)
{
    for (auto& lane : lanes_)
        if (lane != nullptr)
            lane->setClipInteraction (snap, selection);
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

    // PRD-0093: automation lanes occupy the rows BENEATH the three source lanes.
    if (automationStack_ != nullptr && automationRevealed_)
        automationStack_->setBounds (
            bounds.removeFromTop (automationStack_->getPreferredHeight()));
}

juce::ValueTree ChannelGroupView::laneTreeAt (juce::Point<int> pointInGroup) const
{
    if (collapsed_)
        return {};

    for (const auto& lane : lanes_)
    {
        if (lane == nullptr)
            continue;
        if (lane->getBounds().contains (pointInGroup))
            return lane->getLaneTree();
    }
    return {};
}

juce::ValueTree ChannelGroupView::firstLaneTree() const
{
    if (lanes_[0] != nullptr)
        return lanes_[0]->getLaneTree();
    return {};
}

} // namespace Daw
