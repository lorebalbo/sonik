#pragma once
//==============================================================================
// PRD-0071: Recording Session Controller & Record Playhead Clock.
//
// Owns the recording lifecycle (Stopped / Armed / Recording) and the record
// playhead clock for EPIC-0009. Message-thread ONLY: it reads the master clock
// exclusively through the existing lock-free snapshot mechanisms (PRD-0026 /
// PRD-0064) and adds NO audio-thread code. It writes its lifecycle state and the
// live playhead position into the existing `daw` ValueTree (PRD-0063) under a
// `recording` child node, so the UI (PRD-0078) and the capture engine (PRD-0072
// / PRD-0073) observe a single source of truth via JUCE listeners.
//
// SCOPE: clock + state machine only. This PRD does NOT consume performance
// events (PRD-0072), create/place clips (PRD-0073), align to the grid
// (PRD-0074), or add UI (PRD-0078).
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../State/DawState.h"
#include "../Model/MasterGridService.h"

#include <cstdint>
#include <functional>
#include <memory>

class MasterClockManager;

namespace Daw
{

//==============================================================================
// ValueTree identifiers for the `recording` subtree under the `daw` branch.
namespace RecordingIDs
{
    const juce::Identifier recording      { "recording" };       // child of `daw`
    const juce::Identifier state          { "state" };           // string enum
    const juce::Identifier timelineOrigin { "timelineOrigin" };  // int64 project samples
    const juce::Identifier playheadSample { "playheadSample" };  // int64 project samples

    // Canonical string values for `recording.state`.
    inline constexpr const char* kStateStopped   = "Stopped";
    inline constexpr const char* kStateArmed     = "Armed";
    inline constexpr const char* kStateRecording = "Recording";
}

//==============================================================================
// The three lifecycle states. The controller starts in `Stopped`.
enum class RecordingState
{
    Stopped = 0,
    Armed,
    Recording
};

juce::String   recordingStateToString (RecordingState s);
RecordingState recordingStateFromString (const juce::String& s);

//==============================================================================
// Injectable clock source. The controller advances its record playhead by
// sampling this on each tick. Two advance modes are expressed through one set of
// readings so consumers never need to know which mode is active:
//   - master-clock-anchored: while a deck drives the grid, `masterTimelineSample`
//     reports a monotonic project-sample position derived from musical time.
//   - dormant real-time: while the master clock is dormant, the controller
//     advances by `wallClockSeconds` delta * `projectSampleRate`.
struct RecordingClock
{
    virtual ~RecordingClock() = default;

    // True while a deck is driving the master grid (master clock running).
    virtual bool isMasterRunning() const = 0;

    // Monotonic project-sample position anchored to musical time. Only sampled
    // while `isMasterRunning()` is true.
    virtual std::int64_t masterTimelineSample() const = 0;

    // Monotonic wall-clock seconds. Sampled while the master clock is dormant.
    virtual double wallClockSeconds() const = 0;

    // The project sample rate every *Sample field is expressed against.
    virtual double projectSampleRate() const = 0;
};

//==============================================================================
// Production clock built from the existing master-clock stack. It reads the
// "is playing" and sample-rate facts from the MasterGridService snapshot and
// uses the high-resolution wall clock for dormant advance. The musical
// (master-anchored) timeline position is supplied by an injected provider — the
// DAW projection now-line (PRD-0069) — which PRD-0078 wires in. Until a provider
// is set the master-anchored position falls back to a wall-clock projection, so
// the playhead still advances rather than freezing.
class LiveRecordingClock final : public RecordingClock
{
public:
    LiveRecordingClock (MasterClockManager& clockManager, MasterGridService& grid)
        : clockManager_ (clockManager), grid_ (grid)
    {
    }

    // Inject the DAW projection now-line (PRD-0069) so the master-anchored
    // advance tracks musical time. Message-thread only.
    void setMasterTimelineProvider (std::function<std::int64_t()> provider)
    {
        masterTimelineProvider_ = std::move (provider);
    }

    bool isMasterRunning() const override
    {
        return grid_.snapshotGrid().isPlaying;
    }

    std::int64_t masterTimelineSample() const override
    {
        if (masterTimelineProvider_)
            return masterTimelineProvider_();

        // No now-line wired yet: project the wall clock so we still advance.
        return static_cast<std::int64_t> (std::llround (wallClockSeconds() * projectSampleRate()));
    }

    double wallClockSeconds() const override
    {
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    double projectSampleRate() const override
    {
        const auto sr = grid_.snapshotGrid().sampleRate;
        return sr > 0.0 ? sr : DawState::kProjectSampleRate;
    }

private:
    [[maybe_unused]] MasterClockManager& clockManager_; // dependency per PRD-0071; running state read via grid_
    MasterGridService&                   grid_;
    std::function<std::int64_t()>        masterTimelineProvider_;
};

//==============================================================================
class RecordingSessionController
{
public:
    // Production constructor: explicit dependencies, no singletons. When
    // `clockOverride` is null an internal LiveRecordingClock is created from the
    // master-clock stack. Tests inject a fake clock via `clockOverride`.
    RecordingSessionController (juce::ValueTree     dawBranch,
                                MasterGridService&  grid,
                                MasterClockManager& clockManager,
                                RecordingClock*     clockOverride = nullptr);

    ~RecordingSessionController() = default;

    RecordingSessionController (const RecordingSessionController&)            = delete;
    RecordingSessionController& operator= (const RecordingSessionController&) = delete;

    //--------------------------------------------------------------------------
    // Transitions (message thread).

    // Stopped -> Armed. Resets the playhead to 0, re-baselines the clock, and
    // begins advancing the playhead (so dormant silence before the first audio
    // is faithfully represented). No-op from any state other than Stopped.
    void arm();

    // Armed -> Recording. Stamps `timelineOrigin` at the current playhead (the
    // instant the first real audio event occurs, surfaced by PRD-0072) and keeps
    // advancing the playhead. No-op from any state other than Armed.
    void beginCapture();

    // Armed -> Stopped (nothing captured) or Recording -> Stopped. In the
    // Recording case this only performs the transition and raises the stop
    // signal; open-clip finalisation is delegated to PRD-0073. No-op from
    // Stopped.
    void stop();

    //--------------------------------------------------------------------------
    // Clock tick (message thread). Drives the record playhead while Armed or
    // Recording. PRD-0078 wires this to the DAW projection cadence (PRD-0069).
    // Public so it is unit-testable with an injected fake clock.
    void tick();

    //--------------------------------------------------------------------------
    // Queries.
    RecordingState state() const noexcept { return state_; }

    // Live record playhead as an int64 project-sample count. 0 at arm(),
    // monotonically increasing while Armed/Recording.
    std::int64_t currentTimelinePosition() const noexcept { return recordPlayheadSample_; }

    // Project sample at which capture (the first audio event) began; set on
    // beginCapture(). 0 before the first beginCapture() of a take.
    std::int64_t timelineOrigin() const noexcept { return timelineOrigin_; }

    // The "stop signal" PRD-0073 consumes to close open clips. Monotonic counter
    // that increments once per Recording->Stopped transition.
    std::uint64_t stopSignalCount() const noexcept { return stopSignalCount_; }

private:
    void rebaselineClock();
    void advancePlayhead();
    void writeStateToTree();
    void writePlayheadToTree();
    juce::ValueTree recordingNode();

    juce::ValueTree     dawBranch_;

    std::unique_ptr<LiveRecordingClock> ownedClock_; // used when no override given
    RecordingClock&                     clock_;

    RecordingState state_                = RecordingState::Stopped;
    std::int64_t   recordPlayheadSample_ = 0;
    std::int64_t   timelineOrigin_       = 0;
    std::uint64_t  stopSignalCount_      = 0;

    // Clock-advance baselines, refreshed each tick.
    bool         lastWasRunning_   = false;
    std::int64_t lastMasterSample_ = 0;
    double       lastWallSeconds_  = 0.0;
};

} // namespace Daw
