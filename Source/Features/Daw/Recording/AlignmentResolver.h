#pragma once
//==============================================================================
// PRD-0074: Grid Alignment Resolver.
//
// Pure, message-thread placement arithmetic invoked by the Clip Placement
// Engine (PRD-0073) at clip OPEN only. Given a deck's current sync/grid state
// it returns a single timeline start sample and an AlignmentMode. It owns NO
// new sync vocabulary: the "tempo-matched" and "phase-aligned" predicates
// reuse EPIC-0003's existing definitions verbatim (PRD-0027 ratio band +
// PRD-0028 convergenceThreshold). The decision is computed once and is
// immutable for the clip's lifetime.
//
// This is a pure function of its POD inputs: no ValueTree access, no globals,
// no singletons, no allocation, no audio-thread path.
//==============================================================================

#include <cstdint>

namespace Daw
{

//==============================================================================
enum class AlignmentMode
{
    GridAligned,        // deck tempo-matched + phase-aligned: snap start to beat grid
    FirstBeatAnchored   // foreign tempo / unsynced / no beatgrid: anchor first downbeat only
};

//==============================================================================
// All values are assembled by the caller (PRD-0073) from data already on the
// message thread. Sample-domain fields are project-timeline samples unless the
// name says "source".
struct AlignmentInputs
{
    std::int64_t playheadSample      = 0;   // current record playhead (PRD-0064)
    double       deckBPM             = 0.0;
    double       masterBPM           = 0.0;
    bool         isSynced            = false; // PRD-0027 SYNC latch
    double       phaseOffsetBeats    = 0.0;   // PRD-0028 published phase offset, read once
    std::int64_t beatgridAnchor      = 0;     // PRD-0008 deck beatgrid (source samples)
    double       beatgridInterval    = 0.0;   // samples per beat; <= 0 means "no beatgrid"
    std::int64_t sourceStartSample   = 0;     // capture source position at open
    std::int64_t gridOrigin          = 0;     // master grid phase origin (timeline samples)
    double       gridInterval        = 0.0;   // master grid samples per beat
};

struct AlignmentResult
{
    std::int64_t  timelineStartSample = 0;
    AlignmentMode mode                = AlignmentMode::FirstBeatAnchored;
};

//==============================================================================
class AlignmentResolver
{
public:
    AlignmentResolver() = default;

    // The single pure entry point. Identical inputs yield identical outputs.
    AlignmentResult resolve (const AlignmentInputs& in) const;

private:
    // PRD-0027 reuse: "tempo-matched" == SYNC engaged AND the BPM ratio is not a
    // half/double-time relationship (raw ratio inside the [0.667, 1.5] band).
    static bool isTempoMatched (const AlignmentInputs& in) noexcept;

    // PRD-0028 reuse: phase-aligned == |phaseOffsetBeats| < convergenceThreshold.
    static bool isPhaseAligned (const AlignmentInputs& in) noexcept;

    // gridOrigin + round((pos - gridOrigin) / gridInterval) * gridInterval.
    static std::int64_t snapToGrid (std::int64_t pos,
                                    std::int64_t gridOrigin,
                                    double       gridInterval) noexcept;
};

} // namespace Daw
