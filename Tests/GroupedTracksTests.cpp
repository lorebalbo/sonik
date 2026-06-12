//==============================================================================
// Grouped-tracks tests: mute/solo audibility (model + compiler), the M / S
// header toggles' ValueTree contract, the collapsed-group ghost overview
// strip, and the relocated deck automation lane (below the header, source-lane
// height, visible while collapsed).
//
// Everything drives synthetic ValueTrees on the message thread; no audio
// thread, no real decks.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Model/MuteSolo.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Ui/DawLayoutMetrics.h"
#include "Features/Daw/Automation/AutomationModel.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupStack.h"
#include "Features/Daw/Ui/Organisms/ChannelGroupView.h"
#include "Features/Deck/DeckIdentifiers.h"   // IDs::SonikState

namespace
{

Daw::TimelineTransform makeTransform()
{
    return Daw::TimelineTransform (Daw::TimelineTransform::GridSnapshot{},
                                   /*pixelsPerBeat*/ 50.0,
                                   /*leftEdgeSample*/ 0,
                                   /*viewportWidthPx*/ 800.0);
}

// Appends a 1-second clip to the given lane of a track, starting at
// `timelineStart` (project-rate samples). Returns the clip node.
juce::ValueTree addClip (juce::ValueTree track, ChannelGroup::LaneKind kind,
                         std::int64_t timelineStart)
{
    auto lane = ChannelGroup::findLane (track, kind);

    DawClip clip;
    clip.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
    clip.sourceFileId        = "test-source";
    clip.sourceStartSample   = 0;
    clip.sourceEndSample     = 44100;
    clip.sourceLengthSamples = 44100;
    clip.timelineStartSample = timelineStart;

    auto node = DawClip::toValueTree (clip);
    lane.getChildWithName (DawIDs::clips).addChild (node, -1, nullptr);
    return node;
}

// Total clips admitted into the compiled snapshot.
int compiledClipCount (const juce::ValueTree& daw)
{
    Daw::ArrangementCompiler compiler;
    Daw::ArrangementSnapshot snap;
    compiler.compile (daw, snap);
    return snap.totalClipCount();
}

} // namespace

class GroupedTracksTests final : public juce::UnitTest
{
public:
    GroupedTracksTests() : juce::UnitTest ("Grouped Tracks (mute/solo + ghost)", "Sonik") {}

    void runTest() override
    {
        //----------------------------------------------------------------------
        beginTest ("audibility rules: mute wins, solo scope is global");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            auto t0   = DawState::ensureTrackForDeck (daw, 0);
            auto t1   = DawState::ensureTrackForDeck (daw, 1);

            auto l0 = ChannelGroup::findLane (t0, ChannelGroup::LaneKind::Original);
            auto l1 = ChannelGroup::findLane (t1, ChannelGroup::LaneKind::Original);

            // No flags anywhere: everything audible, no solo active.
            expect (! Daw::MuteSolo::anySoloActive (daw));
            expect (Daw::MuteSolo::isLaneAudible (t0, l0, false));

            // Lane solo on deck 0 silences deck 1 (global scope).
            l0.setProperty (DawIDs::solo, true, nullptr);
            expect (Daw::MuteSolo::anySoloActive (daw));
            expect (Daw::MuteSolo::isLaneAudible (t0, l0, true));
            expect (! Daw::MuteSolo::isLaneAudible (t1, l1, true));

            // Group solo on deck 1 brings its lanes back.
            t1.setProperty (DawIDs::solo, true, nullptr);
            expect (Daw::MuteSolo::isLaneAudible (t1, l1, true));

            // Mute always wins, even over an active solo on the same lane.
            l0.setProperty (DawIDs::muted, true, nullptr);
            expect (! Daw::MuteSolo::isLaneAudible (t0, l0, true));

            // Group mute silences a lane with no flags of its own.
            t1.setProperty (DawIDs::muted, true, nullptr);
            expect (! Daw::MuteSolo::isLaneAudible (t1, l1, true));
        }

        //----------------------------------------------------------------------
        beginTest ("compiler: muted / un-soloed lanes never reach the snapshot");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            auto t0   = DawState::ensureTrackForDeck (daw, 0);
            auto t1   = DawState::ensureTrackForDeck (daw, 1);

            addClip (t0, ChannelGroup::LaneKind::Original,     0);
            addClip (t0, ChannelGroup::LaneKind::Instrumental, 0);
            addClip (t1, ChannelGroup::LaneKind::Vocal,        0);

            expectEquals (compiledClipCount (daw), 3);

            // Group mute drops BOTH of deck 0's clips; deck 1 unaffected.
            t0.setProperty (DawIDs::muted, true, nullptr);
            expectEquals (compiledClipCount (daw), 1);
            t0.setProperty (DawIDs::muted, false, nullptr);

            // Lane mute drops just that lane.
            auto inst = ChannelGroup::findLane (t0, ChannelGroup::LaneKind::Instrumental);
            inst.setProperty (DawIDs::muted, true, nullptr);
            expectEquals (compiledClipCount (daw), 2);
            inst.setProperty (DawIDs::muted, false, nullptr);

            // Lane solo isolates exactly that lane across all decks.
            inst.setProperty (DawIDs::solo, true, nullptr);
            expectEquals (compiledClipCount (daw), 1);

            // Adding a group solo on deck 1 re-admits its vocal clip too.
            t1.setProperty (DawIDs::solo, true, nullptr);
            expectEquals (compiledClipCount (daw), 2);
        }

        //----------------------------------------------------------------------
        beginTest ("lane dimming follows mute/solo through the stack");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            auto t0   = DawState::ensureTrackForDeck (daw, 0);
            DawState::ensureTrackForDeck (daw, 1);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });

            auto* g0 = stack.getGroupByDeckIndex (0);
            auto* g1 = stack.getGroupByDeckIndex (1);
            expect (g0 != nullptr && g1 != nullptr);

            // Soloing deck 0's vocal lane (via the property the S button writes)
            // dims everything else on BOTH groups — no manual refresh call: the
            // stack observes the flags itself.
            auto vocal = ChannelGroup::findLane (t0, ChannelGroup::LaneKind::Vocal);
            vocal.setProperty (DawIDs::solo, true, nullptr);

            expect (g0->isLaneAudibleForTests (ChannelGroup::LaneKind::Vocal));
            expect (! g0->isLaneAudibleForTests (ChannelGroup::LaneKind::Original));
            expect (! g1->isLaneAudibleForTests (ChannelGroup::LaneKind::Original));

            vocal.setProperty (DawIDs::solo, false, nullptr);
            expect (g1->isLaneAudibleForTests (ChannelGroup::LaneKind::Original));
        }

        //----------------------------------------------------------------------
        beginTest ("ghost overview: collapsed group shows per-lane clip lines");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            auto t0   = DawState::ensureTrackForDeck (daw, 0);

            addClip (t0, ChannelGroup::LaneKind::Original, 0);
            addClip (t0, ChannelGroup::LaneKind::Vocal,    44100);

            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); });
            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            stack.layoutToContentHeight (800);

            // Hidden while expanded; shown when the group folds.
            expect (! group->getOverviewStrip().isVisible());
            group->setCollapsed (true);
            expect (group->getOverviewStrip().isVisible());

            // The strip overlays the header's timeline area (right of the gutter).
            const auto stripBounds = group->getOverviewStrip().getBounds();
            expectEquals (stripBounds.getX(), Daw::DawLayout::kTrackHeaderWidth);
            expect (stripBounds.getBottom() <= Daw::DawLayout::kGroupHeaderHeight);

            // Two segments on the expected rows, at the transform-mapped x.
            auto segments = group->getOverviewStrip().computeSegments();
            expectEquals ((int) segments.size(), 2);
            expectEquals (segments[0].laneRow, 0); // Original
            expectEquals (segments[1].laneRow, 2); // Vocal
            expectWithinAbsoluteError (segments[0].x0,
                                       (float) transform.sampleToX (0), 1.0f);
            expectWithinAbsoluteError (segments[1].x0,
                                       (float) transform.sampleToX (44100), 1.0f);
            expect (segments[0].audible && segments[1].audible);

            // Muting the vocal lane dims its ghost line (audio-faithful view).
            auto vocal = ChannelGroup::findLane (t0, ChannelGroup::LaneKind::Vocal);
            vocal.setProperty (DawIDs::muted, true, nullptr);
            segments = group->getOverviewStrip().computeSegments();
            expectEquals ((int) segments.size(), 2);
            expect (segments[0].audible);
            expect (! segments[1].audible);
        }

        //----------------------------------------------------------------------
        beginTest ("deck automation lane: below header, lane-height, collapse-proof");
        {
            auto root = juce::ValueTree (IDs::SonikState);
            auto daw  = DawState::getOrCreateDawBranch (root);
            DawState::ensureTrackForDeck (daw, 0);

            Daw::AutomationModel model (daw);
            auto transform = makeTransform();
            Daw::ChannelGroupStack stack (daw, transform,
                                          [] (int) { return juce::ValueTree(); },
                                          {}, &model);
            auto* group = stack.getGroupByDeckIndex (0);
            expect (group != nullptr);

            // Reveal one parameter: the lane is a full source-lane-height row.
            group->setAutomationParameter ("gain");
            auto* autoStack = group->getAutomationStack();
            expect (autoStack != nullptr);
            expectEquals (autoStack->getLaneRowHeight(), Daw::DawLayout::kLaneHeight);
            expectEquals (autoStack->getPreferredHeight(), Daw::DawLayout::kLaneHeight);
            expectEquals (group->getPreferredHeight(),
                          Daw::DawLayout::kExpandedGroupHeight + Daw::DawLayout::kLaneHeight);

            // Laid out DIRECTLY below the DECK header, pushing the source lanes down.
            stack.layoutToContentHeight (800);
            expectEquals (autoStack->getY(), Daw::DawLayout::kGroupHeaderHeight);

            // Collapsing keeps the automation lane visible in the same slot.
            group->setCollapsed (true);
            expect (autoStack->isVisible(),
                    "revealed automation lane survives a group collapse");
            expectEquals (group->getPreferredHeight(),
                          Daw::DawLayout::kCollapsedGroupHeight + Daw::DawLayout::kLaneHeight);
            stack.layoutToContentHeight (800);
            expectEquals (autoStack->getY(), Daw::DawLayout::kGroupHeaderHeight);

            // Turning the parameter OFF removes the row in either state.
            group->setAutomationParameter ({});
            expectEquals (group->getPreferredHeight(), Daw::DawLayout::kCollapsedGroupHeight);
        }
    }
};

static GroupedTracksTests groupedTracksTests;
