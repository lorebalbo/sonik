//==============================================================================
// PRD-0067: ChannelGroupStack organism implementation.
//==============================================================================

#include "ChannelGroupStack.h"

#include <algorithm>

namespace Daw
{

ChannelGroupStack::ChannelGroupStack (juce::ValueTree dawBranch,
                                      const TimelineTransform& transform,
                                      DeckResolver deckResolver,
                                      ClipBlock::WaveformSource waveformSource,
                                      AutomationModel* model)
    : dawBranch_ (std::move (dawBranch)),
      transform_ (transform),
      deckResolver_ (std::move (deckResolver)),
      waveformSource_ (std::move (waveformSource)),
      automationModel_ (model)
{
    tracks_ = dawBranch_.getChildWithName (DawIDs::tracks);
    if (dawBranch_.isValid())
        dawBranch_.addListener (this);

    rebuildGroups();
}

ChannelGroupStack::~ChannelGroupStack()
{
    if (dawBranch_.isValid())
        dawBranch_.removeListener (this);
}

void ChannelGroupStack::rebuildGroups()
{
    groups_.clear();

    if (! tracks_.isValid())
    {
        resized();
        return;
    }

    // Collect track nodes, then sort by deck id ascending (§1.5.4) so the visual
    // order is stable regardless of child insertion order.
    std::vector<juce::ValueTree> trackNodes;
    for (int i = 0; i < tracks_.getNumChildren(); ++i)
    {
        auto t = tracks_.getChild (i);
        if (t.hasType (DawIDs::track))
            trackNodes.push_back (t);
    }

    std::sort (trackNodes.begin(), trackNodes.end(),
               [] (const juce::ValueTree& a, const juce::ValueTree& b)
               {
                   return static_cast<int> (a.getProperty (DawIDs::deckIndex))
                        < static_cast<int> (b.getProperty (DawIDs::deckIndex));
               });

    for (auto& node : trackNodes)
    {
        const int deckIndex = static_cast<int> (node.getProperty (DawIDs::deckIndex));
        juce::ValueTree deckTree = deckResolver_ ? deckResolver_ (deckIndex)
                                                 : juce::ValueTree();

        auto group = std::make_unique<ChannelGroupView> (node, deckTree, transform_,
                                                         waveformSource_, automationModel_);
        group->onPreferredHeightChanged = [this]() { notifyContentHeightChanged(); };
        if (automationPlayheadProvider_)
            group->setAutomationPlayheadProvider (automationPlayheadProvider_);
        addAndMakeVisible (*group);
        groups_.push_back (std::move (group));
    }

    resized();
    notifyContentHeightChanged();
}

int ChannelGroupStack::getContentHeight() const
{
    int h = 0;
    for (const auto& g : groups_)
        h += g->getPreferredHeight();
    return h;
}

ChannelGroupView* ChannelGroupStack::getGroupByDeckIndex (int deckIndex) const
{
    for (const auto& g : groups_)
        if (g->getDeckIndex() == deckIndex)
            return g.get();
    return nullptr;
}

void ChannelGroupStack::layoutToContentHeight (int width)
{
    setSize (width, std::max (1, getContentHeight()));
}

void ChannelGroupStack::refreshClipLayout()
{
    for (const auto& g : groups_)
        if (g != nullptr)
            g->refreshClipLayout();
}

void ChannelGroupStack::setAutomationPlayheadProvider (
    AutomationLaneStackView::PlayheadProvider provider)
{
    automationPlayheadProvider_ = provider;
    for (const auto& g : groups_)
        if (g != nullptr)
            g->setAutomationPlayheadProvider (provider);
}

void ChannelGroupStack::refreshAutomationTransform()
{
    for (const auto& g : groups_)
        if (g != nullptr)
            g->refreshAutomationTransform();
}

void ChannelGroupStack::setEditDispatcher (Daw::EditCommandDispatcher* dispatcher)
{
    for (const auto& g : groups_)
        if (g != nullptr)
            g->setEditDispatcher (dispatcher);
}

void ChannelGroupStack::resized()
{
    auto bounds = getLocalBounds();
    for (const auto& g : groups_)
        g->setBounds (bounds.removeFromTop (g->getPreferredHeight()));
}

void ChannelGroupStack::paint (juce::Graphics& g)
{
    g.fillAll (kBodyBg);
}

void ChannelGroupStack::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == tracks_ && child.hasType (DawIDs::track))
        rebuildGroups();
    else if (parent == dawBranch_ && child.hasType (DawIDs::tracks))
    {
        // The tracks container itself was (re)added — re-observe it.
        tracks_ = child;
        rebuildGroups();
    }
}

void ChannelGroupStack::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == tracks_ && child.hasType (DawIDs::track))
        rebuildGroups();
}

void ChannelGroupStack::notifyContentHeightChanged()
{
    if (onContentHeightChanged)
        onContentHeightChanged();
}

} // namespace Daw
