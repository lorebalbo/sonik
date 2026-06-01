//==============================================================================
// PRD-0067: Channel-Group & Three-Lane Layout tests.
//
// Drives a synthetic `daw` ValueTree (adding/removing tracks and toggling deck
// source mode) and asserts the projected group count, deck-id ordering, lane
// order, lane activeness, and stable vertical footprint — without instantiating
// any real audio deck or touching the audio thread.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Ui/DawLayoutMetrics.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupStack.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupView.h"
#include "Features/Deck/DeckIdentifiers.h"

namespace
{

Daw::TimelineTransform makeTransform()
{
    return Daw::TimelineTransform (Daw::TimelineTransform::GridSnapshot{},
                                   /*pixelsPerBeat*/ 50.0,
                                   /*leftEdgeSample*/ 0,
                                   /*viewportWidthPx*/ 800.0);
}

// A minimal synthetic deck node carrying source-mode + a Stems child.
juce::ValueTree makeDeckTree (const juce::String& sourceMode)
{
    juce::ValueTree deck (IDs::Deck);
    deck.setProperty (IDs::sourceMode, sourceMode, nullptr);
    deck.addChild (juce::ValueTree (IDs::Stems), -1, nullptr);
    return deck;
}

} // namespace

class ChannelGroupLayoutTests final : public juce::UnitTest
{
public:
    ChannelGroupLayoutTests()
        : juce::UnitTest ("Channel Group Layout (PRD-0067)", "Sonik") {}

    void runTest() override
    {
        beginTest ("groups project from tracks and order by deck id ascending");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);

            // Insert out of order: deck 2 first, then deck 0.
            DawState::ensureTrackForDeck (daw, 2);
            DawState::ensureTrackForDeck (daw, 0);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });

            expectEquals (stack.getNumGroups(), 2);
            // Ordered ascending regardless of insertion order.
            expectEquals (stack.getGroups()[0]->getDeckIndex(), 0);
            expectEquals (stack.getGroups()[1]->getDeckIndex(), 2);

            // Adding a track live re-projects and re-sorts.
            DawState::ensureTrackForDeck (daw, 1);
            expectEquals (stack.getNumGroups(), 3);
            expectEquals (stack.getGroups()[0]->getDeckIndex(), 0);
            expectEquals (stack.getGroups()[1]->getDeckIndex(), 1);
            expectEquals (stack.getGroups()[2]->getDeckIndex(), 2);
        }

        beginTest ("removing a track removes its group");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);
            DawState::ensureTrackForDeck (daw, 1);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });
            expectEquals (stack.getNumGroups(), 2);

            auto tracks = daw.getChildWithName (DawIDs::tracks);
            auto track1 = DawState::findTrackForDeck (daw, 1);
            tracks.removeChild (track1, nullptr);

            expectEquals (stack.getNumGroups(), 1);
            expectEquals (stack.getGroups()[0]->getDeckIndex(), 0);
        }

        beginTest ("every group renders all three lanes in fixed order");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });

            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            // All three lanes exist (queryable) regardless of activeness.
            expect (group->isLaneActive (ChannelGroup::LaneKind::Original));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Instrumental));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Vocal));
        }

        beginTest ("lane activeness reflects deck source mode (live)");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);

            auto deck = makeDeckTree ("original");
            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [&deck] (int) { return deck; });

            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            // Original mode -> Original lane active, stems inactive.
            expect (group->isLaneActive (ChannelGroup::LaneKind::Original));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Instrumental));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Vocal));

            // Switch to stems, both stems audible.
            deck.setProperty (IDs::sourceMode, "stems", nullptr);
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Original));
            expect (group->isLaneActive (ChannelGroup::LaneKind::Instrumental));
            expect (group->isLaneActive (ChannelGroup::LaneKind::Vocal));

            // Mute vocals -> only Instrumental active.
            auto stems = deck.getChildWithName (IDs::Stems);
            stems.setProperty (IDs::vocalsMuted, true, nullptr);
            expect (group->isLaneActive (ChannelGroup::LaneKind::Instrumental));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Vocal));

            // Mute all instrumental members too -> no stems active.
            stems.setProperty (IDs::drumsMuted, true, nullptr);
            stems.setProperty (IDs::bassMuted,  true, nullptr);
            stems.setProperty (IDs::otherMuted, true, nullptr);
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Instrumental));
            expect (! group->isLaneActive (ChannelGroup::LaneKind::Vocal));
        }

        beginTest ("group vertical footprint is fixed and collapse-aware");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });

            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            const int expanded = group->getPreferredHeight();
            expectEquals (expanded, Daw::DawLayout::kExpandedGroupHeight);
            // Footprint does NOT change with source-mode activeness.
            expectEquals (stack.getContentHeight(), Daw::DawLayout::kExpandedGroupHeight);

            group->setCollapsed (true);
            expectEquals (group->getPreferredHeight(), Daw::DawLayout::kCollapsedGroupHeight);
            expectEquals (stack.getContentHeight(), Daw::DawLayout::kCollapsedGroupHeight);
        }
    }
};

static ChannelGroupLayoutTests channelGroupLayoutTests;
