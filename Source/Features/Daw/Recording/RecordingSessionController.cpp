#include "RecordingSessionController.h"

#include "../../Sync/MasterClockManager.h"

#include <cmath>

namespace Daw
{

juce::String recordingStateToString (RecordingState s)
{
    switch (s)
    {
        case RecordingState::Armed:     return RecordingIDs::kStateArmed;
        case RecordingState::Recording: return RecordingIDs::kStateRecording;
        case RecordingState::Stopped:
        default:                        return RecordingIDs::kStateStopped;
    }
}

RecordingState recordingStateFromString (const juce::String& s)
{
    if (s == RecordingIDs::kStateArmed)     return RecordingState::Armed;
    if (s == RecordingIDs::kStateRecording) return RecordingState::Recording;
    return RecordingState::Stopped;
}

//==============================================================================
RecordingSessionController::RecordingSessionController (juce::ValueTree     dawBranch,
                                                        MasterGridService&  grid,
                                                        MasterClockManager& clockManager,
                                                        RecordingClock*     clockOverride)
    : dawBranch_ (std::move (dawBranch)),
      grid_ (grid),
      clockManager_ (clockManager),
      ownedClock_ (clockOverride == nullptr
                       ? std::make_unique<LiveRecordingClock> (clockManager, grid)
                       : nullptr),
      clock_ (clockOverride != nullptr ? *clockOverride
                                       : static_cast<RecordingClock&> (*ownedClock_))
{
    // Mirror the initial Stopped state into the model so listeners have a value.
    writeStateToTree();
    writePlayheadToTree();
}

//==============================================================================
void RecordingSessionController::arm()
{
    if (state_ != RecordingState::Stopped)
        return; // no-op: arming is only valid from Stopped.

    // Fresh take: playhead and origin reset to 0 (PRD-0071 §1.5.1 / §1.5.6).
    state_                = RecordingState::Armed;
    recordPlayheadSample_ = 0;
    timelineOrigin_       = 0;

    rebaselineClock();

    writeStateToTree();
    writePlayheadToTree();
}

void RecordingSessionController::beginCapture()
{
    if (state_ != RecordingState::Armed)
        return; // no-op: capture only promotes from Armed.

    // The first real audio event: stamp where capture begins on the take
    // timeline (the playhead has been advancing since arm(), so a deck that
    // starts 30s after Record lands at its true offset, not at 0).
    state_          = RecordingState::Recording;
    timelineOrigin_ = recordPlayheadSample_;

    writeStateToTree();
    writePlayheadToTree();
}

void RecordingSessionController::stop()
{
    if (state_ == RecordingState::Stopped)
        return; // no-op.

    // In the Recording case raise the stop signal PRD-0073 consumes to finalise
    // open clips. This controller performs no clip work itself.
    if (state_ == RecordingState::Recording)
        ++stopSignalCount_;

    state_ = RecordingState::Stopped;

    writeStateToTree();
    writePlayheadToTree();
}

//==============================================================================
void RecordingSessionController::tick()
{
    if (state_ != RecordingState::Armed && state_ != RecordingState::Recording)
        return;

    advancePlayhead();
    writePlayheadToTree();
}

//==============================================================================
void RecordingSessionController::rebaselineClock()
{
    lastWasRunning_   = clock_.isMasterRunning();
    lastMasterSample_ = clock_.masterTimelineSample();
    lastWallSeconds_  = clock_.wallClockSeconds();
}

void RecordingSessionController::advancePlayhead()
{
    const bool         running = clock_.isMasterRunning();
    const std::int64_t master  = clock_.masterTimelineSample();
    const double       wall    = clock_.wallClockSeconds();

    if (running && lastWasRunning_)
    {
        // Master-clock-anchored: advance by the musical-time delta.
        const std::int64_t delta = master - lastMasterSample_;
        if (delta > 0)
            recordPlayheadSample_ += delta;
    }
    else if (! running && ! lastWasRunning_)
    {
        // Dormant: advance in real time at the project sample rate.
        const double dt = wall - lastWallSeconds_;
        if (dt > 0.0)
            recordPlayheadSample_ += static_cast<std::int64_t> (
                std::llround (dt * clock_.projectSampleRate()));
    }
    // else: the master clock just switched mode this tick. Skip advance for one
    // tick and re-baseline so neither domain produces a spurious jump.

    lastWasRunning_   = running;
    lastMasterSample_ = master;
    lastWallSeconds_  = wall;
}

//==============================================================================
juce::ValueTree RecordingSessionController::recordingNode()
{
    auto node = dawBranch_.getOrCreateChildWithName (RecordingIDs::recording, nullptr);
    return node;
}

void RecordingSessionController::writeStateToTree()
{
    if (! dawBranch_.isValid())
        return;

    auto node = recordingNode();
    node.setProperty (RecordingIDs::state, recordingStateToString (state_), nullptr);
    node.setProperty (RecordingIDs::timelineOrigin, timelineOrigin_, nullptr);
}

void RecordingSessionController::writePlayheadToTree()
{
    if (! dawBranch_.isValid())
        return;

    auto node = recordingNode();
    node.setProperty (RecordingIDs::playheadSample, recordPlayheadSample_, nullptr);
}

} // namespace Daw
