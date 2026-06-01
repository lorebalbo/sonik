//==============================================================================
// PRD-0063: DawStateSchemaTests — tree shape, lane pre-creation, and
// ensureTrackForDeck idempotency. JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"

class DawStateSchemaTests final : public juce::UnitTest
{
public:
    DawStateSchemaTests() : juce::UnitTest ("DAW State Schema", "Sonik") {}

    void runTest() override
    {
        testBranchAttachIdempotent();
        testTreeShape();
        testLanePreCreation();
        testEnsureTrackIdempotency();
        testTracksLazyLanesEager();
    }

private:
    static juce::ValueTree makeRootWithDaw()
    {
        juce::ValueTree root ("SonikState");
        return DawState::getOrCreateDawBranch (root);
    }

    void testBranchAttachIdempotent()
    {
        beginTest ("Daw branch get-or-create is idempotent");

        juce::ValueTree root ("SonikState");
        auto daw1 = DawState::getOrCreateDawBranch (root);
        auto daw2 = DawState::getOrCreateDawBranch (root);

        expect (daw1.isValid());
        expect (daw1.hasType (DawIDs::Daw));
        expectEquals (root.getNumChildren(), 1);
        expect (daw1 == daw2, "same branch reused");
        expect (daw1.getChildWithName (DawIDs::tracks).isValid(), "tracks container exists");
        expectEquals (daw1.getChildWithName (DawIDs::tracks).getNumChildren(), 0);
    }

    void testTreeShape()
    {
        beginTest ("daw -> tracks -> track -> lanes -> lane -> clips");

        auto daw = makeRootWithDaw();
        auto track = DawState::ensureTrackForDeck (daw, 0);

        expect (track.hasType (DawIDs::track));
        expectEquals (static_cast<int> (track.getProperty (DawIDs::deckIndex)), 0);

        auto lanes = track.getChildWithName (DawIDs::lanes);
        expect (lanes.isValid(), "lanes container exists");
        expectEquals (lanes.getNumChildren(), ChannelGroup::kLaneCount);

        auto lane = lanes.getChild (0);
        expect (lane.hasType (DawIDs::lane));
        expect (lane.getChildWithName (DawIDs::clips).isValid(), "clips container exists");
        expectEquals (lane.getChildWithName (DawIDs::clips).getNumChildren(), 0);
    }

    void testLanePreCreation()
    {
        beginTest ("Three lanes pre-created with correct kinds and distinct ids");

        auto daw = makeRootWithDaw();
        auto track = DawState::ensureTrackForDeck (daw, 1);

        auto original = ChannelGroup::findLane (track, ChannelGroup::LaneKind::Original);
        auto instr    = ChannelGroup::findLane (track, ChannelGroup::LaneKind::Instrumental);
        auto vocal    = ChannelGroup::findLane (track, ChannelGroup::LaneKind::Vocal);

        expect (original.isValid() && instr.isValid() && vocal.isValid());

        const auto idO = original.getProperty (DawIDs::laneId).toString();
        const auto idI = instr.getProperty (DawIDs::laneId).toString();
        const auto idV = vocal.getProperty (DawIDs::laneId).toString();

        expect (idO.isNotEmpty() && idI.isNotEmpty() && idV.isNotEmpty(), "laneIds minted");
        expect (juce::Uuid (idO) != juce::Uuid::null(), "laneId is a real Uuid");
        expect (idO != idI && idI != idV && idO != idV, "laneIds are distinct");

        expectEquals (original.getProperty (DawIDs::laneKind).toString(), juce::String ("Original"));
        expectEquals (instr.getProperty    (DawIDs::laneKind).toString(), juce::String ("Instrumental"));
        expectEquals (vocal.getProperty    (DawIDs::laneKind).toString(), juce::String ("Vocal"));
    }

    void testEnsureTrackIdempotency()
    {
        beginTest ("ensureTrackForDeck twice == one track, three lanes, no clip dup");

        auto daw = makeRootWithDaw();
        auto track1 = DawState::ensureTrackForDeck (daw, 2);

        // Add a clip to the Original lane so we can prove it survives a 2nd call.
        auto lane = ChannelGroup::findLane (track1, ChannelGroup::LaneKind::Original);
        auto clipsContainer = lane.getChildWithName (DawIDs::clips);

        DawClip clip;
        clip.laneId       = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
        clip.sourceFileId = "library:track-42";
        clipsContainer.addChild (DawClip::toValueTree (clip), -1, nullptr);
        const auto clipId = clip.clipId;

        auto track2 = DawState::ensureTrackForDeck (daw, 2);

        expect (track1 == track2, "same track returned");
        expectEquals (daw.getChildWithName (DawIDs::tracks).getNumChildren(), 1);
        expectEquals (track2.getChildWithName (DawIDs::lanes).getNumChildren(), 3);

        auto laneAfter = ChannelGroup::findLane (track2, ChannelGroup::LaneKind::Original);
        auto clipsAfter = laneAfter.getChildWithName (DawIDs::clips);
        expectEquals (clipsAfter.getNumChildren(), 1, "existing clip not duplicated/reset");
        expect (DawClipModel::findClipNodeById (clipsAfter, clipId).isValid(),
                "clip still resolvable by id");
    }

    void testTracksLazyLanesEager()
    {
        beginTest ("Tracks lazy at branch creation; lanes eager within a track");

        auto daw = makeRootWithDaw();
        expectEquals (daw.getChildWithName (DawIDs::tracks).getNumChildren(), 0,
                      "no tracks until a deck acquires a source");

        DawState::ensureTrackForDeck (daw, 3);
        auto track = DawState::findTrackForDeck (daw, 3);
        expect (track.isValid());
        expectEquals (track.getChildWithName (DawIDs::lanes).getNumChildren(), 3,
                      "all three lanes eagerly present");
    }
};

static DawStateSchemaTests dawStateSchemaTests;
