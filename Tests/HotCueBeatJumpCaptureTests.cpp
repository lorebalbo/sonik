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

// Configurable per-deck source state (mirrors ClipPlacementEngineTests).
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

struct StubLaneResolver final : public LaneResolver
{
    std::map<int, std::vector<juce::Uuid>> lanesByDeck;
    std::vector<juce::Uuid> resolveLanes (int deck) const override
    {
        auto it = lanesByDeck.find (deck);
        return it != lanesByDeck.end() ? it->second : std::vector<juce::Uuid>{};
    }
};

PerformanceEvent ev (PerformanceEventType type, int deck,
                     std::int64_t srcPos, std::int64_t payload = 0)
{
    PerformanceEvent e;
    e.type                 = type;
    e.deckIndex            = static_cast<std::uint8_t> (deck);
    e.sourceSamplePosition = srcPos;
    e.payload              = payload;
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

// Drains a FIFO into a flat record list for assertions.
struct CapturingHandler final : public PerformanceEventHandler
{
    std::vector<PerformanceEvent> events;
    void onPerformanceEvent (const PerformanceEvent& e) override { events.push_back (e); }
};

} // namespace

//==============================================================================
class HotCueBeatJumpCaptureTests : public juce::UnitTest
{
public:
    HotCueBeatJumpCaptureTests()
        : juce::UnitTest ("Hot-Cue & Beat-Jump Capture", "Sonik") {}

    void runTest() override
    {
        testForwardJumpSplits();
        testBackwardJumpOverlaps();
        testSmallDeltaDoesNotSplit();
        testJumpWithNoOpenClipIsNoOp();
        testHotCueJumpSplits();
        testStemsSplitAllLanes();
        testDegenerateJumpCoalesced();
        testFifoSinkGatingAndShape();
    }

    //--------------------------------------------------------------------------
    // §1.5.1/§1.5.3: a forward beat jump closes the active clip at the out-point
    // and opens a contiguous new clip at the in-point with the same file.
    void testForwardJumpSplits()
    {
        beginTest ("Forward jump splits into two contiguous clips, same file");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 5000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 1000;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 5000));
        src.pos[0] = 50000; playhead = 46000; engine.grow();

        // Forward jump: out=50000, in=200000 (delta 150000 > threshold).
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 50000, /*in*/ 200000));

        auto clips = clipsFor (daw, 0, lane);
        expectEquals ((int) clips.size(), 2);

        const auto endFirst   = prop64 (clips[0], DawClipIDs::sourceEndSample);
        const auto startFirst = prop64 (clips[0], DawClipIDs::sourceStartSample);
        expectEquals (startFirst, (std::int64_t) 5000);
        expectEquals (endFirst,   (std::int64_t) 50000);

        const auto startSecond = prop64 (clips[1], DawClipIDs::sourceStartSample);
        expectEquals (startSecond, (std::int64_t) 200000);

        // Contiguous timeline: second starts where the first ends on the grid.
        const auto tlStartFirst = prop64 (clips[0], DawClipIDs::timelineStartSample);
        const auto tlLenFirst   = endFirst - startFirst;
        const auto tlStartSecond = prop64 (clips[1], DawClipIDs::timelineStartSample);
        expectEquals (tlStartSecond, tlStartFirst + tlLenFirst);

        // Same source file on both sides of the jump.
        expect (clips[0].getProperty (DawClipIDs::sourceFileId).toString()
                == clips[1].getProperty (DawClipIDs::sourceFileId).toString());
    }

    //--------------------------------------------------------------------------
    // §1.2: a backward jump produces overlapping source windows but a forward-
    // only timeline (the new clip still starts after the closed one).
    void testBackwardJumpOverlaps()
    {
        beginTest ("Backward jump: overlapping source, forward-only timeline");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 100000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 100000));
        src.pos[0] = 300000; playhead = 200000; engine.grow();

        // Backward jump: out=300000, in=10000.
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 300000, /*in*/ 10000));

        auto clips = clipsFor (daw, 0, lane);
        expectEquals ((int) clips.size(), 2);

        // Overlapping source windows: second start < first end.
        expect (prop64 (clips[1], DawClipIDs::sourceStartSample)
                < prop64 (clips[0], DawClipIDs::sourceEndSample));

        // Timeline still moves forward.
        expect (prop64 (clips[1], DawClipIDs::timelineStartSample)
                >= prop64 (clips[0], DawClipIDs::timelineStartSample));
    }

    //--------------------------------------------------------------------------
    // §1.5.1: a sub-threshold source delta is a normal advance, not a split.
    void testSmallDeltaDoesNotSplit()
    {
        beginTest ("Sub-threshold source delta does not split");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 5000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 5000));

        // Delta of 500 <= kSourceSplitThresholdSamples (1024): no split.
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 5500, /*in*/ 6000));

        expectEquals ((int) clipsFor (daw, 0, lane).size(), 1);
        expectEquals (engine.numOpenSlots(), 1);
    }

    //--------------------------------------------------------------------------
    // §1.3.5/§1.5.6: a jump while no clip is open writes nothing and errors not.
    void testJumpWithNoOpenClipIsNoOp()
    {
        beginTest ("Jump with no open clip is a no-op");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 5000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        // No DeckPlay: deck paused/muted.
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 5000, /*in*/ 500000));

        expectEquals ((int) clipsFor (daw, 0, lane).size(), 0);
        expectEquals (engine.numOpenSlots(), 0);
    }

    //--------------------------------------------------------------------------
    // A HotCueJump event splits identically to a beat jump.
    void testHotCueJumpSplits()
    {
        beginTest ("Hot-cue jump splits into two clips");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 40000; playhead = 39000; engine.grow();

        engine.onEvent (ev (PerformanceEventType::HotCueJump, 0, /*out*/ 40000, /*in*/ 8000));

        expectEquals ((int) clipsFor (daw, 0, lane).size(), 2);
    }

    //--------------------------------------------------------------------------
    // §1.4: one jump closes every open lane and reopens one clip per lane at the
    // same timeline boundary (stems case).
    void testStemsSplitAllLanes()
    {
        beginTest ("Jump on stem deck splits all open lanes");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 2000; src.file[0] = "fileA"; src.length[0] = 5'000'000;

        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst, voc };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 2000));
        expectEquals (engine.numOpenSlots(), 2);

        src.pos[0] = 30000; playhead = 28000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 30000, /*in*/ 300000));

        auto instClips = clipsFor (daw, 0, inst);
        auto vocClips  = clipsFor (daw, 0, voc);
        expectEquals ((int) instClips.size(), 2);
        expectEquals ((int) vocClips.size(),  2);

        // Both new clips share the same timeline boundary.
        expectEquals (prop64 (instClips[1], DawClipIDs::timelineStartSample),
                      prop64 (vocClips[1],  DawClipIDs::timelineStartSample));
    }

    //--------------------------------------------------------------------------
    // §1.5.4: a jump with no intervening growth produces no zero-length clip.
    void testDegenerateJumpCoalesced()
    {
        beginTest ("Degenerate (zero-growth) jump produces no empty clip");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 5000; src.file[0] = "fileA"; src.length[0] = 5'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 5000));
        // Immediate jump with no growth: the opening clip has zero length.
        engine.onEvent (ev (PerformanceEventType::BeatJump, 0, /*out*/ 5000, /*in*/ 500000));

        // The empty first clip is discarded; only the reopened clip remains.
        auto clips = clipsFor (daw, 0, lane);
        for (const auto& c : clips)
            expect (prop64 (c, DawClipIDs::sourceEndSample)
                    >= prop64 (c, DawClipIDs::sourceStartSample));
        expectLessOrEqual ((int) clips.size(), 1);
        expectEquals (engine.numOpenSlots(), 1);
    }

    //--------------------------------------------------------------------------
    // FifoPerformanceCaptureSink: gated by the recording predicate, and enqueues
    // out -> sourceSamplePosition, in -> payload with the right type/deck.
    void testFifoSinkGatingAndShape()
    {
        beginTest ("FIFO capture sink gates on recording and encodes out/in");

        PerformanceEventFifo fifo;
        bool recording = false;
        FifoPerformanceCaptureSink sink (fifo, [&recording] { return recording; });

        // Not recording: dropped.
        sink.captureJump (1, PerformanceEventType::HotCueJump, 1000, 2000);
        expectEquals ((int) fifo.getNumReady(), 0);

        // Recording: captured with correct shape.
        recording = true;
        sink.captureJump (2, PerformanceEventType::BeatJump, 7777, 9999);

        CapturingHandler handler;
        fifo.drain (handler);
        expectEquals ((int) handler.events.size(), 1);
        const auto& e = handler.events.front();
        expect (e.type == PerformanceEventType::BeatJump);
        expectEquals ((int) e.deckIndex, 2);
        expectEquals (e.sourceSamplePosition, (std::int64_t) 7777);
        expectEquals (e.payload,              (std::int64_t) 9999);
    }
};

static HotCueBeatJumpCaptureTests hotCueBeatJumpCaptureTests;
