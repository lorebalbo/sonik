#pragma once
//==============================================================================
// PRD-0068: ClipBlock atom.
//
// Renders a single DawClip (PRD-0063) as a discrete bordered rectangle on its
// lane. Horizontal placement and width come exclusively from the PRD-0065
// TimelineTransform so the block tracks zoom/scroll identically to the grid and
// ruler; the on-timeline length is 1:1 with the crop length (no time-stretch).
//
// Inside the block the source waveform for the crop window
// [sourceStartSample, sourceEndSample] is drawn by REUSING the PRD-0006
// WaveformData mipmap cache — resolved by sourceFileId through an injected
// read-only accessor. No new analysis pass is triggered here; a cache miss
// degrades to a neutral dithered placeholder so the block never disappears.
//
// Visual contract (derived from DESIGN.md waveform + button rules, PRD-0068
// §1.5.7): strict monochrome (#2d2d2d ink on #fdfdfd), 2-px solid #2d2d2d
// border, zero radius, tonal layering above the lane, dithering not colour. The
// WaveformPoint energyLow/Mid/High frequency fields are deliberately ignored.
//
// Message/UI thread only. No DSP, no audio-thread contact: reads cached
// WaveformData and ValueTree properties only.
//==============================================================================

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../Model/DawClip.h"
#include "../../Transform/TimelineTransform.h"
#include "Features/Waveform/WaveformData.h"

namespace Daw
{

class ClipBlock final : public juce::Component,
                        private juce::ValueTree::Listener
{
public:
    // Resolves a clip's sourceFileId to a cached WaveformData (PRD-0006). A
    // cache miss returns nullptr; this accessor never triggers analysis.
    using WaveformSource = std::function<WaveformData::Ptr (const juce::String& sourceFileId)>;

    ClipBlock (juce::ValueTree clipNode,
               const TimelineTransform& transform,
               WaveformSource waveformSource);

    ~ClipBlock() override;

    //--------------------------------------------------------------------------
    // Horizontal placement (pure transform-derived). The owning LaneView sets
    // the block's vertical band; these give the timeline-correct x / width.
    //--------------------------------------------------------------------------
    int getTimelineX() const;
    int getTimelineWidth() const;

    // Convenience: set this block's bounds inside its parent for the given
    // vertical band, deriving x/width from the transform. xOffset shifts the
    // timeline origin to the right (e.g. past a lane header gutter).
    void applyTimelineBounds (int xOffset, int topY, int height);

    //--------------------------------------------------------------------------
    // Crop-window → mipmap mapping (PRD-0068 §1.5.2/§1.5.3). Exposed for tests.
    //--------------------------------------------------------------------------
    struct WaveformSlice
    {
        bool   valid      { false }; // false => draw placeholder
        int    level      { 0 };     // chosen mipmap tier
        int    firstPoint { 0 };     // inclusive point index in the tier
        int    lastPoint  { 0 };     // exclusive-clamped point index in the tier
        bool   truncated  { false }; // crop exceeds analysed length (prefix only)
    };

    // Computes the slice for the current transform zoom and a cached analysis.
    static WaveformSlice computeSlice (const WaveformData& data,
                                       std::int64_t sourceStartSample,
                                       std::int64_t sourceEndSample,
                                       double samplesPerPixel);

    // Samples-per-pixel implied by the transform (1:1 clip, no stretch).
    static double samplesPerPixelFor (const TimelineTransform& transform);

    const DawClip& getClip() const noexcept { return clip_; }

    //--------------------------------------------------------------------------
    // PRD-0084/0085/0086: Editing callbacks.
    //
    // Wire these from the owning LaneView/DawPanel to an EditCommandDispatcher.
    // Each callback receives the clip's clipId and the relevant new value.
    //--------------------------------------------------------------------------

    /// Body drag: called each mouse-move during a body drag with the new
    /// candidate timelineStartSample (already snapped if snap is enabled).
    std::function<void (const juce::String& clipId, int64_t newTimelineStart)> onMoveDrag;
    /// Body drag committed (mouse up).
    std::function<void (const juce::String& clipId, int64_t finalTimelineStart)> onMoveEnd;

    /// Left-edge drag: positive delta = trim inward; negative = uncrop outward.
    /// Receives the new candidate sourceStartSample.
    std::function<void (const juce::String& clipId, int64_t newSourceStart, bool uncrop)> onLeftEdgeDrag;
    std::function<void (const juce::String& clipId, int64_t finalSourceStart, bool uncrop)> onLeftEdgeEnd;

    /// Right-edge drag: positive delta = uncrop outward; negative = trim inward.
    /// Receives the new candidate sourceEndSample.
    std::function<void (const juce::String& clipId, int64_t newSourceEnd, bool uncrop)> onRightEdgeDrag;
    std::function<void (const juce::String& clipId, int64_t finalSourceEnd, bool uncrop)> onRightEdgeEnd;

    /// Split: called when the user double-clicks on the clip body (cuts at cursor).
    std::function<void (const juce::String& clipId, int64_t cutTimelineSample)> onSplit;

    /// Delete: called when the user presses Delete/Backspace while the clip has focus.
    std::function<void (const juce::String& clipId)> onDelete;

    /// Gain: called when the user right-click → gain changes (simplified: scroll wheel).
    std::function<void (const juce::String& clipId, float gainDbDelta)> onGainScroll;

    /// Whether grid-snap is enabled for drags.
    bool snapEnabled { false };

    void paint (juce::Graphics& g) override;

    void mouseEnter       (const juce::MouseEvent& e) override;
    void mouseExit        (const juce::MouseEvent& e) override;
    void mouseMove        (const juce::MouseEvent& e) override;
    void mouseDown        (const juce::MouseEvent& e) override;
    void mouseDrag        (const juce::MouseEvent& e) override;
    void mouseUp          (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove   (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed       (const juce::KeyPress& key) override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override;
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void reloadClip();
    void paintWaveform   (juce::Graphics& g, juce::Rectangle<int> inner);
    void paintPlaceholder (juce::Graphics& g, juce::Rectangle<int> inner);
    void paintGlitch      (juce::Graphics& g, juce::Rectangle<int> inner); // PRD-0097
    void paintEdgeHandles (juce::Graphics& g);

    // Hit-zone detection for edges.
    static constexpr int kEdgeHitWidth = 8; // px each side
    enum class DragZone { None, Body, LeftEdge, RightEdge };
    DragZone hitZoneAt (int localX) const;
    void updateCursorForZone (DragZone zone);

    // ---- Drag state -------------------------------------------------------
    DragZone dragZone_         { DragZone::None };
    int      dragStartX_       { 0 };
    int64_t  dragStartTimeline_ { 0 };
    int64_t  dragStartSrcStart_ { 0 };
    int64_t  dragStartSrcEnd_   { 0 };
    bool     dragActive_       { false };
    bool     hovered_          { false }; // true while mouse is over this clip
    bool     missingSource_    { false }; // PRD-0097: source unresolved -> Glitch

    juce::ValueTree          clipNode_;
    const TimelineTransform& transform_;
    WaveformSource           waveformSource_;
    DawClip                  clip_;

    // Last placement band applied by the owning lane, so the block can re-apply
    // its own bounds the instant its crop end grows (keeping width and waveform
    // in lockstep instead of shimmering between relayouts).
    int  bandXOffset_ { 0 };
    int  bandTopY_    { 0 };
    int  bandHeight_  { 0 };
    bool bandValid_   { false };

    static inline const juce::Colour kInk       { 0xFF2D2D2D }; // primary
    static inline const juce::Colour kSurface   { 0xFFFDFDFD }; // surface
    static inline const juce::Colour kClipFill  { 0xFFF7F7F8 }; // tonal layer over lane

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipBlock)
};

} // namespace Daw
