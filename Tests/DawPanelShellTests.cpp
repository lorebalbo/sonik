//==============================================================================
// PRD-0066: DAW Panel Shell & Time Ruler tests.
//
// Covers the non-rendering contract of the shell:
//   * TimeRuler::computeTicks resolves the right bars/beats for a known grid and
//     maps them through the TimelineTransform (positions match the transform).
//   * Distinct Bar vs Beat tick kinds, and the coarse-zoom beat-drop guard.
//   * DawPanel collapse/expand toggles between the two fixed heights and fires
//     onPreferredHeightChanged.
//
// No paint() is invoked, so these tests do not depend on a graphics context.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

#include "Features/Daw/Ui/Molecules/TimeRuler.h"
#include "Features/Daw/Ui/Organisms/DawPanel.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockSnapshot.h"

class DawPanelShellTests : public juce::UnitTest
{
public:
    DawPanelShellTests() : juce::UnitTest ("DAW Panel Shell (PRD-0066)", "Sonik") {}

    static int countKind (const std::vector<Daw::TimeRuler::TickInfo>& ticks,
                          Daw::RulerTick::Kind kind)
    {
        int n = 0;
        for (const auto& t : ticks)
            if (t.kind == kind)
                ++n;
        return n;
    }

    void runTest() override
    {
        using TT = Daw::TimelineTransform;

        //----------------------------------------------------------------------
        beginTest ("TimeRuler: resolves bars/beats and matches the transform");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ /*BPM*/ 120.0, /*native*/ 120.0, /*phase*/ 0, /*playing*/ true });

            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            // 120 BPM @ 44100 -> 22050 samples/beat. Default DAW zoom: 12.5 px/beat.
            const double ppb = Daw::DawPanel::kDefaultPixelsPerBeat;
            const auto   ctx = grid.snapshotGrid();
            auto         snap = TT::GridSnapshot::fromContext (ctx);

            TT transform (snap, ppb, snap.phaseOriginSample, /*width*/ 600.0,
                          /*contentEnd*/ 0);

            Daw::TimeRuler ruler (grid, transform);
            const auto ticks = ruler.computeTicks();

            expect (! ticks.empty(), "ruler should resolve visible ticks");

            const int bars  = countKind (ticks, Daw::RulerTick::Kind::Bar);
            const int beats = countKind (ticks, Daw::RulerTick::Kind::Beat);
            expect (bars  > 0, "at least one bar tick visible");
            expect (beats > 0, "at least one beat tick visible (beats kept at 12.5 px/beat)");

            // ~50 px per bar over a 600 px viewport -> on the order of a dozen bars.
            expect (bars >= 8 && bars <= 16,
                    "bar count within the expected range for the default zoom");

            // Each bar tick's x must equal the transform's mapping for that bar.
            const double samplesPerBeat = ctx.samplesPerBeat;
            for (const auto& t : ticks)
            {
                if (t.kind != Daw::RulerTick::Kind::Bar)
                    continue;
                const std::int64_t barSample = static_cast<std::int64_t> (
                    std::llround ((t.barNumber - 1) * DawState::kBeatsPerBar * samplesPerBeat));
                expectWithinAbsoluteError (t.x, transform.sampleToX (barSample), 1.0e-6);
            }

            // Adjacent bars are one bar (4 beats) apart in pixels.
            double previousBarX = 0.0;
            bool   havePrevious = false;
            for (const auto& t : ticks)
            {
                if (t.kind != Daw::RulerTick::Kind::Bar)
                    continue;
                if (havePrevious)
                    expectWithinAbsoluteError (t.x - previousBarX,
                                               DawState::kBeatsPerBar * ppb, 1.0e-6);
                previousBarX = t.x;
                havePrevious = true;
            }
        }

        //----------------------------------------------------------------------
        beginTest ("TimeRuler: beat ticks dropped below the minimum-spacing guard");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            const auto ctx  = grid.snapshotGrid();
            auto       snap = TT::GridSnapshot::fromContext (ctx);

            // 4 px/beat is below TimeRuler::kMinBeatSpacingPx (6) -> bars only.
            TT transform (snap, 4.0, snap.phaseOriginSample, 600.0, 0);
            Daw::TimeRuler ruler (grid, transform);
            const auto ticks = ruler.computeTicks();

            expect (countKind (ticks, Daw::RulerTick::Kind::Beat) == 0,
                    "no beat ticks below the spacing guard");
            expect (countKind (ticks, Daw::RulerTick::Kind::Bar) > 0,
                    "bar ticks still shown");
        }

        //----------------------------------------------------------------------
        beginTest ("RulerTick: bar ticks are taller and heavier than beat ticks");
        {
            using RT = Daw::RulerTick;
            const int band = Daw::TimeRuler::kTickBandHeight;
            expect (RT::tickHeightForKind (RT::Kind::Bar, band)
                    > RT::tickHeightForKind (RT::Kind::Beat, band),
                    "bar tick is taller");
            expect (RT::lineWidthForKind (RT::Kind::Bar)
                    > RT::lineWidthForKind (RT::Kind::Beat),
                    "bar tick is heavier");
        }

        //----------------------------------------------------------------------
        beginTest ("DawPanel: collapse/expand toggles between two fixed heights");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (juce::Identifier ("SonikState"));
            auto dawBranch = DawState::getOrCreateDawBranch (root);
            Daw::DawPanel panel (grid, dawBranch,
                                 [] (int) { return juce::ValueTree(); });
            panel.setSize (1000, panel.getPreferredHeight());

            expect (Daw::DawPanel::kExpandedHeight != Daw::DawPanel::kCollapsedHeight,
                    "the two states must have distinct heights");

            // The panel now starts collapsed (deck-forward default matching the
            // Figma layout); expand/collapse is still driven by setExpanded.
            expect (! panel.isExpanded(), "panel starts collapsed");
            expectEquals (panel.getPreferredHeight(), Daw::DawPanel::kCollapsedHeight);

            int callbacks = 0;
            panel.onPreferredHeightChanged = [&callbacks] { ++callbacks; };

            panel.setExpanded (true);
            expect (panel.isExpanded());
            expectEquals (panel.getPreferredHeight(), Daw::DawPanel::kExpandedHeight);
            expectEquals (callbacks, 1, "reflow callback fired on expand");

            panel.setExpanded (false);
            expect (! panel.isExpanded());
            expectEquals (panel.getPreferredHeight(), Daw::DawPanel::kCollapsedHeight);
            expectEquals (callbacks, 2, "reflow callback fired on collapse");

            // Idempotent set does not fire the callback again.
            panel.setExpanded (false);
            expectEquals (callbacks, 2, "no callback when state is unchanged");
        }
    }
};

static DawPanelShellTests dawPanelShellTests;
