#pragma once
//==============================================================================
// PRD-0064: Master Grid Service & Master-Clock Reconciliation
//
// A read-only, MESSAGE-THREAD-ONLY service that turns the authoritative
// `MasterClockSnapshot` (published by MasterClockManager via the SeqLock in
// MasterClockPublisher, PRD-0026) into a DAW bar/beat grid plus the single,
// shared samples<->beats<->bars conversion implementation used by every DAW
// consumer (PRD-0065 transform, PRD-0066 ruler, clip snapping).
//
// It NEVER publishes, mutates, or caches a competing tempo/phase value: the
// master clock remains the one source of tempo truth (EPIC-0003 invariant).
//
// THREADING: every method here runs on the message thread. The only lock taken
// is the bounded SeqLock retry inside MasterClockPublisher::read(). No audio-
// thread code, no allocation in the conversion helpers, no I/O.
//==============================================================================

#include <cstdint>
#include <functional>
#include <vector>

#include "../State/DawState.h"            // kBeatsPerBar, kProjectSampleRate
#include "../../Sync/MasterClockPublisher.h"
#include "../../Sync/MasterClockSnapshot.h"

namespace Daw
{

class MasterGridService
{
public:
    //--------------------------------------------------------------------------
    // Classification of a single grid line (PRD-0064 §1.5.5).
    //--------------------------------------------------------------------------
    enum class GridLineKind
    {
        Bar,        // beat index is a multiple of kBeatsPerBar
        Beat,       // whole-beat line that is not a bar boundary
        SubBeat     // a sub-division between whole beats
    };

    struct GridLine
    {
        std::int64_t  sample   = 0;   // sample position of the line
        double        beat     = 0.0; // beat index relative to phase origin (can be < 0)
        GridLineKind  kind     = GridLineKind::Beat;
    };

    //--------------------------------------------------------------------------
    // A coherent, immutable view of the grid math for one read of the clock.
    // Returned by snapshotGrid() so callers that need many conversions against
    // one snapshot can avoid re-reading the SeqLock per call.
    //--------------------------------------------------------------------------
    struct GridContext
    {
        double        bpm                 = 0.0;
        double        sampleRate          = DawState::kProjectSampleRate;
        double        samplesPerBeat      = 0.0;
        std::int64_t  phaseOriginSample   = 0;
        bool          isPlaying           = false;
        bool          isFallback          = false;

        // Pure conversions (no allocation, no locking, finite for all inputs).
        std::int64_t beatsToSamples (double beats) const;
        double       samplesToBeats (std::int64_t sample) const;
        double       barForSample   (std::int64_t sample) const;
        double       samplesToBars  (std::int64_t sample) const;
    };

    //--------------------------------------------------------------------------
    // Fallback grid used when masterBPM <= 0 (PRD-0064 §1.5.2).
    //--------------------------------------------------------------------------
    static constexpr double kFallbackBpm = 120.0;

    //--------------------------------------------------------------------------
    // Constructor injection (no singletons): the read side of the master clock
    // SeqLock, plus a provider that returns the live audio device sample rate.
    // The service owns neither; it only reads. If the provider is empty or
    // returns <= 0, the conversions fall back to kProjectSampleRate per call.
    //--------------------------------------------------------------------------
    MasterGridService (const MasterClockPublisher& clockPublisher,
                       std::function<double()>      sampleRateProvider);

    MasterGridService (const MasterGridService&)            = delete;
    MasterGridService& operator= (const MasterGridService&) = delete;

    //--------------------------------------------------------------------------
    // Reads one coherent MasterClockSnapshot and resolves the grid math for it
    // (tempo, sample rate, samples-per-beat, phase origin, fallback flag).
    //--------------------------------------------------------------------------
    GridContext snapshotGrid() const;

    //--------------------------------------------------------------------------
    // Emits the ordered set of grid lines within [firstSample, lastSample]
    // (inclusive), each tagged Bar / Beat / SubBeat. subBeatDivision == 1 emits
    // whole beats only; 2 = 1/2-beat lines, 4 = 1/4-beat, etc. The snapshot is
    // read exactly once for the whole call so all emitted lines are coherent.
    //--------------------------------------------------------------------------
    std::vector<GridLine> sampleGrid (std::int64_t firstSample,
                                      std::int64_t lastSample,
                                      int          subBeatDivision = 1) const;

    //--------------------------------------------------------------------------
    // Convenience conversions that each read one fresh snapshot.
    //--------------------------------------------------------------------------
    std::int64_t beatsToSamples (double beats) const;
    double       samplesToBeats (std::int64_t sample) const;
    double       barForSample   (std::int64_t sample) const;
    double       samplesToBars  (std::int64_t sample) const;

    //--------------------------------------------------------------------------
    // True when the most recent snapshot read had masterBPM <= 0 and the
    // service substituted the kFallbackBpm grid (PRD-0064 §1.5.2).
    //--------------------------------------------------------------------------
    bool isFallbackGrid() const;

private:
    double currentSampleRate() const;

    const MasterClockPublisher& clockPublisher_;
    std::function<double()>     sampleRateProvider_;
};

} // namespace Daw
