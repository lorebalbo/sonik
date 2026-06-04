//==============================================================================
// PRD-0094: Automation Editing tests (EPIC-0011 capstone).
//
// Verifies that every automation edit is one EPIC-0010 command on the SHARED
// undo stack and is fully undo/redo-able against the PRD-0087 model:
//   * AddBreakpoint / MoveBreakpoint / DeleteBreakpoint / SetInterpolation apply,
//     undo (restoring the EXACT prior lane state — count + sample + value +
//     interpolation), and redo.
//   * Value clamp (§1.5.6): an out-of-range value stores the clamped boundary; the
//     model never holds an out-of-range value.
//   * Grid snap (§1.5.2): a sample is snapped when snap on, free when off.
//   * Shared history interleave: a clip edit and an automation edit undo in LIFO
//     order on the ONE stack; the automation undo restores the lane.
//   * Boolean: addBooleanStep / moveBooleanStep / deleteBooleanStep apply + undo +
//     redo; stateAt reflects correctly.
//   * CAPSTONE round trip (§1.5.8): capture a filter sweep (real capture path) +
//     a master-tempo nudge → applier reproduces both → edit the curves →
//     applier reproduces the EDITED curve (mixer ValueTree for filter; the
//     MasterClockManager override for tempo — never a forked tempo).
//
// All message thread; no audio-thread state touched.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Editing/EditCommands.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Playback/ArrangementPublisher.h"
#include "Features/Daw/Playback/ArrangementRecompileTrigger.h"
#include "Features/Daw/Automation/AutomationModel.h"
#include "Features/Daw/Automation/ContinuousLane.h"
#include "Features/Daw/Automation/BooleanLane.h"
#include "Features/Daw/Automation/AutomationParamRange.h"
#include "Features/Daw/Automation/AutomationApplier.h"
#include "Features/Daw/Automation/AutomationCaptureTaps.h"
#include "Features/Daw/Automation/ChannelContinuousAutomationCapture.h"
#include "Features/Daw/Automation/MasterTempoAutomationCapture.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Playback/DawTransport.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerIdentifiers.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockManager.h"
#include "Features/Deck/DeckIdentifiers.h"

#include <cstdint>
#include <vector>

using namespace Daw;

namespace
{

// A dispatcher over a real daw branch + UndoManager + recompile trigger.
struct EditHarness
{
    juce::ValueTree   root  { "SonikState" };
    juce::ValueTree   daw;
    juce::UndoManager undo  { 30000, 30 };
    ArrangementPublisher publisher;
    std::unique_ptr<ArrangementRecompileTrigger> trigger;
    std::unique_ptr<EditCommandDispatcher>       dispatcher;
    std::unique_ptr<AutomationModel>             model;

    EditHarness()
    {
        daw     = DawState::getOrCreateDawBranch (root);
        trigger = std::make_unique<ArrangementRecompileTrigger> (
            daw, ArrangementCompiler{}, publisher);
        dispatcher = std::make_unique<EditCommandDispatcher> (daw, undo, *trigger);
        model      = std::make_unique<AutomationModel> (daw, nullptr);
    }

    ContinuousLane continuous (const juce::String& owner, const juce::String& param)
    {
        return model->getContinuousLane (owner, param);
    }
    BooleanLane boolean (const juce::String& owner, const juce::String& param)
    {
        return model->getBooleanLane (owner, param);
    }
};

juce::String addClipToFirstLane (juce::ValueTree& daw, int64_t tStart)
{
    auto track = DawState::ensureTrackForDeck (daw, 0);
    auto lanes = track.getChildWithName (DawIDs::lanes);
    auto lane  = lanes.getChild (0);
    auto clips = lane.getOrCreateChildWithName (DawIDs::clips, nullptr);

    DawClip clip;
    clip.clipId              = juce::Uuid();
    clip.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
    clip.sourceFileId        = "test-source";
    clip.sourceStartSample   = 0;
    clip.sourceEndSample     = 1000;
    clip.timelineStartSample = tStart;
    clip.sourceLengthSamples = 5000;
    clip.gainDb              = 0.0f;
    clips.addChild (DawClip::toValueTree (clip), -1, nullptr);
    return clip.clipId.toString();
}

} // namespace

class AutomationEditingTests final : public juce::UnitTest
{
public:
    AutomationEditingTests() : juce::UnitTest ("Automation Editing (PRD-0094)", "Sonik") {}

    void runTest() override
    {
        addBreakpointApplyUndoRedo();
        moveBreakpointApplyUndoRedo();
        deleteBreakpointApplyUndoRedo();
        setInterpolationApplyUndoRedo();
        valueClampOnAddAndMove();
        gridSnapDrivenBySnappedSample();
        sharedHistoryInterleave();
        booleanStepApplyUndoRedo();
        capstoneRoundTrip();
    }

private:
    //==========================================================================
    void addBreakpointApplyUndoRedo()
    {
        beginTest ("AddBreakpoint applies, undoes (restores exact prior state) and redoes");

        EditHarness h;
        auto node = h.dispatcher->addBreakpoint ("A", "filter", 1000, 0.5);
        expect (node.isValid());

        auto lane = h.continuous ("A", "filter");
        expectEquals (lane.getNumBreakpoints(), 1);
        expectEquals ((int) ContinuousLane::sampleOfNode (lane.getBreakpoint (0)), 1000);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lane.getBreakpoint (0)), 0.5, 1e-9);

        h.dispatcher->undo();
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 0,
                      "undo removes the added breakpoint");

        h.dispatcher->redo();
        auto relane = h.continuous ("A", "filter");
        expectEquals (relane.getNumBreakpoints(), 1);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (relane.getBreakpoint (0)), 0.5, 1e-9);
    }

    //==========================================================================
    void moveBreakpointApplyUndoRedo()
    {
        beginTest ("MoveBreakpoint applies, undoes to exact prior sample/value, and redoes");

        EditHarness h;
        h.dispatcher->addBreakpoint ("A", "filter", 500, -0.5);
        auto bp = h.dispatcher->addBreakpoint ("A", "filter", 1500, 0.5);

        h.dispatcher->moveBreakpoint ("A", "filter", bp, 2000, -0.25);
        expectEquals ((int) ContinuousLane::sampleOfNode (bp), 2000);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (bp), -0.25, 1e-9);

        h.dispatcher->undo();
        expectEquals ((int) ContinuousLane::sampleOfNode (bp), 1500, "undo restores sample");
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (bp), 0.5, 1e-9);

        h.dispatcher->redo();
        expectEquals ((int) ContinuousLane::sampleOfNode (bp), 2000);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (bp), -0.25, 1e-9);
    }

    //==========================================================================
    void deleteBreakpointApplyUndoRedo()
    {
        beginTest ("DeleteBreakpoint removes the point; undo re-forms it; redo deletes again");

        EditHarness h;
        h.dispatcher->addBreakpoint ("A", "filter", 0,    0.0);
        auto mid = h.dispatcher->addBreakpoint ("A", "filter", 1000, 1.0);
        h.dispatcher->addBreakpoint ("A", "filter", 2000, 0.0);
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 3);

        h.dispatcher->deleteBreakpoint ("A", "filter", mid);
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 2);
        // Adjacent segment re-forms: value at 1000 is now linear between 0 and 2000.
        auto v = h.continuous ("A", "filter").evaluateAt (1000);
        expect (v.has_value());
        expectWithinAbsoluteError (*v, 0.0, 1e-9);

        h.dispatcher->undo();
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 3, "undo restores the point");
        auto vmid = h.continuous ("A", "filter").evaluateAt (1000);
        expectWithinAbsoluteError (*vmid, 1.0, 1e-9);

        h.dispatcher->redo();
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 2);
    }

    //==========================================================================
    void setInterpolationApplyUndoRedo()
    {
        beginTest ("SetInterpolation flips linear<->step on the left breakpoint, undoable");

        EditHarness h;
        auto a = h.dispatcher->addBreakpoint ("B", "gain", 0,    0.0, Interpolation::Linear);
        h.dispatcher->addBreakpoint ("B", "gain", 1000, 6.0, Interpolation::Linear);

        // Linear midpoint = 3 dB.
        expectWithinAbsoluteError (*h.continuous ("B", "gain").evaluateAt (500), 3.0, 1e-9);

        h.dispatcher->setBreakpointInterpolation ("B", "gain", a, Interpolation::Step);
        expect (ContinuousLane::interpolationOfNode (a) == Interpolation::Step);
        // Step holds the left value across the segment.
        expectWithinAbsoluteError (*h.continuous ("B", "gain").evaluateAt (500), 0.0, 1e-9);

        h.dispatcher->undo();
        expect (ContinuousLane::interpolationOfNode (a) == Interpolation::Linear, "undo restores linear");
        expectWithinAbsoluteError (*h.continuous ("B", "gain").evaluateAt (500), 3.0, 1e-9);

        h.dispatcher->redo();
        expect (ContinuousLane::interpolationOfNode (a) == Interpolation::Step);
    }

    //==========================================================================
    void valueClampOnAddAndMove()
    {
        beginTest ("Value clamp: out-of-range add/move stores the clamped boundary (model never holds OOR)");

        EditHarness h;

        // Filter native range [-1, +1]: add 5.0 -> +1, add -3.0 -> -1.
        auto hi = h.dispatcher->addBreakpoint ("A", "filter", 0, 5.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (hi), 1.0, 1e-9);
        auto lo = h.dispatcher->addBreakpoint ("A", "filter", 1000, -3.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (lo), -1.0, 1e-9);

        // Move past the edge clamps too.
        h.dispatcher->moveBreakpoint ("A", "filter", hi, 0, 42.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (hi), 1.0, 1e-9);

        // Gain native range [-60, +12]: add 100 -> +12, add -200 -> -60.
        auto g1 = h.dispatcher->addBreakpoint ("C", "gain", 0, 100.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (g1), 12.0, 1e-9);
        auto g2 = h.dispatcher->addBreakpoint ("C", "gain", 1000, -200.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (g2), -60.0, 1e-9);

        // Tempo native range [20, 300].
        auto t1 = h.dispatcher->addBreakpoint ("master", "tempo", 0, 1000.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (t1), 300.0, 1e-9);
        auto t2 = h.dispatcher->addBreakpoint ("master", "tempo", 500, 1.0);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (t2), 20.0, 1e-9);

        // Direct range helper sanity.
        expectWithinAbsoluteError (
            AutomationParamRange::forContinuousParameter ("eq.low").clamp (-999.0), -60.0, 1e-9);
    }

    //==========================================================================
    void gridSnapDrivenBySnappedSample()
    {
        beginTest ("Grid snap: a sample snapped by the transform lands on grid; free when not snapped");

        // 120 BPM default grid: samplesPerBeat = 22050.
        TimelineTransform transform (TimelineTransform::GridSnapshot{},
                                     100.0, 0, 800.0);

        const std::int64_t off = 22050 / 2 + 137; // ~half a beat plus change
        const std::int64_t snapped = transform.snapSampleToGrid (off);
        // Nearest beat boundary is beat 1 = 22050.
        expectEquals ((int) snapped, 22050);
        expect (snapped != off, "snapped sample differs from the raw (free) sample");

        // The command stores whatever sample the caller passes (snapped or free).
        EditHarness h;
        auto snappedBp = h.dispatcher->addBreakpoint ("A", "filter", snapped, 0.0);
        expectEquals ((int) ContinuousLane::sampleOfNode (snappedBp), 22050);

        auto freeBp = h.dispatcher->addBreakpoint ("A", "filter", off, 0.0);
        expectEquals ((int) ContinuousLane::sampleOfNode (freeBp), (int) off);
    }

    //==========================================================================
    void sharedHistoryInterleave()
    {
        beginTest ("Shared history: a clip edit and an automation edit undo LIFO on the one stack");

        EditHarness h;
        const auto clipId = addClipToFirstLane (h.daw, 500);

        // 1) Clip edit: move the clip.
        h.dispatcher->beginMoveDrag (clipId);
        h.dispatcher->moveClip (clipId, 800);

        // 2) Automation edit: add a breakpoint.
        auto bp = h.dispatcher->addBreakpoint ("A", "filter", 1000, 0.5);

        auto clip = ClipCmdHelpers::findClipNode (h.daw, clipId);
        expectEquals (static_cast<int64_t> (static_cast<double> (
            clip.getProperty (DawClipIDs::timelineStartSample))), (int64_t) 800);
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 1);

        // Undo #1 reverts the MOST RECENT (automation) edit; clip edit intact.
        h.dispatcher->undo();
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 0,
                      "automation undo restores the lane");
        clip = ClipCmdHelpers::findClipNode (h.daw, clipId);
        expectEquals (static_cast<int64_t> (static_cast<double> (
            clip.getProperty (DawClipIDs::timelineStartSample))), (int64_t) 800,
            "clip edit survives the automation undo (LIFO)");

        // Undo #2 reverts the clip edit.
        h.dispatcher->undo();
        clip = ClipCmdHelpers::findClipNode (h.daw, clipId);
        expectEquals (static_cast<int64_t> (static_cast<double> (
            clip.getProperty (DawClipIDs::timelineStartSample))), (int64_t) 500,
            "clip move reverts on the same shared stack");

        // Redo replays clip first, then automation.
        h.dispatcher->redo();
        clip = ClipCmdHelpers::findClipNode (h.daw, clipId);
        expectEquals (static_cast<int64_t> (static_cast<double> (
            clip.getProperty (DawClipIDs::timelineStartSample))), (int64_t) 800);
        h.dispatcher->redo();
        expectEquals (h.continuous ("A", "filter").getNumBreakpoints(), 1);
        juce::ignoreUnused (bp);
    }

    //==========================================================================
    void booleanStepApplyUndoRedo()
    {
        beginTest ("Boolean: add / move (horizontal) / delete apply + undo + redo; stateAt reflects");

        EditHarness h;
        auto s0 = h.dispatcher->addBooleanStep ("A", "keyLock", 1000, true);
        expect (s0.isValid());
        expectEquals (h.boolean ("A", "keyLock").getNumSteps(), 1);
        expect (h.boolean ("A", "keyLock").stateAt (1500) == true);
        expect (h.boolean ("A", "keyLock").stateAt (500)  == false);

        // Move horizontally to a later beat.
        h.dispatcher->moveBooleanStep ("A", "keyLock", s0, 2000);
        expectEquals ((int) BooleanLane::sampleOfNode (s0), 2000);
        expect (h.boolean ("A", "keyLock").stateAt (1500) == false, "off before the moved toggle");
        expect (h.boolean ("A", "keyLock").stateAt (2500) == true,  "on after the moved toggle");

        h.dispatcher->undo();
        expectEquals ((int) BooleanLane::sampleOfNode (s0), 1000, "undo restores step sample");

        h.dispatcher->redo();
        expectEquals ((int) BooleanLane::sampleOfNode (s0), 2000);

        // Delete + undo + redo.
        h.dispatcher->deleteBooleanStep ("A", "keyLock", s0);
        expectEquals (h.boolean ("A", "keyLock").getNumSteps(), 0);
        h.dispatcher->undo();
        expectEquals (h.boolean ("A", "keyLock").getNumSteps(), 1);
        h.dispatcher->redo();
        expectEquals (h.boolean ("A", "keyLock").getNumSteps(), 0);
    }

    //==========================================================================
    // CAPSTONE (§1.5.8): capture → playback → edit → playback, one regression.
    //==========================================================================
    void capstoneRoundTrip()
    {
        beginTest ("CAPSTONE: capture (filter sweep + tempo nudge) -> playback reproduces -> edit -> playback reflects edits");

        // ---- World: mixer + automation model + transport + master clock --------
        juce::ValueTree   root  { "SonikState" };
        MixerStateSchema  mixer { root };
        juce::ValueTree   daw   { DawState::createDawBranch() };
        root.addChild (daw, -1, nullptr);
        juce::UndoManager undo  { 30000, 30 };
        AutomationModel   model { daw, nullptr };
        DawTransport      transport;

        // A playing master deck on slot A with a 100-BPM beatgrid (for the clock).
        juce::ValueTree decks (IDs::Decks);
        juce::ValueTree deck (IDs::Deck);
        deck.setProperty (IDs::id,             "A",       nullptr);
        deck.setProperty (IDs::playbackStatus, "playing", nullptr);
        deck.setProperty (IDs::isMaster,       false,     nullptr);
        deck.setProperty (IDs::isSynced,       false,     nullptr);
        deck.setProperty (IDs::speedMultiplier, 1.0f,     nullptr);
        juce::ValueTree grid (IDs::BeatGrid);
        grid.setProperty (IDs::bpm,          100.0,       nullptr);
        grid.setProperty (IDs::anchorSample, (int64_t) 0, nullptr);
        deck.addChild (grid, -1, nullptr);
        decks.addChild (deck, -1, nullptr);
        root.addChild (decks, -1, nullptr);

        MasterClockPublisher publisher;
        MasterClockManager   clock { root, publisher };
        clock.setMaster (0);

        std::vector<double> tempoWrites;
        AutomationApplier applier {
            model, mixer, transport,
            [&clock] (double bpm) { clock.setAutomationTempoOverride (bpm); },
            [] (int, const juce::String&, bool) {}
        };

        // ---- CAPTURE: real capture path, fully scoped so the listener taps and
        // their retained ValueTree handles destruct before the edit/playback phase
        // (capture ends before editing — and keeps the leak detector deterministic).
        auto channelA = mixer.getChannelTree (0);
        bool capArmed = true;
        {
            ModelAutomationAppendSink sink { model, nullptr };
            std::int64_t capPlayhead = 0;
            AutomationCaptureTaps taps {
                [&capArmed]    { return capArmed; },
                [&capPlayhead] { return capPlayhead; },
                sink
            };
            ChannelContinuousAutomationCapture::registerTaps (taps, mixer);
            taps.setApplyingAutomationGuard (applier.makeApplyingGuard());
            taps.captureInitialValues (0);

            // Sweep the channel-A filter 0 -> 1.0 across 0..2000 samples.
            const int steps = 200;
            for (int i = 1; i <= steps; ++i)
            {
                capPlayhead = (std::int64_t) i * 10; // 10..2000
                channelA.setProperty (MixerIDs::filter, (double) i / (double) steps, nullptr);
            }
            taps.flush (2000); // terminate on the resting value (1.0)

            // CAPTURE: master-tempo nudge via the real MasterTempoAutomationCapture.
            double observedBpm = 100.0;
            MasterTempoAutomationCapture tempoCap {
                [&observedBpm] { return observedBpm; },
                [&capArmed]    { return capArmed; },
                [&capPlayhead] { return capPlayhead; },
                sink
            };
            tempoCap.setApplyingAutomationGuard (applier.makeApplyingGuard());
            capPlayhead = 0;
            observedBpm = 100.0;
            tempoCap.seedAtRecordStart();
            capPlayhead = 1000; observedBpm = 128.0; tempoCap.captureTick(); // nudge up to 128
            capPlayhead = 2000; observedBpm = 128.0; tempoCap.captureTick();
        }

        auto filterLane = model.getContinuousLane ("A", "filter");
        expect (filterLane.isValid());
        expect (filterLane.getNumBreakpoints() > 2, "captured sweep has multiple breakpoints");

        auto tempoLane = model.getContinuousLane ("master", "tempo");
        expect (tempoLane.isValid());
        expect (tempoLane.getNumBreakpoints() >= 2, "tempo nudge captured");

        // Disarm capture so the playback applier's writes are never re-recorded.
        capArmed = false;

        // ---- PLAYBACK 1: applier reproduces the captured gestures --------------
        transport.play();

        auto evalFilterCaptured = [&] (std::int64_t s)
        {
            return model.getContinuousLane ("A", "filter").evaluateAt (s);
        };

        // Mid of the sweep ~ 0.5 within decimation tolerance.
        transport.seek (1000);
        applier.tick();
        const double filterAt1000 = (double) channelA.getProperty (MixerIDs::filter);
        expectWithinAbsoluteError (filterAt1000, *evalFilterCaptured (1000), 1e-6);
        expect (std::abs (filterAt1000 - 0.5) < 0.1, "filter mid reproduced near 0.5");

        // Tempo override drives the published master BPM to 128 at/after the nudge.
        transport.seek (2000);
        applier.tick();
        expectWithinAbsoluteError (publisher.read().masterBPM, 128.0, 1e-6);

        // ---- EDIT via the SHARED command dispatcher ----------------------------
        ArrangementPublisher arrPub;
        ArrangementRecompileTrigger trigger { daw, ArrangementCompiler{}, arrPub };
        EditCommandDispatcher dispatcher { daw, undo, trigger };

        // 1) ADD a filter breakpoint forcing a specific value at sample 500.
        auto addedFilter = dispatcher.addBreakpoint ("A", "filter", 500, -1.0);
        expect (addedFilter.isValid());

        // 2) MOVE the tempo's nudge breakpoint to a higher BPM (140) and set the
        //    leading segment to step/hold so the new value is held.
        auto tempoNudge = tempoLane.getBreakpoint (tempoLane.getNumBreakpoints() - 1);
        dispatcher.moveBreakpoint ("master", "tempo", tempoNudge, 2000, 140.0);
        auto tempoFirst = tempoLane.getBreakpoint (0);
        dispatcher.setBreakpointInterpolation ("master", "tempo", tempoFirst, Interpolation::Step);

        // 3) DELETE a filter breakpoint near the start (the seed at 0 if present).
        auto firstFilter = model.getContinuousLane ("A", "filter").getBreakpoint (0);
        const int filterCountBeforeDelete = model.getContinuousLane ("A", "filter").getNumBreakpoints();
        dispatcher.deleteBreakpoint ("A", "filter", firstFilter);
        expectEquals (model.getContinuousLane ("A", "filter").getNumBreakpoints(),
                      filterCountBeforeDelete - 1);

        // 4) Add + MOVE a boolean toggle on a key-lock lane.
        auto keyStep = dispatcher.addBooleanStep ("A", "keyLock", 1000, true);
        dispatcher.moveBooleanStep ("A", "keyLock", keyStep, 1500);
        expect (model.getBooleanLane ("A", "keyLock").stateAt (1600) == true);
        expect (model.getBooleanLane ("A", "keyLock").stateAt (1400) == false);

        // ---- PLAYBACK 2: applier reflects the EDITED curve ---------------------
        // Filter now has a -1.0 breakpoint at 500 → evaluate matches the model.
        transport.seek (500);
        applier.tick();
        const double editedFilterAt500 = (double) channelA.getProperty (MixerIDs::filter);
        const auto modelFilterAt500 = model.getContinuousLane ("A", "filter").evaluateAt (500);
        expect (modelFilterAt500.has_value());
        expectWithinAbsoluteError (editedFilterAt500, *modelFilterAt500, 1e-6);
        expectWithinAbsoluteError (editedFilterAt500, -1.0, 1e-6);

        // Tempo edit: at sample 2000 the moved nudge now reads 140 BPM, driving the
        // MasterClockManager override (never a forked tempo).
        transport.seek (2000);
        applier.tick();
        expectWithinAbsoluteError (publisher.read().masterBPM, 140.0, 1e-6);

        // And before the nudge the step/hold leading segment holds the seed (100).
        transport.seek (0);
        applier.tick();
        expectWithinAbsoluteError (publisher.read().masterBPM,
                                   *model.getContinuousLane ("master", "tempo").evaluateAt (0), 1e-6);

        // ---- UNDO the whole edit batch restores the captured curves ------------
        for (int i = 0; i < 6 && dispatcher.canUndo(); ++i)
            dispatcher.undo();

        // Full undo: the deleted breakpoint is restored AND the added one removed,
        // so the lane returns to the originally CAPTURED count
        // (filterCountBeforeDelete was measured AFTER the add of one breakpoint).
        expectEquals (model.getContinuousLane ("A", "filter").getNumBreakpoints(),
                      filterCountBeforeDelete - 1,
                      "undo restores the captured filter lane breakpoint count");
        // Tempo nudge value restored to the captured 128.
        auto restoredNudge = model.getContinuousLane ("master", "tempo")
                                  .getBreakpoint (tempoLane.getNumBreakpoints() - 1);
        expectWithinAbsoluteError (ContinuousLane::valueOfNode (restoredNudge), 128.0, 1e-6);

        juce::ignoreUnused (tempoWrites);

        // Disengage the automation tempo override and release the undo history (which
        // retains detached breakpoint subtrees from the delete/move edits) before the
        // locals unwind, so the ValueTree SharedObjects are freed deterministically.
        clock.clearAutomationTempoOverride();
        undo.clearUndoHistory();
    }
};

static AutomationEditingTests automationEditingTests;
