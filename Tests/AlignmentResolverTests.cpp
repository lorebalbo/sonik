#include <juce_core/juce_core.h>

#include "Features/Daw/Recording/AlignmentResolver.h"
#include "Features/Sync/PhaseLockEngine.h"

namespace
{

using namespace Daw;

// A musically sensible default: 120 BPM, 44100 Hz -> 22050 samples/beat.
AlignmentInputs baseInputs()
{
    AlignmentInputs in;
    in.playheadSample    = 23000;   // deliberately off-grid
    in.deckBPM           = 120.0;
    in.masterBPM         = 120.0;
    in.isSynced          = true;
    in.phaseOffsetBeats  = 0.0;
    in.beatgridAnchor    = 0;
    in.beatgridInterval  = 22050.0; // deck samples/beat
    in.sourceStartSample = 5000;
    in.gridOrigin        = 0;
    in.gridInterval      = 22050.0; // master samples/beat
    return in;
}

} // namespace

//==============================================================================
class AlignmentResolverTests : public juce::UnitTest
{
public:
    AlignmentResolverTests()
        : juce::UnitTest ("Grid Alignment Resolver", "Sonik") {}

    void runTest() override
    {
        const AlignmentResolver resolver;

        beginTest ("Tempo-matched + phase-aligned -> GridAligned snapped to grid line");
        {
            auto in = baseInputs();
            in.playheadSample = 23000; // nearest beat is 22050 (beat 1)
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::GridAligned);
            expectEquals (r.timelineStartSample, (std::int64_t) 22050);
            // Start lands exactly on a grid line.
            expectEquals (r.timelineStartSample % (std::int64_t) 22050, (std::int64_t) 0);
        }

        beginTest ("GridAligned: a playhead already on a grid line is unchanged");
        {
            auto in = baseInputs();
            in.playheadSample = 44100; // exactly beat 2
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::GridAligned);
            expectEquals (r.timelineStartSample, (std::int64_t) 44100);
        }

        beginTest ("Non-matching BPM (half/double) -> FirstBeatAnchored, downbeat on grid");
        {
            auto in = baseInputs();
            in.deckBPM          = 70.0;
            in.masterBPM        = 140.0;   // raw ratio 2.0 -> outside [0.667, 1.5]
            in.beatgridAnchor   = 0;
            in.beatgridInterval = 18900.0; // deck samples/beat at 140 BPM
            in.sourceStartSample = 5000;
            in.playheadSample    = 23000;

            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::FirstBeatAnchored);

            // firstDownbeatSource = ceil(5000/18900)*18900 = 18900; offset = 13900.
            // downbeatTimeline = 23000 + 13900 = 36900; nearest grid = 44100.
            // anchor = 44100 - 13900 = 30200.
            expectEquals (r.timelineStartSample, (std::int64_t) 30200);

            // The anchored downbeat itself sits exactly on a master grid line.
            const std::int64_t downbeatOffset = 13900;
            const std::int64_t downbeatTimeline = r.timelineStartSample + downbeatOffset;
            expectEquals (downbeatTimeline % (std::int64_t) 22050, (std::int64_t) 0);
        }

        beginTest ("Matched BPM but not phase-aligned (offset 0.1) -> FirstBeatAnchored");
        {
            auto in = baseInputs();
            in.phaseOffsetBeats = 0.1;
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::FirstBeatAnchored);
        }

        beginTest ("phaseOffsetBeats exactly at convergenceThreshold -> FirstBeatAnchored (strict)");
        {
            auto in = baseInputs();
            in.phaseOffsetBeats = PhaseLockEngine::convergenceThreshold; // 0.02, not < 0.02
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::FirstBeatAnchored);

            // Just inside the band IS grid-aligned.
            in.phaseOffsetBeats = PhaseLockEngine::convergenceThreshold - 1.0e-6;
            const auto r2 = resolver.resolve (in);
            expect (r2.mode == AlignmentMode::GridAligned);
        }

        beginTest ("No analysed beatgrid -> FirstBeatAnchored at raw playhead, no crash");
        {
            auto in = baseInputs();
            in.beatgridInterval = 0.0; // unanalysed
            in.playheadSample   = 12345;
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::FirstBeatAnchored);
            expectEquals (r.timelineStartSample, (std::int64_t) 12345);
        }

        beginTest ("isSynced == false at matching BPM -> FirstBeatAnchored");
        {
            auto in = baseInputs();
            in.isSynced = false;
            const auto r = resolver.resolve (in);
            expect (r.mode == AlignmentMode::FirstBeatAnchored);
        }

        beginTest ("Pure function: identical inputs yield identical outputs");
        {
            auto in = baseInputs();
            in.phaseOffsetBeats = 0.3;
            const auto a = resolver.resolve (in);
            const auto b = resolver.resolve (in);
            expect (a.mode == b.mode);
            expectEquals (a.timelineStartSample, b.timelineStartSample);
        }

        beginTest ("Decision is immutable basis: changing sync after open is the caller's concern");
        {
            // The resolver has no re-resolve API; calling again with new state
            // simply yields a fresh decision (the engine never calls it twice).
            auto in = baseInputs();
            const auto open = resolver.resolve (in);
            expect (open.mode == AlignmentMode::GridAligned);

            in.isSynced = false; // deck drifts out of sync later
            const auto later = resolver.resolve (in);
            expect (later.mode == AlignmentMode::FirstBeatAnchored);
            // Proves the two are independent computations, not a mutation.
        }
    }
};

static AlignmentResolverTests alignmentResolverTests;
