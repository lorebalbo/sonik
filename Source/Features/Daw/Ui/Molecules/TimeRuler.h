#pragma once
//==============================================================================
// PRD-0066: TimeRuler molecule.
//
// Renders the DAW time ruler: a dark bar-number header band on top and a light
// tick band below, exactly as the Figma DAW ruler (file 3bmQVcRbY9JSaJqTCPH9AQ,
// frame 86). Bar/beat tick positions are computed from the MasterGridService
// grid lines (PRD-0064) mapped through the TimelineTransform (PRD-0065); the
// ruler never computes tempo or grid positions itself (single source of truth).
//
// Message/UI thread only. No audio-thread code; the master grid is read through
// the MasterGridService snapshot accessor (bounded SeqLock retry, no locks held).
//==============================================================================

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Atoms/RulerTick.h"
#include "../../Model/MasterGridService.h"
#include "../../Transform/TimelineTransform.h"

namespace Daw
{

class TimeRuler final : public juce::Component
{
public:
    //--------------------------------------------------------------------------
    // Visual metrics, derived from Figma frame 86 (33-px total ruler).
    //--------------------------------------------------------------------------
    static constexpr int kHeaderBandHeight = 19;  // dark bar-number band
    static constexpr int kTickBandHeight   = 14;  // light beat-tick band
    static constexpr int kRulerHeight      = kHeaderBandHeight + kTickBandHeight;

    // Beat ticks are dropped (bars only) below this zoom, keeping the ruler
    // legible at coarse zooms (PRD-0066 §1.5.4 minimum-spacing guard).
    static constexpr double kMinBeatSpacingPx = 6.0;

    static int getRulerHeight() noexcept { return kRulerHeight; }

    //--------------------------------------------------------------------------
    // One resolved tick for layout/painting and for tests.
    //--------------------------------------------------------------------------
    struct TickInfo
    {
        double          x         = 0.0;   // pixel position within the ruler
        RulerTick::Kind kind      = RulerTick::Kind::Beat;
        bool            hasLabel  = false; // bar lines carry a number label
        int             barNumber = 0;     // 1-based bar number (bar 1 at origin)
    };

    // grid and transform are injected (no singletons). The transform is owned by
    // the DawPanel; this molecule keeps a reference and reads it each refresh.
    TimeRuler (MasterGridService& grid, const TimelineTransform& transform);

    //--------------------------------------------------------------------------
    // Pure tick resolution (no graphics): the visible bars/beats mapped to x.
    // Testable without a live graphics context.
    //--------------------------------------------------------------------------
    std::vector<TickInfo> computeTicks() const;

    // Recompute ticks and re-lay-out the child RulerTick atoms, then repaint.
    void refresh();

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    MasterGridService&       grid_;
    const TimelineTransform& transform_;

    juce::OwnedArray<RulerTick> tickComponents_;
    std::vector<TickInfo>       ticks_;

    static inline const juce::Colour kInk         { 0xFF2D2D2D }; // primary
    static inline const juce::Colour kSurface     { 0xFFFDFDFD }; // surface
    static inline const juce::Colour kTickBandBg  { 0xFFE2E2E2 }; // container-highest
    static inline const juce::Colour kHeaderBandBg{ 0xFFE2E2E2 }; // container-highest

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeRuler)
};

} // namespace Daw
