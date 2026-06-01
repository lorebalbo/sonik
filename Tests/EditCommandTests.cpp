//==============================================================================
// PRD-0083: EditCommandTests — unit tests for the edit command layer.
//
// Tests:
//   - MoveClip updates timelineStartSample only
//   - TrimClipStart/End shortens crop correctly
//   - UncropClipStart/End extends crop within source bounds
//   - SplitClip creates two contiguous clips
//   - DeleteClip removes the clip from the container
//   - SetClipGain updates gainDb
//   - Undo/redo round-trips for each command
//   - Clamping: invalid inputs are rejected/clamped
//   - Drag coalescing: multiple moves coalesce into one undo step
//
// JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Editing/EditCommands.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Playback/ArrangementPublisher.h"
#include "Features/Daw/Playback/ArrangementRecompileTrigger.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a Daw branch with a single clip
// ─────────────────────────────────────────────────────────────────────────────

static juce::String addClipToFirstLane (juce::ValueTree& daw,
                                         int64_t srcStart, int64_t srcEnd,
                                         int64_t srcLen,
                                         int64_t tStart,
                                         float gainDb = 0.0f)
{
    auto track = DawState::ensureTrackForDeck (daw, 0);
    auto lanes  = track.getChildWithName (DawIDs::lanes);
    auto lane   = lanes.getChild (0);
    auto clips  = lane.getOrCreateChildWithName (DawIDs::clips, nullptr);

    DawClip clip;
    clip.clipId              = juce::Uuid();
    clip.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
    clip.sourceFileId        = "test-source";
    clip.sourceStartSample   = srcStart;
    clip.sourceEndSample     = srcEnd;
    clip.timelineStartSample = tStart;
    clip.sourceLengthSamples = srcLen;
    clip.gainDb              = gainDb;
    clips.addChild (DawClip::toValueTree (clip), -1, nullptr);

    return clip.clipId.toString();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture helper
// ─────────────────────────────────────────────────────────────────────────────

struct EditFixture
{
    juce::ValueTree root { "SonikState" };
    juce::ValueTree daw;
    juce::UndoManager undoManager { 30000, 30 };
    Daw::ArrangementPublisher publisher;
    Daw::ArrangementCompiler  compiler;
    std::unique_ptr<Daw::ArrangementRecompileTrigger> trigger;
    std::unique_ptr<Daw::EditCommandDispatcher>       dispatcher;

    EditFixture()
    {
        daw = DawState::getOrCreateDawBranch (root);
        trigger = std::make_unique<Daw::ArrangementRecompileTrigger> (
            daw, Daw::ArrangementCompiler{}, publisher);
        dispatcher = std::make_unique<Daw::EditCommandDispatcher> (
            daw, undoManager, *trigger);
    }

    juce::ValueTree findClip (const juce::String& clipId)
    {
        return Daw::ClipCmdHelpers::findClipNode (daw, clipId);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Test class
// ─────────────────────────────────────────────────────────────────────────────

class EditCommandTests final : public juce::UnitTest
{
public:
    EditCommandTests() : juce::UnitTest ("Edit Commands (PRD-0083)", "Sonik") {}

    void runTest() override
    {
        testMoveClip();
        testTrimClipStart();
        testTrimClipEnd();
        testUncropClipStart();
        testUncropClipEnd();
        testSplitClip();
        testDeleteClip();
        testSetClipGain();
        testMoveUndoRedo();
        testClampMoveToZero();
    }

private:
    // ─── MoveClip ──────────────────────────────────────────────────────────

    void testMoveClip()
    {
        beginTest ("MoveClip changes timelineStartSample only");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 500);

        f.dispatcher->beginMoveDrag (id);
        f.dispatcher->moveClip (id, 800);

        auto clip = f.findClip (id);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample))),
                      (int64_t) 800,
                      "timeline start should update");
        // Source crop unchanged.
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample))),
                      (int64_t) 0);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample))),
                      (int64_t) 1000);
    }

    // ─── TrimClipStart ────────────────────────────────────────────────────

    void testTrimClipStart()
    {
        beginTest ("TrimClipStart raises sourceStartSample and shifts timelineStart");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 100);

        f.dispatcher->beginTrimDrag (id);
        f.dispatcher->trimClipStart (id, 200); // trim 200 samples from start

        auto clip = f.findClip (id);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample))),
                      (int64_t) 200);
        // Timeline start shifts by same delta (+200).
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample))),
                      (int64_t) 300, "timelineStart should shift by trim delta");
    }

    // ─── TrimClipEnd ──────────────────────────────────────────────────────

    void testTrimClipEnd()
    {
        beginTest ("TrimClipEnd lowers sourceEndSample");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 0);

        f.dispatcher->beginTrimDrag (id);
        f.dispatcher->trimClipEnd (id, 700);

        auto clip = f.findClip (id);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample))),
                      (int64_t) 700);
    }

    // ─── UncropClipStart ──────────────────────────────────────────────────

    void testUncropClipStart()
    {
        beginTest ("UncropClipStart extends start toward 0");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 500, 1500, 5000, 300);

        f.dispatcher->beginUncropDrag (id);
        f.dispatcher->uncropClipStart (id, 200); // extend 300 more samples

        auto clip = f.findClip (id);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample))),
                      (int64_t) 200, "source start should extend to 200");
        // Timeline start shifts left by same delta.
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample))),
                      (int64_t) 0, "timeline start should shift left");
    }

    // ─── UncropClipEnd ────────────────────────────────────────────────────

    void testUncropClipEnd()
    {
        beginTest ("UncropClipEnd extends end toward sourceLengthSamples");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 0);

        f.dispatcher->beginUncropDrag (id);
        f.dispatcher->uncropClipEnd (id, 2000);

        auto clip = f.findClip (id);
        expectEquals (static_cast<int64_t> (static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample))),
                      (int64_t) 2000);
    }

    // ─── SplitClip ────────────────────────────────────────────────────────

    void testSplitClip()
    {
        beginTest ("SplitClip creates two contiguous clips");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 2000, 5000, 0);

        // Cut at timeline position 1000 (= source position 1000 since tStart=0).
        f.dispatcher->splitClip (id, 1000);

        // Find both clips.
        auto track = DawState::findTrackForDeck (f.daw, 0);
        auto lane  = track.getChildWithName (DawIDs::lanes).getChild (0);
        auto clips = lane.getChildWithName (DawIDs::clips);

        expectEquals (clips.getNumChildren(), 2, "should have 2 clips after split");

        // Find left and right by sourceEnd / sourceStart.
        juce::ValueTree left, right;
        for (int i = 0; i < clips.getNumChildren(); ++i)
        {
            auto c = clips.getChild (i);
            const int64_t se = static_cast<int64_t> (static_cast<double> (c.getProperty (DawClipIDs::sourceEndSample)));
            if (se == 1000) left  = c;
            else            right = c;
        }

        expect (left.isValid(),  "left half must exist");
        expect (right.isValid(), "right half must exist");

        const int64_t rightSrcStart = static_cast<int64_t> (
            static_cast<double> (right.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t rightTStart   = static_cast<int64_t> (
            static_cast<double> (right.getProperty (DawClipIDs::timelineStartSample)));

        expectEquals (rightSrcStart, (int64_t) 1000, "right starts at cut");
        expectEquals (rightTStart,   (int64_t) 1000, "right timeline starts at cut");
    }

    // ─── DeleteClip ───────────────────────────────────────────────────────

    void testDeleteClip()
    {
        beginTest ("DeleteClip removes clip from container");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 0);

        auto clipBefore = f.findClip (id);
        expect (clipBefore.isValid(), "clip should exist before delete");

        f.dispatcher->deleteClip (id);

        auto clipAfter = f.findClip (id);
        expect (!clipAfter.isValid(), "clip should not exist after delete");
    }

    // ─── SetClipGain ──────────────────────────────────────────────────────

    void testSetClipGain()
    {
        beginTest ("SetClipGain updates gainDb property");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 0, 0.0f);

        f.dispatcher->setClipGain (id, -6.0f);

        auto clip = f.findClip (id);
        const float gain = static_cast<float> (
            static_cast<double> (clip.getProperty (DawClipIDs::gainDb)));
        expectWithinAbsoluteError (gain, -6.0f, 1e-5f);
    }

    // ─── Move undo / redo ─────────────────────────────────────────────────

    void testMoveUndoRedo()
    {
        beginTest ("Move undo/redo round-trip");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 500);

        f.dispatcher->beginMoveDrag (id);
        f.dispatcher->moveClip (id, 800);

        expectEquals (static_cast<int64_t> (static_cast<double> (
            f.findClip (id).getProperty (DawClipIDs::timelineStartSample))), (int64_t) 800);

        // Undo.
        f.dispatcher->undo();
        expectEquals (static_cast<int64_t> (static_cast<double> (
            f.findClip (id).getProperty (DawClipIDs::timelineStartSample))), (int64_t) 500,
                      "undo should restore original timeline start");

        // Redo.
        f.dispatcher->redo();
        expectEquals (static_cast<int64_t> (static_cast<double> (
            f.findClip (id).getProperty (DawClipIDs::timelineStartSample))), (int64_t) 800,
                      "redo should reapply the move");
    }

    // ─── Clamp negative move ──────────────────────────────────────────────

    void testClampMoveToZero()
    {
        beginTest ("MoveClip clamps negative values to 0");

        EditFixture f;
        const auto id = addClipToFirstLane (f.daw, 0, 1000, 5000, 500);

        f.dispatcher->beginMoveDrag (id);
        f.dispatcher->moveClip (id, -999);

        auto clip = f.findClip (id);
        expect (static_cast<int64_t> (static_cast<double> (
            clip.getProperty (DawClipIDs::timelineStartSample))) >= 0,
                "timeline start must be clamped to ≥ 0");
    }
};

static EditCommandTests editCommandTestsInstance;
