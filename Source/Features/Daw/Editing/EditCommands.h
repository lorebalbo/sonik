#pragma once
//==============================================================================
// PRD-0083: EditCommands — command-pattern edit layer over the `daw` ValueTree.
//
// Every clip mutation (move, trim, uncrop, split, delete, gain) runs through
// this layer.  Each command:
//   1. Validates and clamps its parameters.
//   2. Applies the mutation to the ValueTree via juce::UndoManager transactions.
//   3. Triggers a debounced schedule recompile (PRD-0079).
//
// All commands run exclusively on the MESSAGE THREAD.
//
// DESIGN:
//   Commands are free functions (not class hierarchies) for brevity; they share
//   a common signature and dispatch through a thin EditCommandDispatcher that
//   holds the UndoManager and recompile trigger.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>
#include <juce_core/juce_core.h>

#include "../State/DawClipModel.h"
#include "../Model/DawClip.h"
#include "../Playback/ArrangementRecompileTrigger.h"

#include "../Automation/AutomationModel.h"
#include "../Automation/ContinuousLane.h"
#include "../Automation/BooleanLane.h"
#include "../Automation/AutomationIds.h"
#include "../Automation/AutomationParamRange.h"

namespace Daw
{

//==============================================================================
// ClipCommandHelpers — internal utilities shared by edit commands
//==============================================================================

namespace ClipCmdHelpers
{
    /// Find the clips container for a given clipId by searching all tracks/lanes.
    inline juce::ValueTree findClipNode (const juce::ValueTree& dawBranch,
                                         const juce::String&    clipIdStr)
    {
        auto tracks = dawBranch.getChildWithName (DawIDs::tracks);
        if (!tracks.isValid()) return {};

        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto track = tracks.getChild (t);
            auto lanes  = track.getChildWithName (DawIDs::lanes);
            if (!lanes.isValid()) continue;

            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto lane  = lanes.getChild (l);
                auto clips = lane.getChildWithName (DawIDs::clips);
                if (!clips.isValid()) continue;

                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    auto clip = clips.getChild (c);
                    if (clip.getProperty (DawClipIDs::clipId).toString() == clipIdStr)
                        return clip;
                }
            }
        }
        return {};
    }

    /// Minimum clip length in samples (prevents zero-length clips).
    static constexpr int64_t kMinClipLength = 64;
}

//==============================================================================
// EditCommandDispatcher — context for all edit commands
//==============================================================================

class EditCommandDispatcher
{
public:
    EditCommandDispatcher (juce::ValueTree&             dawBranch,
                           juce::UndoManager&           undoManager,
                           ArrangementRecompileTrigger& recompileTrigger)
        : daw_       (dawBranch)
        , undo_      (undoManager)
        , recompile_ (recompileTrigger)
    {}

    //--------------------------------------------------------------------------
    // MoveClip — changes timelineStartSample only; source crop unchanged.
    //--------------------------------------------------------------------------

    /// Begin a drag gesture; subsequent moveClip calls coalesce into one undo step.
    void beginMoveDrag (const juce::String& clipId)
    {
        undo_.beginNewTransaction ("Move Clip: " + clipId);
    }

    void moveClip (const juce::String& clipId, int64_t newTimelineStart)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t clamped = juce::jmax ((int64_t) 0, newTimelineStart);
        clip.setProperty (DawClipIDs::timelineStartSample, clamped, &undo_);
        recompile_.requestRecompile();
    }

    //--------------------------------------------------------------------------
    // TrimClip — shorten only (inward).  Outward extension → UncropClip.
    //--------------------------------------------------------------------------

    void beginTrimDrag (const juce::String& clipId)
    {
        undo_.beginNewTransaction ("Trim Clip: " + clipId);
    }

    /// Trim the start inward: raises sourceStartSample and shifts timelineStartSample.
    void trimClipStart (const juce::String& clipId, int64_t newSourceStart)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t sourceEnd = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));
        const int64_t sourceLen = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceLengthSamples)));
        const int64_t oldSourceStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t oldTimelineStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample)));

        // Clamp: must not exceed sourceEnd - minLength; must be ≥ 0.
        const int64_t clamped = juce::jlimit ((int64_t) 0,
                                              sourceEnd - ClipCmdHelpers::kMinClipLength,
                                              newSourceStart);
        // Trim-shorten only: do not let trim go before current start.
        const int64_t effective = juce::jmax (clamped, oldSourceStart);

        const int64_t delta = effective - oldSourceStart;
        clip.setProperty (DawClipIDs::sourceStartSample,   effective,                    &undo_);
        clip.setProperty (DawClipIDs::timelineStartSample, oldTimelineStart + delta,     &undo_);
        juce::ignoreUnused (sourceLen);
        recompile_.requestRecompile();
    }

    /// Trim the end inward: lowers sourceEndSample.
    void trimClipEnd (const juce::String& clipId, int64_t newSourceEnd)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t sourceStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t oldSourceEnd = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));

        // Clamp: must not go below sourceStart + minLength; trim-shorten only.
        const int64_t clamped  = juce::jmin (newSourceEnd, oldSourceEnd);
        const int64_t effective = juce::jmax (clamped, sourceStart + ClipCmdHelpers::kMinClipLength);

        clip.setProperty (DawClipIDs::sourceEndSample, effective, &undo_);
        recompile_.requestRecompile();
    }

    //--------------------------------------------------------------------------
    // UncropClip — extend crop window outward (reveal more source).
    //--------------------------------------------------------------------------

    void beginUncropDrag (const juce::String& clipId)
    {
        undo_.beginNewTransaction ("Uncrop Clip: " + clipId);
    }

    /// Extend start leftward (earlier in source): lowers sourceStartSample toward 0.
    void uncropClipStart (const juce::String& clipId, int64_t newSourceStart)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t oldSourceStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t sourceEnd = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));
        const int64_t oldTimelineStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample)));

        // Uncrop (extend outward): newSourceStart must be < oldSourceStart.
        // Clamp to [0, sourceEnd - minLength].
        const int64_t clamped  = juce::jlimit ((int64_t) 0,
                                               sourceEnd - ClipCmdHelpers::kMinClipLength,
                                               newSourceStart);
        // Only uncrop (extend): do not let it go past current start.
        const int64_t effective = juce::jmin (clamped, oldSourceStart);

        const int64_t delta = effective - oldSourceStart; // negative = extending left
        clip.setProperty (DawClipIDs::sourceStartSample,   effective,                    &undo_);
        clip.setProperty (DawClipIDs::timelineStartSample, oldTimelineStart + delta,     &undo_);
        recompile_.requestRecompile();
    }

    /// Extend end rightward: raises sourceEndSample toward sourceLengthSamples.
    void uncropClipEnd (const juce::String& clipId, int64_t newSourceEnd)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t sourceStart       = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t oldSourceEnd      = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));
        const int64_t sourceLengthSamples = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceLengthSamples)));

        // Only uncrop (extend outward): new end must be > old end.
        const int64_t clamped   = juce::jmin (newSourceEnd, sourceLengthSamples);
        const int64_t extended  = juce::jmax (clamped, oldSourceEnd);
        const int64_t effective = juce::jmax (extended, sourceStart + ClipCmdHelpers::kMinClipLength);

        clip.setProperty (DawClipIDs::sourceEndSample, effective, &undo_);
        recompile_.requestRecompile();
    }

    //--------------------------------------------------------------------------
    // SplitClip — cut into two contiguous clips at cutSample (project time).
    //--------------------------------------------------------------------------

    void splitClip (const juce::String& clipId, int64_t cutTimelineSample)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        const int64_t tStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample)));
        const int64_t srcStart = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
        const int64_t srcEnd   = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));
        const int64_t srcLen   = static_cast<int64_t> (
            static_cast<double> (clip.getProperty (DawClipIDs::sourceLengthSamples)));
        const float   gainDb   = static_cast<float> (
            static_cast<double> (clip.getProperty (DawClipIDs::gainDb)));
        const juce::String sourceFileId =
            clip.getProperty (DawClipIDs::sourceFileId).toString();
        const juce::String laneId =
            clip.getProperty (DawClipIDs::laneId).toString();

        // Compute cut position in source.
        const int64_t cutOffset = cutTimelineSample - tStart;
        const int64_t cutSourceSample = srcStart + cutOffset;

        // Validate: cut must be inside the clip with enough room for both halves.
        if (cutSourceSample <= srcStart + ClipCmdHelpers::kMinClipLength)  return;
        if (cutSourceSample >= srcEnd   - ClipCmdHelpers::kMinClipLength)  return;

        undo_.beginNewTransaction ("Split Clip");

        // Left half: same start, truncated at cut.
        clip.setProperty (DawClipIDs::sourceEndSample, cutSourceSample, &undo_);

        // Right half: new clip node.
        DawClip right;
        right.clipId              = juce::Uuid();
        right.laneId              = juce::Uuid (laneId);
        right.sourceFileId        = sourceFileId;
        right.sourceStartSample   = cutSourceSample;
        right.sourceEndSample     = srcEnd;
        right.timelineStartSample = cutTimelineSample;
        right.sourceLengthSamples = srcLen;
        right.gainDb              = gainDb;

        auto rightNode = DawClip::toValueTree (right);

        // Insert the right half next to the left half.
        auto parentContainer = clip.getParent();
        if (parentContainer.isValid())
        {
            const int leftIdx = parentContainer.indexOf (clip);
            parentContainer.addChild (rightNode, leftIdx + 1, &undo_);
        }

        recompile_.requestRecompile();
    }

    //--------------------------------------------------------------------------
    // DeleteClip — removes a clip from its container.
    //--------------------------------------------------------------------------

    void deleteClip (const juce::String& clipId)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        undo_.beginNewTransaction ("Delete Clip");
        auto parent = clip.getParent();
        if (parent.isValid())
            parent.removeChild (clip, &undo_);

        recompile_.requestRecompile();
    }

    //--------------------------------------------------------------------------
    // SetClipGain — adjusts per-clip gain in dB.
    //--------------------------------------------------------------------------

    void setClipGain (const juce::String& clipId, float gainDb)
    {
        auto clip = ClipCmdHelpers::findClipNode (daw_, clipId);
        if (!clip.isValid()) return;

        undo_.beginNewTransaction ("Set Clip Gain");
        clip.setProperty (DawClipIDs::gainDb, gainDb, &undo_);
        recompile_.requestRecompile();
    }

    //==========================================================================
    // PRD-0094: Automation edit commands.
    //
    // Every automation edit is one EPIC-0010 command on the SAME shared undo
    // stack as the clip edits above (one interleaved history). Each command:
    //   - begins a new undo transaction,
    //   - mutates the lane via the PRD-0087 mutators with `&undo_` (so the change
    //     is fully undo/redo-able and the sorted invariant is preserved),
    //   - clamps every value to the parameter's NATIVE range (hard invariant:
    //     the model never holds an out-of-range value — §1.5.6),
    //   - clamps every timeline sample to >= 0 (the caller grid-snaps first).
    //
    // Automation-only edits do NOT trigger an arrangement recompile (the applier
    // re-reads the model on its next tick — §1.5.7). Message thread only.
    //==========================================================================

    // AddBreakpoint — double-click on empty continuous-lane region (§1.5.1).
    juce::ValueTree addBreakpoint (const juce::String& owner,
                                   const juce::String& parameterId,
                                   std::int64_t        timelineSample,
                                   double              value,
                                   Interpolation       interpolation = Interpolation::Linear)
    {
        auto lane = automationModel().getOrCreateContinuousLane (owner, parameterId);
        if (! lane.isValid())
            return {};

        undo_.beginNewTransaction ("Automation: Add Breakpoint");
        const std::int64_t s = juce::jmax ((std::int64_t) 0, timelineSample);
        const double       v = clampValue (parameterId, value);
        return lane.addBreakpoint (s, v, interpolation, &undo_);
    }

    // MoveBreakpoint — ONE command on drag end (not per mouse-move, §1.4).
    void moveBreakpoint (const juce::String& owner,
                         const juce::String& parameterId,
                         juce::ValueTree     breakpointNode,
                         std::int64_t        newSample,
                         double              newValue)
    {
        auto lane = automationModel().getContinuousLane (owner, parameterId);
        if (! lane.isValid() || ! breakpointNode.isValid())
            return;

        undo_.beginNewTransaction ("Automation: Move Breakpoint");
        const std::int64_t s = juce::jmax ((std::int64_t) 0, newSample);
        const double       v = clampValue (parameterId, newValue);
        lane.moveBreakpoint (breakpointNode, s, v, &undo_);
    }

    // DeleteBreakpoint — adjacent segment re-forms automatically (§1.4).
    void deleteBreakpoint (const juce::String& owner,
                           const juce::String& parameterId,
                           juce::ValueTree     breakpointNode)
    {
        auto lane = automationModel().getContinuousLane (owner, parameterId);
        if (! lane.isValid() || ! breakpointNode.isValid())
            return;

        undo_.beginNewTransaction ("Automation: Delete Breakpoint");
        lane.removeBreakpoint (breakpointNode, &undo_);
    }

    // SetInterpolation — per-segment, stored on the LEFT breakpoint (§1.5.3).
    void setBreakpointInterpolation (const juce::String& owner,
                                     const juce::String& parameterId,
                                     juce::ValueTree     breakpointNode,
                                     Interpolation       interpolation)
    {
        auto lane = automationModel().getContinuousLane (owner, parameterId);
        if (! lane.isValid() || ! breakpointNode.isValid())
            return;

        undo_.beginNewTransaction ("Automation: Set Interpolation");
        lane.setInterpolation (breakpointNode, interpolation, &undo_);
    }

    // AddBooleanStep — double-click adds a toggle (§1.5.1 / §1.5.5).
    juce::ValueTree addBooleanStep (const juce::String& owner,
                                    const juce::String& parameterId,
                                    std::int64_t        sample,
                                    bool                state)
    {
        auto lane = automationModel().getOrCreateBooleanLane (owner, parameterId);
        if (! lane.isValid())
            return {};

        undo_.beginNewTransaction ("Automation: Add Toggle");
        const std::int64_t s = juce::jmax ((std::int64_t) 0, sample);
        return lane.addStep (s, state, &undo_);
    }

    // MoveBooleanStep — horizontal-only, grid-snapped by the caller (§1.5.5).
    void moveBooleanStep (const juce::String& owner,
                          const juce::String& parameterId,
                          juce::ValueTree     stepNode,
                          std::int64_t        newSample)
    {
        auto lane = automationModel().getBooleanLane (owner, parameterId);
        if (! lane.isValid() || ! stepNode.isValid())
            return;

        undo_.beginNewTransaction ("Automation: Move Toggle");
        const std::int64_t s = juce::jmax ((std::int64_t) 0, newSample);
        lane.moveStep (stepNode, s, &undo_);
    }

    // DeleteBooleanStep — the boolean equivalent of DeleteBreakpoint (§1.4).
    void deleteBooleanStep (const juce::String& owner,
                            const juce::String& parameterId,
                            juce::ValueTree     stepNode)
    {
        auto lane = automationModel().getBooleanLane (owner, parameterId);
        if (! lane.isValid() || ! stepNode.isValid())
            return;

        undo_.beginNewTransaction ("Automation: Delete Toggle");
        lane.removeStep (stepNode, &undo_);
    }

    // The native clamp range chosen for a parameterId (exposed for the UI's
    // value-drag clamp so the on-screen preview matches the committed value).
    static double clampValue (const juce::String& parameterId, double value)
    {
        return AutomationParamRange::forContinuousParameter (parameterId).clamp (value);
    }

    //--------------------------------------------------------------------------
    // Undo / redo delegation
    //--------------------------------------------------------------------------

    bool canUndo() const noexcept { return undo_.canUndo(); }
    bool canRedo() const noexcept { return undo_.canRedo(); }

    void undo()
    {
        undo_.undo();
        recompile_.requestRecompile();
    }

    void redo()
    {
        undo_.redo();
        recompile_.requestRecompile();
    }

    juce::UndoManager& undoManager() noexcept { return undo_; }

private:
    // A fresh AutomationModel wrapper over the daw branch. The lane CONTAINER /
    // empty-lane scaffolding is structural and created WITHOUT the undo manager
    // (an empty lane carries no user data); only the breakpoint/step MUTATIONS are
    // recorded as undoable actions (the mutators below are always called with
    // `&undo_`). This keeps the undo history to user-meaningful edits and avoids
    // retaining structural subtrees in the history.
    AutomationModel automationModel()
    {
        return AutomationModel (daw_, nullptr);
    }

    juce::ValueTree&              daw_;
    juce::UndoManager&            undo_;
    ArrangementRecompileTrigger&  recompile_;
};

} // namespace Daw
