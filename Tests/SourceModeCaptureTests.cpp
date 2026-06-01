#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Recording/ClipPlacementEngine.h"
#include "Features/Daw/Recording/PerformanceEventFifo.h"
#include "Features/Daw/Recording/PerformanceCaptureSink.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/State/DawClipModel.h"
#include "Features/Daw/Model/ChannelGroup.h"

#include <map>
#include <vector>

namespace
{

using namespace Daw;

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

// Mutable so tests can flip the deck's active lane set between events.
struct StubLaneResolver final : public LaneResolver
{
    std::map<int, std::vector<juce::Uuid>> lanesByDeck;
    std::vector<juce::Uuid> resolveLanes (int deck) const override
    {
        auto it = lanesByDeck.find (deck);
        return it != lanesByDeck.end() ? it->second : std::vector<juce::Uuid>{};
    }
};

// Counts resolver invocations to prove the alignment seam is re-run per open.
struct CountingSeam final : public ClipAlignmentSeam
{
    mutable int calls = 0;
    AlignmentResult resolveOpen (int, std::int64_t rawPlayhead, std::int64_t) const override
    {
        ++calls;
        return { rawPlayhead, AlignmentMode::FirstBeatAnchored };
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

juce::Uuid laneIdFor (juce::ValueTree dawBranch, int deck, ChannelGroup::LaneKind kind)
{
    auto track = DawState::ensureTrackForDeck (dawBranch, deck);
    auto lane  = ChannelGroup::findLane (track, kind);
    return juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
}

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

juce::ValueTree makeDaw()
{
    juce::ValueTree daw (DawIDs::Daw);
    daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);
    return daw;
}

struct CapturingHandler final : public PerformanceEventHandler
{
    std::vector<PerformanceEvent> events;
    void onPerformanceEvent (const PerformanceEvent& e) override { events.push_back (e); }
};

} // namespace

//==============================================================================
class SourceModeCaptureTests : public juce::UnitTest
{
public:
    SourceModeCaptureTests() : juce::UnitTest ("Source-Mode Capture", "Sonik") {}

    void runTest() override
    {
        testSingleLaneSwap();
        testOneToTwoMultiLaneOpen();
        testTwoToOneMultiLaneClose();
        testSharedLaneReOpen();
        testBothStemsMutedSilenceGap();
        testChangeOnStoppedDeckMutatesNothing();
        testResolverReRunPerOpen();
        testFifoSinkSourceModeEvent();
    }

    //--------------------------------------------------------------------------
    // Original -> single stem (Instrumental): one close, one open, contiguous.
    void testSingleLaneSwap()
    {
        beginTest ("Single-lane swap closes outgoing and opens incoming, contiguous");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto orig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { orig };
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        playhead = 4000; // 1:1 with source advance (1000->5000)

        lanes.lanesByDeck[0] = { inst };                 // flip to stems/Instrumental
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));

        auto origClips = clipsFor (daw, 0, orig);
        auto instClips = clipsFor (daw, 0, inst);
        expectEquals ((int) origClips.size(), 1);
        expectEquals ((int) instClips.size(), 1);

        expectEquals (prop64 (origClips[0], DawClipIDs::sourceEndSample), (std::int64_t) 5000);
        expectEquals (prop64 (instClips[0], DawClipIDs::sourceStartSample), (std::int64_t) 5000);
        expectEquals (prop64 (instClips[0], DawClipIDs::timelineStartSample), (std::int64_t) 4000);

        // Contiguity: incoming start == outgoing implicit timeline end.
        const auto origTlEnd = prop64 (origClips[0], DawClipIDs::timelineStartSample)
                             + (prop64 (origClips[0], DawClipIDs::sourceEndSample)
                                - prop64 (origClips[0], DawClipIDs::sourceStartSample));
        expectEquals (prop64 (instClips[0], DawClipIDs::timelineStartSample), origTlEnd);
    }

    //--------------------------------------------------------------------------
    // original -> stems (both audible): one close, two contiguous opens.
    void testOneToTwoMultiLaneOpen()
    {
        beginTest ("original -> both stems: one close, two contiguous opens");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto orig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { orig };
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        playhead = 4000;

        lanes.lanesByDeck[0] = { inst, voc };
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));

        expectEquals ((int) clipsFor (daw, 0, orig).size(), 1);
        auto instClips = clipsFor (daw, 0, inst);
        auto vocClips  = clipsFor (daw, 0, voc);
        expectEquals ((int) instClips.size(), 1);
        expectEquals ((int) vocClips.size(), 1);
        expectEquals (prop64 (instClips[0], DawClipIDs::timelineStartSample), (std::int64_t) 4000);
        expectEquals (prop64 (vocClips[0],  DawClipIDs::timelineStartSample), (std::int64_t) 4000);
        expectEquals (prop64 (instClips[0], DawClipIDs::sourceStartSample), (std::int64_t) 5000);
        expectEquals (prop64 (vocClips[0],  DawClipIDs::sourceStartSample), (std::int64_t) 5000);
    }

    //--------------------------------------------------------------------------
    // stems (both audible) -> original: two closes, one open.
    void testTwoToOneMultiLaneClose()
    {
        beginTest ("both stems -> original: two closes, one open");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto orig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst, voc };
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        playhead = 4000;

        lanes.lanesByDeck[0] = { orig };
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));

        expectEquals ((int) clipsFor (daw, 0, inst).size(), 1);
        expectEquals ((int) clipsFor (daw, 0, voc).size(),  1);
        auto origClips = clipsFor (daw, 0, orig);
        expectEquals ((int) origClips.size(), 1);
        expectEquals (prop64 (origClips[0], DawClipIDs::timelineStartSample), (std::int64_t) 4000);
    }

    //--------------------------------------------------------------------------
    // §1.5.1: a lane shared across the change is closed and re-opened fresh.
    void testSharedLaneReOpen()
    {
        beginTest ("Shared lane is closed and re-opened on un-mute");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst };   // VOC muted
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        playhead = 4000;

        lanes.lanesByDeck[0] = { inst, voc };                      // un-mute VOC
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));

        // Instrumental: original clip closed + fresh contiguous clip re-opened.
        auto instClips = clipsFor (daw, 0, inst);
        expectEquals ((int) instClips.size(), 2);
        expectEquals (prop64 (instClips[0], DawClipIDs::sourceEndSample),   (std::int64_t) 5000);
        expectEquals (prop64 (instClips[1], DawClipIDs::sourceStartSample), (std::int64_t) 5000);
        expectEquals (prop64 (instClips[1], DawClipIDs::timelineStartSample), (std::int64_t) 4000);

        // Vocal: one new clip, same boundary.
        auto vocClips = clipsFor (daw, 0, voc);
        expectEquals ((int) vocClips.size(), 1);
        expectEquals (prop64 (vocClips[0], DawClipIDs::timelineStartSample), (std::int64_t) 4000);
    }

    //--------------------------------------------------------------------------
    // §1.5.6: muting both stems closes all clips and opens none (silence gap);
    // a later un-mute re-opens with a true timeline gap.
    void testBothStemsMutedSilenceGap()
    {
        beginTest ("Both stems muted = silence gap; un-mute re-opens after the gap");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst, voc };
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        playhead = 4000;

        // Mute both stems: no lanes.
        lanes.lanesByDeck[0] = {};
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));
        expectEquals (engine.numOpenSlots(), 0);
        expectEquals ((int) clipsFor (daw, 0, inst).size(), 1); // closed clip only
        expectEquals ((int) clipsFor (daw, 0, voc).size(),  1);

        // Silence interval, then un-mute Instrumental at a later position.
        src.pos[0] = 9000; playhead = 8000;
        lanes.lanesByDeck[0] = { inst };
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 9000));

        auto instClips = clipsFor (daw, 0, inst);
        expectEquals ((int) instClips.size(), 2);
        // True gap: the re-opened clip starts after the closed clip's end.
        const auto closedEnd = prop64 (instClips[0], DawClipIDs::timelineStartSample)
                             + (prop64 (instClips[0], DawClipIDs::sourceEndSample)
                                - prop64 (instClips[0], DawClipIDs::sourceStartSample));
        expectEquals (closedEnd, (std::int64_t) 4000);
        expectEquals (prop64 (instClips[1], DawClipIDs::timelineStartSample), (std::int64_t) 8000);
        expect (prop64 (instClips[1], DawClipIDs::timelineStartSample) > closedEnd);
    }

    //--------------------------------------------------------------------------
    // A source-mode change on a deck with no open clip mutates nothing.
    void testChangeOnStoppedDeckMutatesNothing()
    {
        beginTest ("Source-mode change on a stopped deck mutates no clips");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto orig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { orig };
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        // Never played, then stopped — deck is not recording.
        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::DeckStop, 0, 5000));

        lanes.lanesByDeck[0] = { inst };
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000));

        // No Instrumental clip opened; only the original (now closed) clip exists.
        expectEquals ((int) clipsFor (daw, 0, inst).size(), 0);
        expectEquals ((int) clipsFor (daw, 0, orig).size(), 1);
        expectEquals (engine.numOpenSlots(), 0);
    }

    //--------------------------------------------------------------------------
    // §1.5.7: the alignment resolver is re-run for each newly opened clip.
    void testResolverReRunPerOpen()
    {
        beginTest ("Alignment resolver re-runs for each new open");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto orig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes; lanes.lanesByDeck[0] = { orig };
        CountingSeam seam;
        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; }, &seam);

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000)); // open 1
        src.pos[0] = 5000; engine.grow();
        playhead = 4000;

        lanes.lanesByDeck[0] = { inst, voc };
        engine.onEvent (ev (PerformanceEventType::SourceModeChange, 0, 5000)); // open 2 + 3

        // One open at play + two opens at the switch = three resolver calls.
        expectEquals (seam.calls, 3);
    }

    //--------------------------------------------------------------------------
    // FifoPerformanceCaptureSink encodes a source-mode change event.
    void testFifoSinkSourceModeEvent()
    {
        beginTest ("FIFO capture sink encodes source-mode change");

        PerformanceEventFifo fifo;
        bool recording = true;
        FifoPerformanceCaptureSink sink (fifo, [&recording] { return recording; });

        sink.captureSourceModeChange (3, 12345);

        CapturingHandler handler;
        fifo.drain (handler);
        expectEquals ((int) handler.events.size(), 1);
        expect (handler.events[0].type == PerformanceEventType::SourceModeChange);
        expectEquals ((int) handler.events[0].deckIndex, 3);
        expectEquals (handler.events[0].sourceSamplePosition, (std::int64_t) 12345);

        recording = false;
        sink.captureSourceModeChange (3, 99999);
        expectEquals ((int) fifo.getNumReady(), 0);
    }
};

static SourceModeCaptureTests sourceModeCaptureTests;
