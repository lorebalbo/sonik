#pragma once
//==============================================================================
// PRD-0093: AutomationLaneViewBase — shared chrome for a single automation lane.
//
// Both the continuous and boolean lane organisms share the same lane frame: a
// left value-axis gutter (Space Mono min/max + all-caps parameter label), a
// per-lane DESIGN.md bypass (enable) button with active/inactive fill inversion,
// a 2-px ink lane frame, the dithered "inactive wash" applied when bypassed, and
// the read-only playhead vertical line. Subclasses only paint the lane BODY
// (curve / steps / fill / markers) inside paintLaneBody().
//
// This PRD performs NO editing. The only model write is the bypass flag. Mouse
// handling on the body is left virtual (onBodyMouseDown / onBodyMouseDoubleClick)
// so PRD-0094 can add breakpoint editing without rewriting this class.
//
// Message/UI thread only; observes the lane node via juce::ValueTree::Listener;
// no polling; no audio-thread code.
//==============================================================================

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AutomationLaneMetrics.h"
#include "../AutomationModel.h"
#include "../AutomationIds.h"
#include "../../Ui/DawLayoutMetrics.h"
#include "../../Transform/TimelineTransform.h"
#include "../../Editing/EditCommands.h"

namespace Daw
{

class AutomationLaneViewBase : public juce::Component,
                               protected juce::ValueTree::Listener
{
public:
    using PlayheadProvider = std::function<std::int64_t()>;

    AutomationLaneViewBase (juce::ValueTree laneNode,
                            const TimelineTransform& transform,
                            AutomationModel* model)
        : laneNode_ (std::move (laneNode)),
          transform_ (transform),
          model_ (model)
    {
        // The lane itself is non-interactive except the bypass button hit-test we
        // do directly in mouseUp; body clicks are inert in this PRD.
        if (laneNode_.isValid())
            laneNode_.addListener (this);
    }

    ~AutomationLaneViewBase() override
    {
        if (laneNode_.isValid())
            laneNode_.removeListener (this);
    }

    juce::ValueTree getLaneNode() const noexcept { return laneNode_; }

    bool isLaneEnabled() const
    {
        return laneNode_.isValid() ? AutomationModel::isLaneEnabled (laneNode_) : true;
    }

    // Inject the read-only playhead-sample provider (default returns -1 = hidden).
    void setPlayheadProvider (PlayheadProvider provider)
    {
        playheadProvider_ = std::move (provider);
        repaint();
    }

    //--------------------------------------------------------------------------
    // PRD-0094 editing wiring. The view dispatches every edit through this
    // shared EPIC-0010 dispatcher (one interleaved undo history with clip edits).
    // The (owner, parameterId) pair identifies this lane's model key.
    //--------------------------------------------------------------------------
    using SnapEnabledProvider = std::function<bool()>;

    void setEditDispatcher (EditCommandDispatcher* dispatcher) { dispatcher_ = dispatcher; }

    void setLaneKey (const juce::String& owner, const juce::String& parameterId)
    {
        owner_       = owner;
        parameterId_ = parameterId;
    }

    // The global snap toggle (PRD-0065). Default: snap ON (matches the timeline).
    void setSnapEnabledProvider (SnapEnabledProvider provider)
    {
        snapEnabledProvider_ = std::move (provider);
    }

    // Reposition after a transform (zoom/scroll) change — the lane bounds don't
    // move on zoom so resized() won't fire.
    void refreshTransform() { repaint(); }

    // Test/inspection hook: number of times the lane body has been (re)painted.
    int getPaintCount() const noexcept { return paintCount_; }

    void paint (juce::Graphics& g) final
    {
        ++paintCount_;

        const auto bounds = getLocalBounds();
        const int  gutter = DawLayout::kTrackHeaderWidth;

        auto gutterCell = bounds.withWidth (gutter);
        auto bodyCell   = bounds.withTrimmedLeft (gutter);

        const bool enabled = isLaneEnabled();

        // ---- Lane body background (canvas) ------------------------------
        g.setColour (kSurface);
        g.fillRect (bodyCell);

        // Bypassed: a denser dither wash over the body (one tonal layer down) so
        // it reads inactive while the curve/steps stay visible (§1.5.7).
        if (! enabled)
            paintInactiveWash (g, bodyCell);

        // ---- Subclass paints the lane body content ----------------------
        const auto bodyInner = bodyCell.reduced (0, AutomationLaneMetrics::kBodyInsetY)
                                         .withTrimmedLeft (0);
        paintLaneBody (g, bodyInner, enabled);

        // ---- Read-only playhead indicator (suppressed when bypassed) -----
        if (enabled)
            paintPlayhead (g, bodyCell);

        // ---- 2-px lane frame around the body ----------------------------
        g.setColour (kInk);
        g.drawRect (bodyCell, 2);

        // ---- Left value-axis gutter -------------------------------------
        paintGutter (g, gutterCell, enabled);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (bypassButtonBounds_.contains (event.getPosition()))
        {
            toggleBypass();
            return;
        }
        onBodyMouseUp (event);
    }

    void mouseDown (const juce::MouseEvent& event) override { onBodyMouseDown (event); }
    void mouseDrag (const juce::MouseEvent& event) override { onBodyMouseDrag (event); }
    void mouseDoubleClick (const juce::MouseEvent& event) override { onBodyMouseDoubleClick (event); }

    void resized() override
    {
        // The bypass button lives in the rightmost slice of the gutter.
        const int w   = AutomationLaneMetrics::kBypassButtonWidth;
        const int ins = AutomationLaneMetrics::kBypassButtonInset;
        bypassButtonBounds_ = juce::Rectangle<int> (
            DawLayout::kTrackHeaderWidth - w - ins,
            ins,
            w,
            getHeight() - 2 * ins);
    }

protected:
    //--------------------------------------------------------------------------
    // Subclass contract.
    //--------------------------------------------------------------------------
    // Paint the lane body (curve / steps / fill / markers) inside `body` (the
    // content area, already inset vertically, with x measured from the gutter).
    virtual void paintLaneBody (juce::Graphics& g,
                                juce::Rectangle<int> body,
                                bool enabled) = 0;

    // Maps a timeline sample to an x in THIS component's coordinate space (the
    // shared transform's content axis already starts at kTrackHeaderWidth).
    double sampleToBodyX (std::int64_t sample) const
    {
        return TimelineTransform::alignToPixelGrid (transform_.sampleToX (sample));
    }

    // PRD-0094 seams — overridable, no-ops here (no editing in PRD-0093).
    virtual void onBodyMouseDown (const juce::MouseEvent&) {}
    virtual void onBodyMouseUp (const juce::MouseEvent&) {}
    virtual void onBodyMouseDrag (const juce::MouseEvent&) {}
    virtual void onBodyMouseDoubleClick (const juce::MouseEvent&) {}

    //--------------------------------------------------------------------------
    // PRD-0094 editing helpers (subclasses use these).
    //--------------------------------------------------------------------------
    EditCommandDispatcher* editDispatcher() const noexcept { return dispatcher_; }
    const juce::String&    owner()          const noexcept { return owner_; }
    const juce::String&    parameterId()    const noexcept { return parameterId_; }

    // The active snap state, with the platform snap-override modifier (the same
    // Cmd/Ctrl EPIC-0010 clip edits use) DEFEATING snap for the current gesture
    // (§1.5.2). When no provider is injected, snap defaults ON.
    bool snapActive (const juce::MouseEvent& event) const
    {
        const bool globalSnap = snapEnabledProvider_ ? snapEnabledProvider_() : true;
        const bool override   = event.mods.isCommandDown() || event.mods.isCtrlDown();
        return globalSnap && ! override;
    }

    // Map a body x to a timeline sample, snapping to the master grid when snap is
    // active for this gesture (§1.5.2).
    std::int64_t bodyXToSample (double bodyX, const juce::MouseEvent& event) const
    {
        const std::int64_t raw = transform_.xToSample (bodyX);
        return snapActive (event) ? transform_.snapSampleToGrid (raw) : raw;
    }

    // The current playhead sample (-1 when hidden / no provider).
    std::int64_t currentPlayheadSample() const
    {
        return playheadProvider_ ? playheadProvider_() : (std::int64_t) -1;
    }

    // The all-caps parameter label + min/max labels printed in the gutter.
    juce::String paramLabel_, minLabel_ { "0" }, maxLabel_ { "1" };

    // ValueTree::Listener — repaint reactively on any lane change.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { repaint(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { repaint(); }
    void valueTreeParentChanged (juce::ValueTree&) override { repaint(); }

    const TimelineTransform& transform_;

    static inline const juce::Colour kInk     { 0xFF2D2D2D };
    static inline const juce::Colour kSurface { 0xFFFDFDFD };
    static inline const juce::Colour kGutterFill { 0xFFE5E5E5 };

private:
    void toggleBypass()
    {
        if (model_ == nullptr || ! laneNode_.isValid())
            return;
        model_->setLaneEnabled (laneNode_, ! AutomationModel::isLaneEnabled (laneNode_));
        repaint();
    }

    void paintGutter (juce::Graphics& g, juce::Rectangle<int> cell, bool enabled)
    {
        g.setColour (kGutterFill);
        g.fillRect (cell);
        g.setColour (kInk);
        g.drawRect (cell, 2);

        // Parameter label (all-caps Space Mono), top-left.
        g.setColour (kInk);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::bold));
        g.drawText (paramLabel_,
                    cell.withTrimmedLeft (5).withTrimmedTop (2)
                        .withWidth (DawLayout::kTrackHeaderWidth
                                    - AutomationLaneMetrics::kBypassButtonWidth - 10)
                        .withHeight (12),
                    juce::Justification::topLeft, false);

        // Min (bottom) / max (top) value labels, small Space Mono.
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 8.0f, juce::Font::plain));
        auto labelCol = cell.withTrimmedLeft (5)
                            .withWidth (DawLayout::kTrackHeaderWidth
                                        - AutomationLaneMetrics::kBypassButtonWidth - 10);
        g.drawText (maxLabel_, labelCol.removeFromTop (cell.getHeight() / 2)
                                       .removeFromBottom (10),
                    juce::Justification::bottomLeft, false);
        g.drawText (minLabel_, labelCol.removeFromBottom (10),
                    juce::Justification::bottomLeft, false);

        // Bypass (enable) button — DESIGN.md fill inversion: enabled = filled ink.
        g.setColour (enabled ? kInk : kSurface);
        g.fillRect (bypassButtonBounds_);
        g.setColour (kInk);
        g.drawRect (bypassButtonBounds_, 2);
        g.setColour (enabled ? kSurface : kInk);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 8.0f, juce::Font::bold));
        // "ON" when enabled (driving the mix), "BYP" when bypassed.
        g.drawText (enabled ? juce::String ("ON") : juce::String ("BYP"),
                    bypassButtonBounds_, juce::Justification::centred, false);
    }

    void paintPlayhead (juce::Graphics& g, juce::Rectangle<int> bodyCell)
    {
        const std::int64_t sample = currentPlayheadSample();
        if (sample < 0)
            return;

        const double x = sampleToBodyX (sample);
        if (x < (double) bodyCell.getX() || x > (double) bodyCell.getRight())
            return;

        g.setColour (kInk);
        g.fillRect ((float) x, (float) bodyCell.getY(), 1.0f, (float) bodyCell.getHeight());
    }

    void paintInactiveWash (juce::Graphics& g, juce::Rectangle<int> area)
    {
        // A denser checkerboard wash than the source-lane "inactive" dither: a
        // 1-px dot on a 3-px grid reads clearly as bypassed without colour.
        g.setColour (kInk.withAlpha (0.16f));
        for (int y = area.getY(); y < area.getBottom(); y += 3)
            for (int x = area.getX() + ((y / 3) % 2) * 1; x < area.getRight(); x += 3)
                g.fillRect (x, y, 1, 1);
    }

    juce::ValueTree   laneNode_;
    AutomationModel*  model_ { nullptr };
    PlayheadProvider  playheadProvider_;
    juce::Rectangle<int> bypassButtonBounds_;
    int               paintCount_ { 0 };

    // PRD-0094 editing state.
    EditCommandDispatcher* dispatcher_ { nullptr };
    juce::String           owner_, parameterId_;
    SnapEnabledProvider    snapEnabledProvider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneViewBase)
};

} // namespace Daw
