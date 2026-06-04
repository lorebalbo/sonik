//==============================================================================
// PRD-0088: Automation Capture Taps — implementation.
//==============================================================================

#include "AutomationCaptureTaps.h"
#include "AutomationModel.h"

namespace Daw
{

//==============================================================================
AutomationCaptureTaps::AutomationCaptureTaps (std::function<bool()>         isRecordingArmed,
                                              std::function<std::int64_t()> recordPlayhead,
                                              AutomationAppendSink&         sink)
    : isRecordingArmed_ (std::move (isRecordingArmed)),
      recordPlayhead_   (std::move (recordPlayhead)),
      sink_             (sink)
{
}

AutomationCaptureTaps::~AutomationCaptureTaps()
{
    for (auto& tree : listenedTrees_)
        tree.removeListener (this);
}

//==============================================================================
void AutomationCaptureTaps::ensureListenerOn (juce::ValueTree tree)
{
    for (auto& existing : listenedTrees_)
        if (existing == tree)
            return; // already listening on this exact node

    // The listener registers on the specific ValueTree instance, so we must add
    // it to the persistent copy we retain — not the by-value parameter, which is
    // destroyed on return (taking its registration with it).
    listenedTrees_.push_back (tree);
    listenedTrees_.back().addListener (this);
}

void AutomationCaptureTaps::registerContinuousTap (juce::ValueTree         sourceTree,
                                                   const juce::Identifier& property,
                                                   const juce::String&     owner,
                                                   const juce::String&     parameterId,
                                                   Interpolation           interpolation,
                                                   ThinningPolicy          thinning)
{
    Tap tap;
    tap.sourceTree    = sourceTree;
    tap.property      = property;
    tap.owner         = owner;
    tap.parameterId   = parameterId;
    tap.kind          = LaneKind::Continuous;
    tap.interpolation = interpolation;
    tap.thinning      = thinning;
    taps_.push_back (std::move (tap));

    ensureListenerOn (sourceTree);
}

void AutomationCaptureTaps::registerBooleanTap (juce::ValueTree         sourceTree,
                                                const juce::Identifier& property,
                                                const juce::String&     owner,
                                                const juce::String&     parameterId)
{
    Tap tap;
    tap.sourceTree  = sourceTree;
    tap.property    = property;
    tap.owner       = owner;
    tap.parameterId = parameterId;
    tap.kind        = LaneKind::Boolean;
    taps_.push_back (std::move (tap));

    ensureListenerOn (sourceTree);
}

//==============================================================================
void AutomationCaptureTaps::captureInitialValues (std::int64_t timelineSample)
{
    for (auto& tap : taps_)
    {
        const juce::var current = tap.sourceTree.getProperty (tap.property);

        if (tap.kind == LaneKind::Continuous)
        {
            tap.lastBreakpoint = sink_.appendBreakpoint (tap.owner, tap.parameterId,
                                                         timelineSample, (double) current,
                                                         tap.interpolation);
            tap.hasLast    = true;
            tap.lastSample = timelineSample;
            tap.lastValue  = (double) current;
        }
        else
        {
            sink_.appendStep (tap.owner, tap.parameterId, timelineSample, (bool) current);
            tap.hasLast    = true;
            tap.lastSample = timelineSample;
            tap.lastValue  = (bool) current ? 1.0 : 0.0;
        }
    }
}

void AutomationCaptureTaps::flush (std::int64_t timelineSample)
{
    for (auto& tap : taps_)
    {
        if (tap.kind != LaneKind::Continuous || ! tap.hasLast)
            continue;

        const double value = (double) tap.sourceTree.getProperty (tap.property);
        if (value == tap.lastValue)
            continue; // already terminates on the resting value

        if (timelineSample == tap.lastSample)
        {
            if (tap.lastBreakpoint.isValid())
                sink_.updateBreakpoint (tap.lastBreakpoint, timelineSample, value);
            tap.lastValue = value;
            continue;
        }

        tap.lastBreakpoint = sink_.appendBreakpoint (tap.owner, tap.parameterId,
                                                     timelineSample, value, tap.interpolation);
        tap.lastSample = timelineSample;
        tap.lastValue  = value;
    }
}

//==============================================================================
void AutomationCaptureTaps::valueTreePropertyChanged (juce::ValueTree& tree,
                                                      const juce::Identifier& property)
{
    // PRD-0092 re-entrancy guard: if the playback applier is currently writing
    // the authoritative tree, this change is automation-originated — ignore it so
    // it is not re-recorded into the lane (no feedback loop).
    if (applyingAutomationGuard_ && applyingAutomationGuard_())
        return;

    // Only-while-recording gate (§1.5.5): a single boolean check while disarmed,
    // before any timestamp read or append.
    if (isRecordingArmed_ && ! isRecordingArmed_())
        return;

    const std::int64_t playhead = recordPlayhead_ ? recordPlayhead_() : 0;

    for (auto& tap : taps_)
    {
        if (tap.property != property || ! (tap.sourceTree == tree))
            continue;

        // Read the CURRENT authoritative value directly from the property that
        // just changed (never a cached copy).
        const juce::var current = tree.getProperty (property);

        if (tap.kind == LaneKind::Continuous)
            handleContinuous (tap, (double) current, playhead);
        else
            handleBoolean (tap, (bool) current, playhead);
    }
}

//==============================================================================
void AutomationCaptureTaps::handleContinuous (Tap& tap, double value, std::int64_t playhead)
{
    if (! tap.hasLast)
    {
        tap.lastBreakpoint = sink_.appendBreakpoint (tap.owner, tap.parameterId,
                                                     playhead, value, tap.interpolation);
        tap.hasLast    = true;
        tap.lastSample = playhead;
        tap.lastValue  = value;
        return;
    }

    // Coalesce a same-sample burst (§1.5.6): the playhead has not advanced since
    // the last append, so update the existing breakpoint in place rather than
    // stacking a duplicate at the same timeline sample.
    if (playhead == tap.lastSample)
    {
        if (tap.lastBreakpoint.isValid())
            sink_.updateBreakpoint (tap.lastBreakpoint, playhead, value);
        tap.lastValue = value;
        return;
    }

    // Value thinning (§1.5.2): drop imperceptible jitter within the deadband.
    if (std::abs (value - tap.lastValue) <= tap.thinning.valueDeadband)
        return;

    tap.lastBreakpoint = sink_.appendBreakpoint (tap.owner, tap.parameterId,
                                                 playhead, value, tap.interpolation);
    tap.lastSample = playhead;
    tap.lastValue  = value;
}

void AutomationCaptureTaps::handleBoolean (Tap& tap, bool value, std::int64_t playhead)
{
    // Boolean steps are never thinned or coalesced — every toggle matters.
    sink_.appendStep (tap.owner, tap.parameterId, playhead, value);
    tap.hasLast    = true;
    tap.lastSample = playhead;
    tap.lastValue  = value ? 1.0 : 0.0;
}

//==============================================================================
// Production append bridge.
juce::ValueTree ModelAutomationAppendSink::appendBreakpoint (const juce::String& owner,
                                                             const juce::String& parameterId,
                                                             std::int64_t        timelineSample,
                                                             double              value,
                                                             Interpolation       interpolation)
{
    auto lane = model_.getOrCreateContinuousLane (owner, parameterId);
    if (! lane.isValid())
        return {};
    return lane.addBreakpoint (timelineSample, value, interpolation, undo_);
}

void ModelAutomationAppendSink::updateBreakpoint (juce::ValueTree breakpoint,
                                                  std::int64_t    timelineSample,
                                                  double          value)
{
    if (! breakpoint.isValid())
        return;
    breakpoint.setProperty (AutomationIDs::timelineSample, (juce::int64) timelineSample, undo_);
    breakpoint.setProperty (AutomationIDs::value,          value,                        undo_);
}

void ModelAutomationAppendSink::setBreakpointInterpolation (juce::ValueTree breakpoint,
                                                            Interpolation   interpolation)
{
    if (! breakpoint.isValid())
        return;
    breakpoint.setProperty (AutomationIDs::interpolation,
                            interpolationToString (interpolation), undo_);
}

juce::ValueTree ModelAutomationAppendSink::appendStep (const juce::String& owner,
                                                       const juce::String& parameterId,
                                                       std::int64_t        timelineSample,
                                                       bool                value)
{
    auto lane = model_.getOrCreateBooleanLane (owner, parameterId);
    if (! lane.isValid())
        return {};
    return lane.addStep (timelineSample, value, undo_);
}

void ModelAutomationAppendSink::removeStep (juce::ValueTree step)
{
    if (! step.isValid())
        return;

    // Detach the step from whatever lane node currently owns it. We address the
    // parent directly (rather than re-resolving by owner/parameterId) so the
    // caller need only hand back the node it appended.
    auto parent = step.getParent();
    if (parent.isValid())
        parent.removeChild (step, undo_);
}

} // namespace Daw
