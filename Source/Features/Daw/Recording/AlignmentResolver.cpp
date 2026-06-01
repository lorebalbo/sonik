#include "AlignmentResolver.h"

#include "../../Sync/PhaseLockEngine.h"

#include <cmath>

namespace Daw
{

//==============================================================================
bool AlignmentResolver::isTempoMatched (const AlignmentInputs& in) noexcept
{
    if (! in.isSynced)
        return false;
    if (in.deckBPM <= 0.0 || in.masterBPM <= 0.0)
        return false;

    // PRD-0027's normalisation band. A raw ratio inside [0.667, 1.5] means the
    // deck is tempo-locked to the master, not half/double-time mismatched
    // (§1.5.1). The SYNC latch alone would happily lock a half-time relationship,
    // so the band guards against that.
    const double ratio = in.masterBPM / in.deckBPM;
    return ratio >= 0.667 && ratio <= 1.5;
}

bool AlignmentResolver::isPhaseAligned (const AlignmentInputs& in) noexcept
{
    // PRD-0028's dead-band, imported — never re-declared (§1.5.2).
    return std::abs (in.phaseOffsetBeats) < PhaseLockEngine::convergenceThreshold;
}

std::int64_t AlignmentResolver::snapToGrid (std::int64_t pos,
                                            std::int64_t gridOrigin,
                                            double       gridInterval) noexcept
{
    if (gridInterval <= 0.0)
        return pos; // no grid to snap to

    const double beats   = static_cast<double> (pos - gridOrigin) / gridInterval;
    const double snapped = std::llround (beats) * gridInterval;
    return gridOrigin + static_cast<std::int64_t> (std::llround (snapped));
}

//==============================================================================
AlignmentResult AlignmentResolver::resolve (const AlignmentInputs& in) const
{
    // §1.5.5: no analysed beatgrid -> raw record-playhead position, no snapping,
    // no division by a zero interval.
    if (in.beatgridInterval <= 0.0)
        return { in.playheadSample, AlignmentMode::FirstBeatAnchored };

    // Grid-aligned only when the deck is BOTH tempo-matched and phase-aligned.
    if (isTempoMatched (in) && isPhaseAligned (in))
    {
        const std::int64_t start = snapToGrid (in.playheadSample, in.gridOrigin, in.gridInterval);
        return { start, AlignmentMode::GridAligned };
    }

    // FirstBeatAnchored: anchor only the first captured downbeat to the grid
    // (§1.5.3), leaving the remainder free.
    const double beatsAhead = static_cast<double> (in.sourceStartSample - in.beatgridAnchor)
                            / in.beatgridInterval;
    std::int64_t firstDownbeatSource =
        in.beatgridAnchor
        + static_cast<std::int64_t> (std::llround (std::ceil (beatsAhead) * in.beatgridInterval));

    // Clamp to >= beatgridAnchor (capture may start before the first analysed beat).
    if (firstDownbeatSource < in.beatgridAnchor)
        firstDownbeatSource = in.beatgridAnchor;

    const std::int64_t downbeatSourceOffset = firstDownbeatSource - in.sourceStartSample;

    // Deck plays at its own tempo from clip open, so source maps 1:1 to timeline.
    const std::int64_t downbeatTimeline = in.playheadSample + downbeatSourceOffset;
    const std::int64_t snappedDownbeat  = snapToGrid (downbeatTimeline, in.gridOrigin, in.gridInterval);

    const std::int64_t anchor = snappedDownbeat - downbeatSourceOffset;
    return { anchor, AlignmentMode::FirstBeatAnchored };
}

} // namespace Daw
