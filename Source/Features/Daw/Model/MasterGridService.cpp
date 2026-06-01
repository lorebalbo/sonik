//==============================================================================
// PRD-0064: Master Grid Service implementation.
//==============================================================================

#include "MasterGridService.h"

#include <cmath>

namespace Daw
{

//==============================================================================
// GridContext conversions
//==============================================================================

std::int64_t MasterGridService::GridContext::beatsToSamples (double beats) const
{
    // sample = phaseOrigin + round(beats * samplesPerBeat).
    return phaseOriginSample + static_cast<std::int64_t> (std::llround (beats * samplesPerBeat));
}

double MasterGridService::GridContext::samplesToBeats (std::int64_t sample) const
{
    // samplesPerBeat is guaranteed > 0 by snapshotGrid() (fallback substitution).
    return static_cast<double> (sample - phaseOriginSample) / samplesPerBeat;
}

double MasterGridService::GridContext::barForSample (std::int64_t sample) const
{
    return samplesToBeats (sample) / static_cast<double> (DawState::kBeatsPerBar);
}

double MasterGridService::GridContext::samplesToBars (std::int64_t sample) const
{
    return barForSample (sample);
}

//==============================================================================
// MasterGridService
//==============================================================================

MasterGridService::MasterGridService (const MasterClockPublisher& clockPublisher,
                                      std::function<double()>      sampleRateProvider)
    : clockPublisher_     (clockPublisher),
      sampleRateProvider_ (std::move (sampleRateProvider))
{
}

double MasterGridService::currentSampleRate() const
{
    if (sampleRateProvider_)
    {
        const double rate = sampleRateProvider_();
        if (rate > 0.0 && std::isfinite (rate))
            return rate;
    }
    // Invalid / zero device rate during a transition (PRD-0064 §1.5.6).
    return DawState::kProjectSampleRate;
}

MasterGridService::GridContext MasterGridService::snapshotGrid() const
{
    const MasterClockSnapshot snap = clockPublisher_.read();

    GridContext ctx;
    ctx.sampleRate = currentSampleRate();

    if (snap.masterBPM > 0.0 && std::isfinite (snap.masterBPM))
    {
        // Normal / paused-master path: use the (possibly held) master tempo.
        ctx.bpm               = snap.masterBPM;
        ctx.phaseOriginSample = snap.masterPhaseOriginSample;
        ctx.isPlaying         = snap.masterIsPlaying;
        ctx.isFallback        = false;
    }
    else
    {
        // Dormant clock / never had a master (PRD-0064 §1.5.2).
        ctx.bpm               = kFallbackBpm;
        ctx.phaseOriginSample = 0;
        ctx.isPlaying         = false;
        ctx.isFallback        = true;
    }

    ctx.samplesPerBeat = ctx.sampleRate * 60.0 / ctx.bpm;
    return ctx;
}

std::vector<MasterGridService::GridLine>
MasterGridService::sampleGrid (std::int64_t firstSample,
                               std::int64_t lastSample,
                               int          subBeatDivision) const
{
    std::vector<GridLine> lines;

    if (lastSample < firstSample)
        return lines;

    const int division = subBeatDivision > 0 ? subBeatDivision : 1;

    // One coherent read for the whole call.
    const GridContext ctx = snapshotGrid();

    // Step between emitted lines, in beats (e.g. division 4 => 0.25-beat step).
    const double beatStep = 1.0 / static_cast<double> (division);

    // First sub-division index at or after firstSample. Beat index of a line is
    // (subIndex * beatStep). Solve subIndex >= samplesToBeats(firstSample)/beatStep.
    const double firstBeat   = ctx.samplesToBeats (firstSample);
    const double firstSubF   = firstBeat / beatStep;
    std::int64_t subIndex    = static_cast<std::int64_t> (std::ceil (firstSubF - 1e-9));

    // Guard against an unbounded loop on pathological inputs.
    constexpr std::size_t kMaxLines = 1'000'000;

    for (; lines.size() < kMaxLines; ++subIndex)
    {
        const double       beat   = static_cast<double> (subIndex) * beatStep;
        const std::int64_t sample = ctx.beatsToSamples (beat);

        if (sample > lastSample)
            break;
        if (sample < firstSample)
            continue;

        GridLine line;
        line.sample = sample;
        line.beat   = beat;

        // Classify: whole beats are Bar (multiple of kBeatsPerBar) or Beat;
        // anything else is a SubBeat. Use floor-based modulo so negative beat
        // indices classify correctly on both sides of the origin.
        const double  beatRounded = std::round (beat);
        const bool    isWholeBeat = std::abs (beat - beatRounded) < 1e-6;

        if (isWholeBeat)
        {
            const std::int64_t beatIndex = static_cast<std::int64_t> (beatRounded);
            const std::int64_t bpb       = DawState::kBeatsPerBar;
            const std::int64_t mod       = ((beatIndex % bpb) + bpb) % bpb;
            line.kind = (mod == 0) ? GridLineKind::Bar : GridLineKind::Beat;
        }
        else
        {
            line.kind = GridLineKind::SubBeat;
        }

        lines.push_back (line);
    }

    return lines;
}

std::int64_t MasterGridService::beatsToSamples (double beats) const
{
    return snapshotGrid().beatsToSamples (beats);
}

double MasterGridService::samplesToBeats (std::int64_t sample) const
{
    return snapshotGrid().samplesToBeats (sample);
}

double MasterGridService::barForSample (std::int64_t sample) const
{
    return snapshotGrid().barForSample (sample);
}

double MasterGridService::samplesToBars (std::int64_t sample) const
{
    return snapshotGrid().samplesToBars (sample);
}

bool MasterGridService::isFallbackGrid() const
{
    return snapshotGrid().isFallback;
}

} // namespace Daw
