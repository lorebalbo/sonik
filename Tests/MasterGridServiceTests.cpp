//==============================================================================
// PRD-0064: MasterGridService tests.
//
// Verifies grid-line sample positions for a known BPM / sample-rate / phase
// origin, beat-0 alignment, beatsToSamples/samplesToBeats round-trip, bar
// boundaries every kBeatsPerBar beats, the masterBPM == 0 fallback, and the
// absence of NaN/infinity across a sweep of snapshots.
//==============================================================================

#include <juce_core/juce_core.h>
#include <cmath>

#include "Features/Sync/MasterClockSnapshot.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/State/DawState.h"

class MasterGridServiceTests : public juce::UnitTest
{
public:
    MasterGridServiceTests() : juce::UnitTest ("Master Grid Service", "Sonik") {}

    void runTest() override
    {
        using namespace Daw;

        beginTest ("samplesPerBeat = sampleRate * 60 / BPM; beat 0 at phase origin");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;
            s.masterPhaseOriginSample = 10'000;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 48000.0; });
            const auto ctx = svc.snapshotGrid();

            // 48000 * 60 / 120 = 24000 samples per beat.
            expectWithinAbsoluteError (ctx.samplesPerBeat, 24000.0, 1e-9);
            expect (! ctx.isFallback);

            // Beat 0 lands exactly on the phase origin.
            expectEquals (ctx.beatsToSamples (0.0), (std::int64_t) 10'000);
            expectWithinAbsoluteError (ctx.samplesToBeats (10'000), 0.0, 1e-9);
        }

        beginTest ("grid-line positions for a known BPM/rate/origin triple");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;             // 22050 spb at 44100
            s.masterPhaseOriginSample = 0;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            // Four beats: 0, 22050, 44100, 66150.
            const auto lines = svc.sampleGrid (0, 66150, 1);
            expectEquals ((int) lines.size(), 4);
            expectEquals (lines[0].sample, (std::int64_t) 0);
            expectEquals (lines[1].sample, (std::int64_t) 22050);
            expectEquals (lines[2].sample, (std::int64_t) 44100);
            expectEquals (lines[3].sample, (std::int64_t) 66150);

            // Beat 0 -> Bar, beats 1..3 -> Beat (kBeatsPerBar == 4).
            expect (lines[0].kind == MasterGridService::GridLineKind::Bar);
            expect (lines[1].kind == MasterGridService::GridLineKind::Beat);
            expect (lines[2].kind == MasterGridService::GridLineKind::Beat);
            expect (lines[3].kind == MasterGridService::GridLineKind::Beat);
        }

        beginTest ("bar boundaries every kBeatsPerBar beats");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;             // 22050 spb at 44100
            s.masterPhaseOriginSample = 0;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            // Beats 0..8 -> bars at 0, 4, 8.
            const auto lines = svc.sampleGrid (0, 22050 * 8, 1);
            expectEquals ((int) lines.size(), 9);
            for (const auto& l : lines)
            {
                const int beatIdx = (int) std::llround (l.beat);
                const bool isBar = (beatIdx % DawState::kBeatsPerBar) == 0;
                expect ((l.kind == MasterGridService::GridLineKind::Bar) == isBar);
            }

            // barForSample at exactly kBeatsPerBar beats == bar 1.
            const std::int64_t oneBarSample = (std::int64_t) (22050 * DawState::kBeatsPerBar);
            expectWithinAbsoluteError (svc.barForSample (oneBarSample), 1.0, 1e-9);
        }

        beginTest ("sub-beat divisions emit finer lines tagged SubBeat");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;             // 22050 spb at 44100
            s.masterPhaseOriginSample = 0;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            // Division 2 across [0, 22050] -> beats 0, 0.5, 1.0 => 3 lines.
            const auto lines = svc.sampleGrid (0, 22050, 2);
            expectEquals ((int) lines.size(), 3);
            expect (lines[0].kind == MasterGridService::GridLineKind::Bar);     // beat 0
            expect (lines[1].kind == MasterGridService::GridLineKind::SubBeat); // beat 0.5
            expect (lines[2].kind == MasterGridService::GridLineKind::Beat);    // beat 1
            expectEquals (lines[1].sample, (std::int64_t) 11025);
        }

        beginTest ("beatsToSamples / samplesToBeats round-trip");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 128.3;
            s.masterPhaseOriginSample = 123'456;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });

            for (double beat : { -8.0, -1.5, 0.0, 1.0, 3.25, 17.0, 256.0 })
            {
                const std::int64_t sample = svc.beatsToSamples (beat);
                const double back = svc.samplesToBeats (sample);
                // Round-trip differs by less than one beat of rounding error.
                expect (std::abs (back - beat) < 1.0);
            }
        }

        beginTest ("negative beat indices before the phase origin");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;             // 22050 spb at 44100
            s.masterPhaseOriginSample = 44100; // beat 0 at sample 44100
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            // Range [0, 44100] -> beats -2, -1, 0 at samples 0, 22050, 44100.
            const auto lines = svc.sampleGrid (0, 44100, 1);
            expectEquals ((int) lines.size(), 3);
            expectEquals (lines[0].sample, (std::int64_t) 0);
            expectWithinAbsoluteError (lines[0].beat, -2.0, 1e-6);
            // beat -2 is a multiple of kBeatsPerBar? -2 % 4 != 0 -> Beat.
            expect (lines[0].kind == MasterGridService::GridLineKind::Beat);
            // beat 0 -> Bar.
            expect (lines[2].kind == MasterGridService::GridLineKind::Bar);
        }

        beginTest ("masterBPM == 0 uses 120 BPM fallback, origin 0, isFallback true");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 0.0;
            s.masterPhaseOriginSample = 99'999; // must be ignored in fallback
            s.masterIsPlaying = false;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            const auto ctx = svc.snapshotGrid();

            expect (ctx.isFallback);
            expect (svc.isFallbackGrid());
            expectWithinAbsoluteError (ctx.bpm, 120.0, 1e-9);
            expectEquals (ctx.phaseOriginSample, (std::int64_t) 0);
            // 44100 * 60 / 120 = 22050.
            expectWithinAbsoluteError (ctx.samplesPerBeat, 22050.0, 1e-9);
            // Fallback still produces a finite grid.
            const auto lines = svc.sampleGrid (0, 22050 * 4, 1);
            expectEquals ((int) lines.size(), 5);
        }

        beginTest ("paused master (isPlaying false, BPM > 0) is NOT fallback");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 124.0;
            s.masterPhaseOriginSample = 5000;
            s.masterIsPlaying = false;        // paused
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            const auto ctx = svc.snapshotGrid();
            expect (! ctx.isFallback);
            expectWithinAbsoluteError (ctx.bpm, 124.0, 1e-9);
            expectEquals (ctx.phaseOriginSample, (std::int64_t) 5000);
        }

        beginTest ("invalid/zero device rate falls back to project rate");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;
            s.masterPhaseOriginSample = 0;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 0.0; }); // bad device rate
            const auto ctx = svc.snapshotGrid();
            expectWithinAbsoluteError (ctx.sampleRate, DawState::kProjectSampleRate, 1e-9);
            // 44100 * 60 / 120 = 22050.
            expectWithinAbsoluteError (ctx.samplesPerBeat, 22050.0, 1e-9);
        }

        beginTest ("no NaN/infinity across a sweep of snapshots");
        {
            const double bpms[]   = { 0.0, -5.0, 60.0, 120.0, 128.0, 174.0, 300.0 };
            const double rates[]  = { 0.0, 44100.0, 48000.0, 96000.0 };
            const std::int64_t origins[] = { 0, -10'000, 1'000'000 };

            for (double bpm : bpms)
                for (double rate : rates)
                    for (std::int64_t origin : origins)
                    {
                        MasterClockPublisher pub;
                        MasterClockSnapshot s;
                        s.masterBPM = bpm;
                        s.masterPhaseOriginSample = origin;
                        s.masterIsPlaying = bpm > 0.0;
                        pub.publish (s);

                        MasterGridService svc (pub, [rate] { return rate; });
                        const auto ctx = svc.snapshotGrid();

                        expect (std::isfinite (ctx.samplesPerBeat));
                        expect (ctx.samplesPerBeat > 0.0);

                        for (std::int64_t sample : { (std::int64_t) -50'000, (std::int64_t) 0,
                                                     (std::int64_t) 500'000 })
                        {
                            expect (std::isfinite (ctx.samplesToBeats (sample)));
                            expect (std::isfinite (ctx.barForSample (sample)));
                        }
                        for (double beat : { -16.0, 0.0, 64.0 })
                            expect (ctx.beatsToSamples (beat) == ctx.beatsToSamples (beat)); // not NaN
                    }
        }

        beginTest ("empty / inverted range returns no lines");
        {
            MasterClockPublisher pub;
            MasterClockSnapshot s;
            s.masterBPM = 120.0;
            s.masterPhaseOriginSample = 0;
            s.masterIsPlaying = true;
            pub.publish (s);

            MasterGridService svc (pub, [] { return 44100.0; });
            expect (svc.sampleGrid (1000, 500, 1).empty());
        }
    }
};

static MasterGridServiceTests masterGridServiceTests;
