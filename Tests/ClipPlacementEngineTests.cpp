#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Recording/ClipPlacementEngine.h"
#include "Features/Daw/Recording/PerformanceEventFifo.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/State/DawClipModel.h"
#include "Features/Daw/Model/ChannelGroup.h"

#include <map>
#include <vector>

namespace
{

using namespace Daw;

// Configurable per-deck source state.
struct StubSourceProvider final : public DeckSourceProvider
{
    std::map<int, std::int64_t> pos;
    std::map<int, juce::String> file;
    std::map<int, std::int64_t> length;

    std::int64_t getSourceSample (int deck) const override
    {
        auto it = pos.find (deck);
        return it != pos.end() ? it->second : 0;
    }
    juce::String getSourceFileId (int deck) const override
    {
        auto it = file.find (deck);
        return it != file.end() ? it->second : juce::String();
    }
    std::int64_t getSourceLength (int deck) const override
    {
        auto it = length.find (deck);
        return it != length.end() ? it->second : 0;
    }
};

// Returns a fixed set of lane ids per deck (single Original lane by default).
struct StubLaneResolver final : public LaneResolver
{
    std::map<int, std::vector<juce::Uuid>> lanesByDeck;
    std::vector<juce::Uuid> resolveLanes (int deck) const override
    {
        auto it = lanesByDeck.find (deck);
        return it != lanesByDeck.end() ? it->second : std::vector<juce::Uuid>{};
    }
};

PerformanceEvent ev (PerformanceEventType type, int deck, std::int64_t srcPos)
{
    PerformanceEvent e;
    e.type                 = type;
    e.deckIndex            = static_cast<std::uint8_t> (deck);
    e.sourceSamplePosition = srcPos;
    return e;
}

juce::Uuid originalLaneId (juce::ValueTree dawBranch, int deck)
{
    auto track = DawState::ensureTrackForDeck (dawBranch, deck);
    auto lane  = ChannelGroup::findLane (track, ChannelGroup::LaneKind::Original);
    return juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
}

juce::Uuid laneIdFor (juce::ValueTree dawBranch, int deck, ChannelGroup::LaneKind kind)
{
    auto track = DawState::ensureTrackForDeck (dawBranch, deck);
    auto lane  = ChannelGroup::findLane (track, kind);
    return juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
}

// All clip nodes for a deck's lane, in tree order.
std::vector<juce::ValueTree> clipsFor (juce::ValueTree dawBranch, int deck, const juce::Uuid& laneId)
{
    std::vector<juce::ValueTree> out;
    auto track = DawState::findTrackForDeck (dawBranch, deck);
    if (! track.isValid()) return out;
    auto lanes = track.getChildWithName (DawIDs::lanes);
    for (int i = 0; i < lanes.getNumChildren(); ++i)
    {
        auto lane = lanes.getChild (i);
        if (lane.getProperty (DawIDs::laneId).toString() != laneId.toString())
            continue;
        auto clips = lane.getChildWithName (DawIDs::clips);
        for (int j = 0; j < clips.getNumChildren(); ++j)
            if (clips.getChild (j).hasType (DawIDs::clip))
                out.push_back (clips.getChild (j));
    }
    return out;
}

std::int64_t prop64 (const juce::ValueTree& node, const juce::Identifier& id)
{
    return static_cast<std::int64_t> (node.getProperty (id));
}

} // namespace

//==============================================================================
class ClipPlacementEngineTests : public juce::UnitTest
{
public:
    ClipPlacementEngineTests()
        : juce::UnitTest ("Clip Placement Engine", "Sonik") {}

    void runTest() override
    {
        beginTest ("Single open -> grow -> close produces one correct clip");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0]    = 5000;
            src.file[0]   = "fileA";
            src.length[0] = 1'000'000;

            const auto lane = originalLaneId (daw, 0);
            StubLaneResolver lanes;
            lanes.lanesByDeck[0] = { lane };

            std::int64_t playhead = 1000;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 5000));
            expectEquals (engine.numOpenSlots(), 1);

            // Deck advances; grow tracks the live source position.
            src.pos[0] = 5500;
            engine.grow();
            src.pos[0] = 6000;
            engine.grow();

            engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 6200));
            expectEquals (engine.numOpenSlots(), 0);

            auto clips = clipsFor (daw, 0, lane);
            expectEquals ((int) clips.size(), 1);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceStartSample),   (std::int64_t) 5000);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample),     (std::int64_t) 6200);
            expectEquals (prop64 (clips[0], DawClipIDs::timelineStartSample), (std::int64_t) 1000);
            expectEquals (clips[0].getProperty (DawClipIDs::sourceFileId).toString(), juce::String ("fileA"));
            expectEquals (clips[0].getProperty (DawClipIDs::laneId).toString(), lane.toString());
        }

        beginTest ("grow() advances monotonically and never moves backwards");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0] = 1000;
            const auto lane = originalLaneId (daw, 0);
            StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };
            std::int64_t playhead = 0;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
            src.pos[0] = 2000; engine.grow();
            auto clips = clipsFor (daw, 0, lane);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample), (std::int64_t) 2000);

            // A backwards source reading must not retract the crop end.
            src.pos[0] = 1500; engine.grow();
            clips = clipsFor (daw, 0, lane);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample), (std::int64_t) 2000);

            src.pos[0] = 3000; engine.grow();
            clips = clipsFor (daw, 0, lane);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample), (std::int64_t) 3000);
        }

        beginTest ("Mid-playback split (JumpOut + JumpIn) produces two contiguous clips");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0]    = 5000;
            src.file[0]   = "fileA";
            const auto lane = originalLaneId (daw, 0);
            StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

            std::int64_t playhead = 1000;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 5000));

            // Jump fires: out at source 5500 (pre-jump), in at source 8000 (post-jump).
            // Both drained in the same pass -> same playhead snapshot.
            playhead = 1500;
            engine.onEvent (ev (PerformanceEventType::CueJumpOut, 0, 5500));
            engine.onEvent (ev (PerformanceEventType::CueJumpIn,  0, 8000));
            expectEquals (engine.numOpenSlots(), 1);

            src.pos[0] = 8400;
            engine.grow();
            engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 8600));

            auto clips = clipsFor (daw, 0, lane);
            expectEquals ((int) clips.size(), 2);

            const auto c0Start = prop64 (clips[0], DawClipIDs::timelineStartSample);
            const auto c0Len   = prop64 (clips[0], DawClipIDs::sourceEndSample)
                               - prop64 (clips[0], DawClipIDs::sourceStartSample);
            const auto c1Start = prop64 (clips[1], DawClipIDs::timelineStartSample);

            // Contiguity: clip 2 starts exactly where clip 1 ends on the timeline.
            expectEquals (c1Start, c0Start + c0Len);

            // Source discontinuity is real: clip 2 starts at the jumped-to position.
            expectEquals (prop64 (clips[1], DawClipIDs::sourceStartSample), (std::int64_t) 8000);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample),   (std::int64_t) 5500);
        }

        beginTest ("Concurrent two-deck capture writes independent clips");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0] = 100; src.file[0] = "A";
            src.pos[1] = 900; src.file[1] = "B";

            const auto laneA = originalLaneId (daw, 0);
            const auto laneB = originalLaneId (daw, 1);
            StubLaneResolver lanes;
            lanes.lanesByDeck[0] = { laneA };
            lanes.lanesByDeck[1] = { laneB };

            std::int64_t playhead = 0;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 100));
            engine.onEvent (ev (PerformanceEventType::DeckPlay, 1, 900));
            expectEquals (engine.numOpenSlots(), 2);

            src.pos[0] = 300; src.pos[1] = 1500;
            engine.grow();

            // Stopping deck 0 must not affect deck 1's open clip.
            engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 350));
            expectEquals (engine.numOpenSlots(), 1);

            src.pos[1] = 1800; engine.grow();
            engine.onEvent (ev (PerformanceEventType::DeckStop, 1, 1850));

            auto a = clipsFor (daw, 0, laneA);
            auto b = clipsFor (daw, 1, laneB);
            expectEquals ((int) a.size(), 1);
            expectEquals ((int) b.size(), 1);
            expectEquals (prop64 (a[0], DawClipIDs::sourceEndSample), (std::int64_t) 350);
            expectEquals (prop64 (b[0], DawClipIDs::sourceEndSample), (std::int64_t) 1850);
        }

        beginTest ("finaliseAll closes still-open clips at the deck source position");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0] = 2000;
            const auto lane = originalLaneId (daw, 0);
            StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };
            std::int64_t playhead = 500;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 2000));
            src.pos[0] = 2600; engine.grow();

            src.pos[0] = 2700;            // current deck source position at stop
            engine.finaliseAll (9999);
            expectEquals (engine.numOpenSlots(), 0);

            auto clips = clipsFor (daw, 0, lane);
            expectEquals ((int) clips.size(), 1);
            expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample), (std::int64_t) 2700);
        }

        beginTest ("Zero-length clip is discarded, not persisted");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0] = 4000;
            const auto lane = originalLaneId (daw, 0);
            StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };
            std::int64_t playhead = 0;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            // Open and immediately close at the same source position -> length 0.
            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 4000));
            engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 4000));

            auto clips = clipsFor (daw, 0, lane);
            expectEquals ((int) clips.size(), 0);
        }

        beginTest ("Multi-lane (stems) open creates one clip per resolved lane");
        {
            juce::ValueTree daw (DawIDs::Daw);
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

            StubSourceProvider src;
            src.pos[0] = 0; src.file[0] = "stemly";
            const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
            const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);
            StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst, voc };
            std::int64_t playhead = 0;
            ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

            engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 0));
            expectEquals (engine.numOpenSlots(), 2);

            src.pos[0] = 1000; engine.grow();
            engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 1000));

            expectEquals ((int) clipsFor (daw, 0, inst).size(), 1);
            expectEquals ((int) clipsFor (daw, 0, voc).size(), 1);
        }
    }
};

static ClipPlacementEngineTests clipPlacementEngineTests;
