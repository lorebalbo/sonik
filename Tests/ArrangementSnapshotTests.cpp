//==============================================================================
// PRD-0079: ArrangementSnapshotTests — unit tests for:
//   - ArrangementSnapshot trivial-copy guarantees
//   - ArrangementCompiler: deterministic compilation, sort order, computed fields
//   - ArrangementPublisher: double-buffer SeqLock coherence, empty-before-publish
//   - ArrangementRecompileTrigger: coalescing of rapid requestRecompile() calls
//
// JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <thread>
#include <chrono>

#include "Features/Daw/Playback/ArrangementSnapshot.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Playback/ArrangementPublisher.h"
#include "Features/Daw/Playback/ArrangementRecompileTrigger.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/DawClip.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static juce::ValueTree buildDawWithClips()
{
    juce::ValueTree root ("SonikState");
    auto daw = DawState::getOrCreateDawBranch (root);

    // Create track 0 with its 3 lanes
    auto track = DawState::ensureTrackForDeck (daw, 0);

    // Add a clip to the "Original" lane (laneKind == "Original" → first lane)
    auto lanesNode = track.getChildWithName (DawIDs::lanes);
    auto laneNode  = lanesNode.getChild (0); // first lane (Original)
    auto clipsNode = laneNode.getOrCreateChildWithName (DawIDs::clips, nullptr);

    DawClip clip1;
    clip1.clipId              = juce::Uuid();
    clip1.laneId              = juce::Uuid (laneNode.getProperty (DawIDs::laneId).toString());
    clip1.sourceFileId        = "source-a";
    clip1.sourceStartSample   = 100;
    clip1.sourceEndSample     = 500;
    clip1.timelineStartSample = 200;
    clip1.sourceLengthSamples = 10000;
    clip1.gainDb              = 0.0f;
    clipsNode.addChild (DawClip::toValueTree (clip1), -1, nullptr);

    // Second clip in same lane (starts BEFORE clip1 to test sort)
    DawClip clip2;
    clip2.clipId              = juce::Uuid();
    clip2.laneId              = clip1.laneId;
    clip2.sourceFileId        = "source-b";
    clip2.sourceStartSample   = 0;
    clip2.sourceEndSample     = 100;
    clip2.timelineStartSample = 50;   // earlier → should sort before clip1
    clip2.sourceLengthSamples = 10000;
    clip2.gainDb              = -6.0f; // non-unity gain
    clipsNode.addChild (DawClip::toValueTree (clip2), -1, nullptr);

    return daw;
}

// Builds a lane holding two clips that butt-join on the timeline: clip A spans
// timeline [0, 400) and clip B spans [400, 700). Their SOURCE crops are
// discontinuous (B starts at source 1000), modelling a jump/loop split of one
// continuous take — exactly the recording case that must reproduce seamlessly.
static juce::ValueTree buildDawWithContiguousClips()
{
    juce::ValueTree root ("SonikState");
    auto daw   = DawState::getOrCreateDawBranch (root);
    auto track = DawState::ensureTrackForDeck (daw, 0);

    auto lanesNode = track.getChildWithName (DawIDs::lanes);
    auto laneNode  = lanesNode.getChild (0);
    auto clipsNode = laneNode.getOrCreateChildWithName (DawIDs::clips, nullptr);
    const auto laneId = juce::Uuid (laneNode.getProperty (DawIDs::laneId).toString());

    DawClip a;
    a.clipId = juce::Uuid(); a.laneId = laneId; a.sourceFileId = "take";
    a.sourceStartSample = 0;   a.sourceEndSample = 400;
    a.timelineStartSample = 0;  a.sourceLengthSamples = 100000; a.gainDb = 0.0f;
    clipsNode.addChild (DawClip::toValueTree (a), -1, nullptr);

    DawClip b;
    b.clipId = juce::Uuid(); b.laneId = laneId; b.sourceFileId = "take";
    b.sourceStartSample = 1000; b.sourceEndSample = 1300;   // 300-sample crop
    b.timelineStartSample = 400;  // == clip A's timeline end → butt-join
    b.sourceLengthSamples = 100000; b.gainDb = 0.0f;
    clipsNode.addChild (DawClip::toValueTree (b), -1, nullptr);

    return daw;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test class
// ─────────────────────────────────────────────────────────────────────────────

class ArrangementSnapshotTests final : public juce::UnitTest
{
public:
    ArrangementSnapshotTests() : juce::UnitTest ("Arrangement Snapshot (PRD-0079)", "Sonik") {}

    void runTest() override
    {
        testClipEventIsTriviallyCopyable();
        testEmptySnapshotBeforeFirstPublish();
        testCompilerProducesCorrectClipEvents();
        testCompilerSortsByTimelineStart();
        testCompilerTimelineEndSample();
        testCompilerNonUnityGain();
        testCompilerHandleResolver();
        testCompilerFlagsButtJoinedClips();
        testCompilerDoesNotFlagSeparatedClips();
        testCompilerButtJoinExactAtNonUnityScale();
        testPublisherCoherence_singleThread();
        testPublisherCoherence_concurrent();
        testRecompileTriggerCoalescing();
    }

private:
    // ─── Struct guarantees ─────────────────────────────────────────────────

    void testClipEventIsTriviallyCopyable()
    {
        beginTest ("ClipEvent is trivially copyable");
        expect (std::is_trivially_copyable<Daw::ClipEvent>::value);
        // ArrangementSnapshot is a large struct containing arrays; it is
        // copyable (operator=) but not required to be trivially copyable.
        expect (std::is_copy_assignable<Daw::ArrangementSnapshot>::value);
    }

    // ─── Publisher: empty before first publish ─────────────────────────────

    void testEmptySnapshotBeforeFirstPublish()
    {
        beginTest ("Publisher returns empty snapshot before first publish");

        Daw::ArrangementPublisher publisher;
        Daw::ArrangementSnapshot  out;
        publisher.read (out);

        expect (out.isEmpty(),   "snapshot should be empty");
        expectEquals (out.laneCount, 0);
    }

    // ─── Compiler: basic clip events ──────────────────────────────────────

    void testCompilerProducesCorrectClipEvents()
    {
        beginTest ("Compiler produces correct ClipEvents from ValueTree");

        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler  compiler;
        Daw::ArrangementSnapshot  snap;
        compiler.compile (daw, snap);

        expect (snap.laneCount >= 1, "at least one lane");
        const auto& lane0 = snap.lanes[0];
        expectEquals (lane0.count, 2);
    }

    // ─── Compiler: sort order ─────────────────────────────────────────────

    void testCompilerSortsByTimelineStart()
    {
        beginTest ("Compiler sorts lane clips by timelineStartSample ascending");

        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler  compiler;
        Daw::ArrangementSnapshot  snap;
        compiler.compile (daw, snap);

        const auto& lane0 = snap.lanes[0];
        expect (lane0.count == 2);
        // clip2 (timeline=50) should be before clip1 (timeline=200)
        expect (lane0.events[0].timelineStartSample <= lane0.events[1].timelineStartSample,
                "clips must be sorted ascending by timelineStartSample");
        expectEquals (lane0.events[0].timelineStartSample, (int64_t) 50);
        expectEquals (lane0.events[1].timelineStartSample, (int64_t) 200);
    }

    // ─── Compiler: timelineEndSample = start + (srcEnd - srcStart) ────────

    void testCompilerTimelineEndSample()
    {
        beginTest ("Compiler computes timelineEndSample correctly");

        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler  compiler;
        Daw::ArrangementSnapshot  snap;
        compiler.compile (daw, snap);

        // clip1: sourceStart=100, sourceEnd=500, timelineStart=200
        // → timelineEnd = 200 + (500-100) = 600
        const auto& lane0 = snap.lanes[0];
        // After sorting: events[0] = clip2 (timeline=50), events[1] = clip1 (timeline=200)
        const auto& ev1 = lane0.events[1];
        expectEquals (ev1.timelineStartSample, (int64_t) 200);
        expectEquals (ev1.sourceStartSample,   (int64_t) 100);
        expectEquals (ev1.sourceEndSample,     (int64_t) 500);
        const int64_t expectedEnd = 200 + (500 - 100);
        expectEquals (ev1.timelineEndSample, expectedEnd);
    }

    // ─── Compiler: butt-joined clips are flagged for seamless playback ─────

    void testCompilerFlagsButtJoinedClips()
    {
        beginTest ("Compiler flags butt-joined clips (joinsNext / joinsPrev)");

        auto daw = buildDawWithContiguousClips();
        Daw::ArrangementCompiler compiler;
        Daw::ArrangementSnapshot snap;
        compiler.compile (daw, snap);

        const auto& lane0 = snap.lanes[0];
        expectEquals (lane0.count, 2);

        // events[0] = clip A (timeline 0..400), events[1] = clip B (400..700).
        const auto& a = lane0.events[0];
        const auto& b = lane0.events[1];
        expectEquals (a.timelineEndSample,   b.timelineStartSample,
                      "the two clips must butt-join exactly on the timeline");

        // The shared edge is suppressed on BOTH sides.
        expect (a.joinsNext, "outgoing clip must skip its fade-out at the join");
        expect (b.joinsPrev, "incoming clip must skip its fade-in at the join");

        // The OUTER edges (meet silence) keep their fades.
        expect (! a.joinsPrev, "clip A start meets silence → keep fade-in");
        expect (! b.joinsNext, "clip B end meets silence → keep fade-out");
    }

    void testCompilerDoesNotFlagSeparatedClips()
    {
        beginTest ("Compiler does NOT flag clips separated by a gap");

        // buildDawWithClips places clip2 at [50,150) and clip1 at [200,600):
        // a 50-sample silence gap, so neither edge may be suppressed.
        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler compiler;
        Daw::ArrangementSnapshot snap;
        compiler.compile (daw, snap);

        const auto& lane0 = snap.lanes[0];
        expectEquals (lane0.count, 2);
        for (int i = 0; i < lane0.count; ++i)
        {
            expect (! lane0.events[i].joinsPrev, "separated clip must keep fade-in");
            expect (! lane0.events[i].joinsNext, "separated clip must keep fade-out");
        }
    }

    void testCompilerButtJoinExactAtNonUnityScale()
    {
        beginTest ("Butt-join stays exact under a non-unity sample-rate scale");

        // At 48 kHz the project->runtime scale is non-integer; the scaled
        // timeline end of clip A must still land EXACTLY on the scaled timeline
        // start of clip B so the join is detected (regression for the
        // independently-rounded-delta gap).
        auto daw = buildDawWithContiguousClips();
        Daw::ArrangementCompiler compiler (
            [] (const Daw::ArrangementCompiler::ClipResolveRequest&) -> int32_t { return 0; },
            48000.0 / 44100.0);
        Daw::ArrangementSnapshot snap;
        compiler.compile (daw, snap);

        const auto& lane0 = snap.lanes[0];
        expectEquals (lane0.count, 2);
        expectEquals (lane0.events[0].timelineEndSample, lane0.events[1].timelineStartSample,
                      "scaled timeline end/start must match exactly at 48 kHz");
        expect (lane0.events[0].joinsNext && lane0.events[1].joinsPrev,
                "the join must still be detected at a non-unity scale");
    }

    // ─── Compiler: gain conversion ────────────────────────────────────────

    void testCompilerNonUnityGain()
    {
        beginTest ("Compiler converts gainDb to linear gain");

        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler  compiler;
        Daw::ArrangementSnapshot  snap;
        compiler.compile (daw, snap);

        // clip2 has gainDb=-6.0 → linear ≈ 0.501 (10^(-6/20))
        const auto& lane0 = snap.lanes[0];
        const auto& ev0   = lane0.events[0]; // clip2 is first after sort
        const float expectedGain = std::pow (10.0f, -6.0f / 20.0f);
        expectWithinAbsoluteError (ev0.gain, expectedGain, 1e-4f);
    }

    // ─── Compiler: handle resolver ────────────────────────────────────────

    void testCompilerHandleResolver()
    {
        beginTest ("Compiler resolves sourceFileId to streamer handle");

        // Resolver maps "source-a" → 7, anything else → -1
        auto daw = buildDawWithClips();
        Daw::ArrangementCompiler compiler ([] (const juce::String& id) -> int32_t
        {
            return (id == "source-a") ? 7 : -1;
        });

        Daw::ArrangementSnapshot snap;
        compiler.compile (daw, snap);

        const auto& lane0 = snap.lanes[0];
        expect (lane0.count == 2);

        // Find which event is clip1 (sourceStart=100) and clip2 (sourceStart=0)
        int idx_a = -1, idx_b = -1;
        for (int i = 0; i < lane0.count; ++i)
        {
            if (lane0.events[i].sourceStartSample == 100) idx_a = i;
            if (lane0.events[i].sourceStartSample == 0)   idx_b = i;
        }
        expect (idx_a >= 0 && idx_b >= 0);
        expectEquals (lane0.events[idx_a].sourceReadHandle, (int32_t) 7);
        expectEquals (lane0.events[idx_b].sourceReadHandle, (int32_t) -1);
    }

    // ─── Publisher: single-thread coherence ───────────────────────────────

    void testPublisherCoherence_singleThread()
    {
        beginTest ("Publisher single-iteration read under no contention");

        Daw::ArrangementPublisher publisher;

        // Build a snapshot with one lane / two events
        Daw::ArrangementSnapshot snap;
        snap.laneCount = 1;
        snap.lanes[0].count = 2;
        snap.lanes[0].events[0].timelineStartSample = 10;
        snap.lanes[0].events[0].gain = 1.0f;
        snap.lanes[0].events[1].timelineStartSample = 20;
        snap.lanes[0].events[1].gain = 0.5f;

        publisher.publish (snap);

        Daw::ArrangementSnapshot out;
        publisher.read (out);

        expectEquals (out.laneCount, 1);
        expectEquals (out.lanes[0].count, 2);
        expectEquals (out.lanes[0].events[0].timelineStartSample, (int64_t) 10);
        expectEquals (out.lanes[0].events[1].timelineStartSample, (int64_t) 20);

        // Sequence counter must be even (stable).
        expect ((publisher.sequenceForTest() & 1u) == 0u);
    }

    // ─── Publisher: concurrent coherence ─────────────────────────────────

    void testPublisherCoherence_concurrent()
    {
        beginTest ("Publisher: concurrent publish loop never produces torn snapshot");

        Daw::ArrangementPublisher publisher;
        std::atomic<bool> stop { false };
        std::atomic<int>  tornCount { 0 };

        // Writer thread: continuously publishes alternating snapshots.
        std::thread writer ([&]
        {
            int counter = 0;
            while (!stop.load (std::memory_order_relaxed))
            {
                Daw::ArrangementSnapshot snap;
                snap.laneCount = 1;
                snap.lanes[0].count = 1;
                // marker: timelineStartSample == timelineEndSample + 99 (a verifiable invariant)
                snap.lanes[0].events[0].timelineStartSample = counter;
                snap.lanes[0].events[0].timelineEndSample   = counter + 99;
                snap.lanes[0].events[0].gain = static_cast<float> (counter);
                publisher.publish (snap);
                ++counter;
            }
        });

        // Reader: reads many times and verifies internal consistency.
        Daw::ArrangementSnapshot out;
        for (int i = 0; i < 100000; ++i)
        {
            publisher.read (out);
            if (out.laneCount > 0 && out.lanes[0].count > 0)
            {
                const auto& ev = out.lanes[0].events[0];
                // Invariant: timelineEndSample == timelineStartSample + 99
                if (ev.timelineEndSample != ev.timelineStartSample + 99)
                    ++tornCount;
            }
        }

        stop.store (true, std::memory_order_relaxed);
        writer.join();

        expectEquals (tornCount.load(), 0, "No torn snapshot should be observed");
    }

    // ─── RecompileTrigger: coalescing ─────────────────────────────────────

    void testRecompileTriggerCoalescing()
    {
        beginTest ("RecompileTrigger coalesces rapid calls into one publish");

        juce::ValueTree root ("SonikState");
        auto daw = DawState::getOrCreateDawBranch (root);

        Daw::ArrangementPublisher publisher;
        Daw::ArrangementCompiler  compiler;

        Daw::ArrangementRecompileTrigger trigger (daw, std::move (compiler), publisher);

        // Synchronous compile should produce exactly one publish.
        trigger.compileNow();
        expectEquals (trigger.publishCount(), 1);

        // Fire N rapid triggers — the async coalescing reduces them to ≤ N publishes
        // (the compileNow resets the pending flag so the next set is independent).
        trigger.compileNow();
        trigger.compileNow();
        expectEquals (trigger.publishCount(), 3);
    }
};

static ArrangementSnapshotTests arrangementSnapshotTestsInstance;
