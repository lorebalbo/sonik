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

struct CapturingHandler final : public PerformanceEventHandler
{
    std::vector<PerformanceEvent> events;
    void onPerformanceEvent (const PerformanceEvent& e) override { events.push_back (e); }
};

// Asserts every clip in tree order tiles back-to-back with no gap/overlap.
} // namespace

//==============================================================================
class LoopCaptureTests : public juce::UnitTest
{
public:
    LoopCaptureTests() : juce::UnitTest ("Loop Capture", "Sonik") {}

    void runTest() override
    {
        testOnePassPerLoopWithLeadInAndPartial();
        testBoundsChangeMidLoopAppliesNextPass();
        testExitAtBoundaryEmitsNoPartial();
        testStemsLoopWritesAllLanes();
        testReLoopStartsFreshContiguousSequence();
        testLoopWhilePausedFirstPassAligns();
        testGrowSuspendedWhileLooping();
        testFifoSinkLoopEvents();
    }

    //--------------------------------------------------------------------------
    // Full loop run: lead-in, two identical full passes, a partial final pass,
    // then a resumed growing clip — all contiguous on the timeline.
    void testOnePassPerLoopWithLeadInAndPartial()
    {
        beginTest ("Loop run: lead-in + N identical passes + partial + resume, contiguous");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 5000; engine.grow();                    // lead-in grows to 5000

        engine.onEvent (ev (PerformanceEventType::LoopIn, 0, /*engage*/ 5000));
        expect (engine.isLooping (0));
        expectEquals (engine.numOpenSlots(), 0);             // grow path suspended

        // Two full passes [2000, 10000).
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));

        // Exit mid-cycle at source 6000 -> partial [2000, 6000).
        engine.onEvent (ev (PerformanceEventType::LoopOut, 0, /*exit*/ 6000, /*in*/ 2000));
        expect (! engine.isLooping (0));
        expectEquals (engine.numOpenSlots(), 1);             // resumed growing clip

        auto clips = clipsFor (daw, 0, lane);
        // lead-in, pass1, pass2, partial, resume.
        expectEquals ((int) clips.size(), 5);

        // Pass crops are identical full-loop crops.
        for (int i : { 1, 2 })
        {
            expectEquals (prop64 (clips[i], DawClipIDs::sourceStartSample), (std::int64_t) 2000);
            expectEquals (prop64 (clips[i], DawClipIDs::sourceEndSample),   (std::int64_t) 10000);
        }
        // Partial final pass.
        expectEquals (prop64 (clips[3], DawClipIDs::sourceStartSample), (std::int64_t) 2000);
        expectEquals (prop64 (clips[3], DawClipIDs::sourceEndSample),   (std::int64_t) 6000);

        // Back-to-back timeline contiguity across the whole run (excluding the
        // still-growing final clip whose end is its start).
        for (int i = 1; i < (int) clips.size(); ++i)
        {
            const auto prevStart = prop64 (clips[i - 1], DawClipIDs::timelineStartSample);
            const auto prevLen   = prop64 (clips[i - 1], DawClipIDs::sourceEndSample)
                                 - prop64 (clips[i - 1], DawClipIDs::sourceStartSample);
            expectEquals (prop64 (clips[i], DawClipIDs::timelineStartSample), prevStart + prevLen);
        }

        // Same source file on every clip in the run.
        for (const auto& c : clips)
            expect (c.getProperty (DawClipIDs::sourceFileId).toString() == juce::String ("fileA"));
    }

    //--------------------------------------------------------------------------
    // §1.5.3: each pass clip's crop is the bounds carried by its own wrap event.
    void testBoundsChangeMidLoopAppliesNextPass()
    {
        beginTest ("Mid-loop bounds change applies to the next pass");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 2000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::LoopIn, 0, /*engage*/ 2000));

        // Pass 1: 4-beat loop [2000, 10000). Then doubled: pass 2 [2000, 18000).
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 18000, /*in*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopOut,  0, /*exit*/ 10000, /*in*/ 2000));

        auto clips = clipsFor (daw, 0, lane);
        // lead-in, pass1, pass2, partial[2000,10000), resume.
        expect ((int) clips.size() >= 3);
        expectEquals (prop64 (clips[1], DawClipIDs::sourceEndSample), (std::int64_t) 10000);
        expectEquals (prop64 (clips[2], DawClipIDs::sourceEndSample), (std::int64_t) 18000);
    }

    //--------------------------------------------------------------------------
    // Exit exactly at the boundary emits no zero-length partial pass.
    void testExitAtBoundaryEmitsNoPartial()
    {
        beginTest ("Exit at loop boundary emits no zero-length clip");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 2000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::LoopIn,   0, /*engage*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));
        // Exit exactly at loopIn (== boundary, no partial heard).
        engine.onEvent (ev (PerformanceEventType::LoopOut,  0, /*exit*/ 2000, /*in*/ 2000));

        auto clips = clipsFor (daw, 0, lane);
        // lead-in, pass1, resume. No phantom partial.
        expectEquals ((int) clips.size(), 3);
        for (const auto& c : clips)
            expect (prop64 (c, DawClipIDs::sourceEndSample) >= prop64 (c, DawClipIDs::sourceStartSample));
    }

    //--------------------------------------------------------------------------
    // §1.4: one loop pass writes a clip to every active lane (stems).
    void testStemsLoopWritesAllLanes()
    {
        beginTest ("Loop pass writes to all active stem lanes");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto inst = laneIdFor (daw, 0, ChannelGroup::LaneKind::Instrumental);
        const auto voc  = laneIdFor (daw, 0, ChannelGroup::LaneKind::Vocal);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { inst, voc };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 2000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::LoopIn,   0, /*engage*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));

        auto instClips = clipsFor (daw, 0, inst);
        auto vocClips  = clipsFor (daw, 0, voc);
        // lead-in + 2 passes on each lane.
        expectEquals ((int) instClips.size(), 3);
        expectEquals ((int) vocClips.size(),  3);
        // Both lanes' passes share the timeline boundary.
        expectEquals (prop64 (instClips[1], DawClipIDs::timelineStartSample),
                      prop64 (vocClips[1],  DawClipIDs::timelineStartSample));
    }

    //--------------------------------------------------------------------------
    // §1.5.6: re-loop is a fresh loop-enter, contiguous with what preceded it.
    void testReLoopStartsFreshContiguousSequence()
    {
        beginTest ("Re-loop begins a fresh contiguous loop sequence");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 2000; engine.grow();

        // First loop run.
        engine.onEvent (ev (PerformanceEventType::LoopIn,   0, 2000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 10000, /*in*/ 2000));
        engine.onEvent (ev (PerformanceEventType::LoopOut,  0, /*exit*/ 10000, /*in*/ 2000));
        // resume growing clip; advance it.
        src.pos[0] = 20000; engine.grow();

        // Re-loop (fresh enter at 20000).
        engine.onEvent (ev (PerformanceEventType::LoopIn,   0, 20000));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 28000, /*in*/ 20000));
        engine.onEvent (ev (PerformanceEventType::LoopOut,  0, /*exit*/ 28000, /*in*/ 20000));

        auto clips = clipsFor (daw, 0, lane);
        // Verify monotonic, gapless timeline across both runs.
        for (int i = 1; i < (int) clips.size(); ++i)
        {
            const auto prevStart = prop64 (clips[i - 1], DawClipIDs::timelineStartSample);
            const auto prevLen   = prop64 (clips[i - 1], DawClipIDs::sourceEndSample)
                                 - prop64 (clips[i - 1], DawClipIDs::sourceStartSample);
            expectEquals (prop64 (clips[i], DawClipIDs::timelineStartSample), prevStart + prevLen);
        }
        expect ((int) clips.size() >= 4);
    }

    //--------------------------------------------------------------------------
    // §1.5.4: looping with no open clip (paused) still records passes; the first
    // pass aligns via the seam (pass-through = raw playhead).
    void testLoopWhilePausedFirstPassAligns()
    {
        beginTest ("Loop engaged while paused: first pass aligns via seam");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 0; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 7777;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        // No DeckPlay: nothing open. Engage loop directly.
        engine.onEvent (ev (PerformanceEventType::LoopIn,   0, /*engage*/ 0));
        engine.onEvent (ev (PerformanceEventType::LoopPass, 0, /*out*/ 9000, /*in*/ 1000));

        auto clips = clipsFor (daw, 0, lane);
        expectEquals ((int) clips.size(), 1);
        // First pass timeline start came from the seam (raw playhead).
        expectEquals (prop64 (clips[0], DawClipIDs::timelineStartSample), (std::int64_t) 7777);
        expectEquals (prop64 (clips[0], DawClipIDs::sourceStartSample),   (std::int64_t) 1000);
        expectEquals (prop64 (clips[0], DawClipIDs::sourceEndSample),     (std::int64_t) 9000);
    }

    //--------------------------------------------------------------------------
    // The ordinary grow path is suspended for a deck in loop-capture mode.
    void testGrowSuspendedWhileLooping()
    {
        beginTest ("Grow path is suspended while looping");

        auto daw = makeDaw();
        StubSourceProvider src;
        src.pos[0] = 1000; src.file[0] = "fileA"; src.length[0] = 10'000'000;
        const auto lane = originalLaneId (daw, 0);
        StubLaneResolver lanes; lanes.lanesByDeck[0] = { lane };

        std::int64_t playhead = 0;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        engine.onEvent (ev (PerformanceEventType::DeckPlay, 0, 1000));
        src.pos[0] = 3000; engine.grow();
        engine.onEvent (ev (PerformanceEventType::LoopIn, 0, /*engage*/ 3000));

        const int before = (int) clipsFor (daw, 0, lane).size();
        // Advancing the source and growing must not create or extend clips.
        src.pos[0] = 50000; engine.grow();
        expectEquals (engine.numOpenSlots(), 0);
        expectEquals ((int) clipsFor (daw, 0, lane).size(), before);
    }

    //--------------------------------------------------------------------------
    // FifoPerformanceCaptureSink encodes loop enter/pass/exit events correctly.
    void testFifoSinkLoopEvents()
    {
        beginTest ("FIFO capture sink encodes loop enter/pass/exit");

        PerformanceEventFifo fifo;
        bool recording = true;
        FifoPerformanceCaptureSink sink (fifo, [&recording] { return recording; });

        sink.captureLoopEnter (1, 5000);
        sink.captureLoopPass  (1, /*in*/ 2000, /*out*/ 10000);
        sink.captureLoopExit  (1, /*exit*/ 6000, /*in*/ 2000);

        CapturingHandler handler;
        fifo.drain (handler);
        expectEquals ((int) handler.events.size(), 3);

        expect (handler.events[0].type == PerformanceEventType::LoopIn);
        expectEquals (handler.events[0].sourceSamplePosition, (std::int64_t) 5000);

        expect (handler.events[1].type == PerformanceEventType::LoopPass);
        expectEquals (handler.events[1].sourceSamplePosition, (std::int64_t) 10000); // out
        expectEquals (handler.events[1].payload,              (std::int64_t) 2000);  // in

        expect (handler.events[2].type == PerformanceEventType::LoopOut);
        expectEquals (handler.events[2].sourceSamplePosition, (std::int64_t) 6000);  // exit
        expectEquals (handler.events[2].payload,              (std::int64_t) 2000);  // in

        // Gating: nothing captured when not recording.
        recording = false;
        sink.captureLoopPass (1, 2000, 10000);
        expectEquals ((int) fifo.getNumReady(), 0);
    }
};

static LoopCaptureTests loopCaptureTests;
