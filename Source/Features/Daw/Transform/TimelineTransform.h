#pragma once
//==============================================================================
// PRD-0065: Timeline Coordinate Transform
//
// A pure, MESSAGE/UI-THREAD value type that composes PRD-0064's sample<->beat
// musical conversion with a horizontal view transform (zoom + scroll) to map
// between three coordinate spaces:
//   * samples       — timeline sample position (int64)
//   * musical time   — beats / bars (double)
//   * pixels         — x within the timeline viewport (double)
//
// It owns NO JUCE Component, renders nothing, handles no input, mutates no
// ValueTree. It allocates nothing on any mapping call and takes no locks. All
// rounding is confined to the continuous<->discrete boundary (PRD-0065 §1.5.6).
//
// The forward sample->pixel path is a pure affine function of the sample
// (§1.5.7): x = (sample - leftEdgeSample) * pixelsPerBeat / samplesPerBeat.
// It never routes through a quantised beat index, so off-beat / non-matching-
// tempo clips map correctly.
//==============================================================================

#include <cstdint>

#include <juce_core/juce_core.h>

#include "../Model/MasterGridService.h"

namespace Daw
{

class TimelineTransform
{
public:
    //--------------------------------------------------------------------------
    // Minimal immutable musical snapshot the transform needs from PRD-0064.
    // Built from MasterGridService::GridContext (or directly in tests).
    //--------------------------------------------------------------------------
    struct GridSnapshot
    {
        // Default: 120 BPM at the project sample rate (44100*60/120 = 22050).
        double       samplesPerBeat    = DawState::kProjectSampleRate * 60.0 / 120.0;
        std::int64_t phaseOriginSample = 0;
        // Master tempo as a rate-independent BPM number. Carried alongside
        // samplesPerBeat so clip drawing can apply the elastic time-stretch
        // (sourceBpm/masterBpm) without re-deriving tempo from the sample rate.
        double       bpm               = 120.0;

        static GridSnapshot fromContext (const MasterGridService::GridContext& ctx)
        {
            GridSnapshot s;
            s.samplesPerBeat    = ctx.samplesPerBeat > 0.0 ? ctx.samplesPerBeat : 1.0;
            s.phaseOriginSample = ctx.phaseOriginSample;
            s.bpm               = ctx.bpm > 0.0 ? ctx.bpm : 120.0;
            return s;
        }

        // Musical conversions (mirror PRD-0064 anchoring: beat 0 at origin).
        std::int64_t beatToSample (double beat) const
        {
            return phaseOriginSample
                 + static_cast<std::int64_t> (std::llround (beat * samplesPerBeat));
        }

        double sampleToBeat (std::int64_t sample) const
        {
            return static_cast<double> (sample - phaseOriginSample) / samplesPerBeat;
        }
    };

    //--------------------------------------------------------------------------
    // Zoom limits (PRD-0065 §1.5.2). Named constants asserted by the tests.
    //   * min: a few pixels per beat — whole-arrangement overview.
    //   * max: one beat fills a large fraction of a viewport, but never finer
    //          than ~1 sample/pixel at high sample rates (keeps inverse stable).
    //--------------------------------------------------------------------------
    static constexpr double kMinPixelsPerBeat = 2.0;
    static constexpr double kMaxPixelsPerBeat = 4000.0;

    // Default zoom: comfortable mid-range (about one bar per ~400 px at 4/4).
    static constexpr double kDefaultPixelsPerBeat = 100.0;

    //--------------------------------------------------------------------------
    // Scroll margin defaults (PRD-0065 §1.5.4), expressed in beats.
    //--------------------------------------------------------------------------
    static constexpr double kLeftMarginBeats = 1.0;   // negative margin before bar 0

    //--------------------------------------------------------------------------
    // Construction: a grid snapshot, the current view state, the viewport
    // width (px), and the arrangement content-end sample (for scroll clamping).
    //--------------------------------------------------------------------------
    TimelineTransform (GridSnapshot  grid,
                       double        pixelsPerBeat,
                       std::int64_t  leftEdgeSample,
                       double        viewportWidthPx,
                       std::int64_t  contentEndSample = 0);

    //--------------------------------------------------------------------------
    // View-state accessors / setters (all clamped).
    //--------------------------------------------------------------------------
    double       getPixelsPerBeat()  const { return pixelsPerBeat_; }
    std::int64_t getLeftEdgeSample() const { return leftEdgeSample_; }
    double       getViewportWidth()  const { return viewportWidthPx_; }

    void setPixelsPerBeat   (double newPixelsPerBeat);
    void setLeftEdgeSample  (std::int64_t newLeftEdgeSample);
    void setViewportWidth   (double newWidthPx);
    void setContentEndSample (std::int64_t newContentEndSample);

    // Convert a presentation "bars on screen" value to pixelsPerBeat (§1.5.1).
    double barsPerScreenToPixelsPerBeat (double barsPerScreen) const;
    double pixelsPerBeatToBarsPerScreen() const;

    //--------------------------------------------------------------------------
    // Forward mappings (sub-pixel double; never rounded internally).
    //--------------------------------------------------------------------------
    double sampleToX (std::int64_t sample) const;
    double beatToX   (double beat) const;
    double barToX    (double bar)  const;

    //--------------------------------------------------------------------------
    // Inverse mappings (rounded once to int64; clamped to scroll/content bounds).
    //--------------------------------------------------------------------------
    std::int64_t xToSample (double px) const;
    double       xToBeat   (double px) const;

    //--------------------------------------------------------------------------
    // Focal-point zoom (§1.5.5): scales pixelsPerBeat by zoomFactor while
    // keeping the sample under focusPx fixed at focusPx.
    //--------------------------------------------------------------------------
    void zoomAroundX (double focusPx, double zoomFactor);

    //--------------------------------------------------------------------------
    // Scroll by a pixel delta (clamped).
    //--------------------------------------------------------------------------
    void scrollByX (double deltaPx);

    //--------------------------------------------------------------------------
    // Nearest beat-boundary sample (round-half-up). No UI work.
    //--------------------------------------------------------------------------
    std::int64_t snapSampleToGrid (std::int64_t sample) const;

    //--------------------------------------------------------------------------
    // Device-pixel alignment helper (§1.5.3). Renderers call this at draw time
    // for crisp 1-/2-px lines; the forward mappings never round internally.
    //--------------------------------------------------------------------------
    static double alignToPixelGrid (double x, double displayScale = 1.0);

    //--------------------------------------------------------------------------
    // Scroll bounds (§1.5.4), derived from content end + viewport + margins.
    //--------------------------------------------------------------------------
    std::int64_t minLeftEdgeSample() const;
    std::int64_t maxLeftEdgeSample() const;

    const GridSnapshot& grid() const { return grid_; }

private:
    std::int64_t clampLeftEdge (std::int64_t candidate) const;
    static double clampZoom (double pixelsPerBeat);
    double samplesPerPixel() const;

    GridSnapshot grid_;
    double       pixelsPerBeat_   = kDefaultPixelsPerBeat;
    std::int64_t leftEdgeSample_  = 0;
    double       viewportWidthPx_ = 0.0;
    std::int64_t contentEndSample_ = 0;
};

} // namespace Daw
