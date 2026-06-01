#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Recording/ClipPlacementEngine.h"
#include "Features/Daw/Recording/PerformanceEventFifo.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/State/DawClipModel.h"
#include "Features/Daw/Model/ChannelGroup.h"

#include <cmath>
#include <map>
#include <vector>

namespace
{

using namespace Daw;

constexpr double kSR = DawState::kProjectSampleRate; // 44100

std::int64_t sec (double s) { return static_cast<std::int64_t> (std::llround (s * kSR)); }

struct StubSourceProvider final : public DeckSourceProvider
{
    std::map<int, std::int64_t> pos;
    std::map<int, juce::String> file;
    std::map<int, std::int64_t> length;

    std::int64_t getSourceSample (int deck) const override
    {
        auto it = pos.find (deck); return it != pos.end() ? it->second : 0;
    }
    juce::String getSourceFileId (int deck) const override
    {
        auto it = file.find (deck); return it != file.end() ? it->second : juce::String();
    }
    std::int64_t getSourceLength (int deck) const override
    {
        auto it = length.find (deck); return it != length.end() ? it->second : 0;
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

PerformanceEvent ev (PerformanceEventType type, int deck, std::int64_t srcPos, std::int64_t payload = 0)
{
    PerformanceEvent e;
    e.type                 = type;
    e.deckIndex            = static_cast<std::uint8_t> (deck);
    e.sourceSamplePosition = srcPos;
    e.payload              = payload;
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
        if (lane.getProperty (DawIDs::laneId).toString() != laneId.toString()) continue;
        auto clips = lane.getChildWithName (DawIDs::clips);
        for (int j = 0; j < clips.getNumChildren(); ++j)
            if (clips.getChild (j).hasType (DawIDs::clip))
                out.push_back (clips.getChild (j));
    }
    return out;
}

std::int64_t prop64 (const juce::ValueTree& n, const juce::Identifier& id)
{
    return static_cast<std::int64_t> (n.getProperty (id));
}

juce::ValueTree makeDaw()
{
    juce::ValueTree daw (DawIDs::Daw);
    daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);
    return daw;
}

} // namespace

//==============================================================================
// PRD-0078: full-pipeline validation of four decks captured simultaneously,
// each into its own channel group, exercising play, hot-cue, loop and
// source-mode through ONE PerformanceEventFifo drained on the message thread.
class RecordingCaptureTests : public juce::UnitTest
{
public:
    RecordingCaptureTests() : juce::UnitTest ("Multi-Deck Recording Capture", "Sonik") {}

    void runTest() override
    {
        testFourDeckSimultaneousCapture();
    }

    void testFourDeckSimultaneousCapture()
    {
        beginTest ("Four decks capture simultaneously into their own channel groups");

        auto daw = makeDaw();
        StubSourceProvider src;
        for (int d = 0; d < 4; ++d) { src.length[d] = 100'000'000; }
        src.file[0] = "fileA"; src.file[1] = "fileB";
        src.file[2] = "fileC"; src.file[3] = "fileD";

        // Lane ids per deck.
        const auto aOrig = laneIdFor (daw, 0, ChannelGroup::LaneKind::Original);
        const auto bOrig = laneIdFor (daw, 1, ChannelGroup::LaneKind::Original);
        const auto cOrig = laneIdFor (daw, 2, ChannelGroup::LaneKind::Original);
        const auto dOrig = laneIdFor (daw, 3, ChannelGroup::LaneKind::Original);
        const auto dInst = laneIdFor (daw, 3, ChannelGroup::LaneKind::Instrumental);
        const auto dVoc  = laneIdFor (daw, 3, ChannelGroup::LaneKind::Vocal);

        StubLaneResolver lanes;
        lanes.lanesByDeck[0] = { aOrig };
        lanes.lanesByDeck[1] = { bOrig };
        lanes.lanesByDeck[2] = { cOrig };
        lanes.lanesByDeck[3] = { dOrig };

        std::int64_t playhead = 0;
        PerformanceEventFifo fifo;
        ClipPlacementEngine engine (daw, src, lanes, [&playhead] { return playhead; });

        auto drain = [&] { fifo.drain (engine); };

        // ---- Staggered play: A@30s, B@45s, C@60s, D@75s --------------------
        playhead = sec (30); src.pos[0] = sec (120);
        fifo.enqueue (ev (PerformanceEventType::DeckPlay, 0, sec (120))); drain();

        src.pos[0] = sec (120) + sec (15); engine.grow();
        playhead = sec (45); src.pos[1] = 0;
        fifo.enqueue (ev (PerformanceEventType::DeckPlay, 1, 0)); drain();

        src.pos[0] += sec (15); src.pos[1] = sec (15); engine.grow();
        playhead = sec (60); src.pos[2] = 0;
        fifo.enqueue (ev (PerformanceEventType::DeckPlay, 2, 0)); drain();

        src.pos[0] += sec (15); src.pos[1] += sec (15); src.pos[2] = sec (15); engine.grow();
        playhead = sec (75); src.pos[3] = 0;
        fifo.enqueue (ev (PerformanceEventType::DeckPlay, 3, 0)); drain();

        // ---- Hot cue on deck B only (jump to source 10s) -------------------
        src.pos[0] += sec (15); src.pos[1] += sec (15);
        src.pos[2] += sec (15); src.pos[3] = sec (15); engine.grow();
        playhead = sec (90);
        const std::int64_t bJumpOut = src.pos[1];
        fifo.enqueue (ev (PerformanceEventType::HotCueJump, 1, bJumpOut, sec (10))); drain();
        src.pos[1] = sec (10); // deck B now plays from the cue point

        // ---- Loop on deck C only: enter, 2 passes, exit at boundary --------
        src.pos[0] += sec (15); src.pos[1] += sec (15);
        src.pos[2] += sec (15); src.pos[3] += sec (15); engine.grow();
        playhead = sec (105);
        const std::int64_t loopIn  = src.pos[2];
        const std::int64_t loopLen = sec (2);
        fifo.enqueue (ev (PerformanceEventType::LoopIn,  2, loopIn));                       drain();
        fifo.enqueue (ev (PerformanceEventType::LoopPass, 2, loopIn + loopLen, loopIn));    drain();
        fifo.enqueue (ev (PerformanceEventType::LoopPass, 2, loopIn + loopLen, loopIn));    drain();
        fifo.enqueue (ev (PerformanceEventType::LoopOut,  2, loopIn, loopIn));              drain();
        src.pos[2] = loopIn; // resumed from the loop in-point

        // ---- Source-mode switch on deck D only: Original -> stems ----------
        src.pos[0] += sec (15); src.pos[1] += sec (15);
        src.pos[2] += sec (15); src.pos[3] += sec (15); engine.grow();
        playhead = sec (120);
        lanes.lanesByDeck[3] = { dInst, dVoc };
        fifo.enqueue (ev (PerformanceEventType::SourceModeChange, 3, src.pos[3])); drain();

        // ---- Stop: finalise all open clips ---------------------------------
        src.pos[0] += sec (15); src.pos[1] += sec (15);
        src.pos[2] += sec (15); src.pos[3] += sec (15); engine.grow();
        playhead = sec (135);
        engine.finaliseAll (playhead);

        // ===== Assertions ===================================================

        // No events lost through the FIFO.
        expectEquals ((int) fifo.getNumReady(), 0);
        expectEquals ((int) fifo.getOverflowCount(), 0);

        // ---- Deck A: one clip, staggered start at 0:30, own file -----------
        auto a = clipsFor (daw, 0, aOrig);
        expectEquals ((int) a.size(), 1);
        expectEquals (prop64 (a[0], DawClipIDs::timelineStartSample), sec (30));
        expectEquals (prop64 (a[0], DawClipIDs::sourceStartSample), sec (120));
        expect (a[0].getProperty (DawClipIDs::sourceFileId).toString() == "fileA");

        // ---- Deck B: two clips (pre- and post-hot-cue), contiguous ---------
        auto b = clipsFor (daw, 1, bOrig);
        expectEquals ((int) b.size(), 2);
        expectEquals (prop64 (b[0], DawClipIDs::timelineStartSample), sec (45));
        expectEquals (prop64 (b[0], DawClipIDs::sourceEndSample), bJumpOut);
        expectEquals (prop64 (b[1], DawClipIDs::timelineStartSample), sec (90));
        expectEquals (prop64 (b[1], DawClipIDs::sourceStartSample), sec (10));
        for (auto& c : b)
            expect (c.getProperty (DawClipIDs::sourceFileId).toString() == "fileB");

        // ---- Deck C: lead-in + two loop passes + resume, ordered -----------
        auto c = clipsFor (daw, 2, cOrig);
        expectEquals ((int) c.size(), 4);
        expectEquals (prop64 (c[0], DawClipIDs::timelineStartSample), sec (60));
        for (int i = 1; i < (int) c.size(); ++i)
            expect (prop64 (c[i], DawClipIDs::timelineStartSample)
                    > prop64 (c[i - 1], DawClipIDs::timelineStartSample));
        for (auto& clip : c)
            expect (clip.getProperty (DawClipIDs::sourceFileId).toString() == "fileC");

        // ---- Deck D: Original closed, both stems opened at the switch -------
        auto dO = clipsFor (daw, 3, dOrig);
        auto dI = clipsFor (daw, 3, dInst);
        auto dV = clipsFor (daw, 3, dVoc);
        expectEquals ((int) dO.size(), 1);
        expectEquals ((int) dI.size(), 1);
        expectEquals ((int) dV.size(), 1);
        expectEquals (prop64 (dO[0], DawClipIDs::timelineStartSample), sec (75));
        expectEquals (prop64 (dI[0], DawClipIDs::timelineStartSample), sec (120));
        expectEquals (prop64 (dV[0], DawClipIDs::timelineStartSample), sec (120));

        // ---- No cross-deck leakage: each deck only wrote its own lanes -----
        expectEquals ((int) clipsFor (daw, 0, bOrig).size(), 0);
        expectEquals ((int) clipsFor (daw, 1, aOrig).size(), 0);
        expectEquals ((int) clipsFor (daw, 0, dInst).size(), 0);

        // Everything finalised: no open slots remain.
        expectEquals (engine.numOpenSlots(), 0);
    }
};

static RecordingCaptureTests recordingCaptureTests;
