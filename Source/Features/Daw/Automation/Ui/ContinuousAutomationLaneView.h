#pragma once
//==============================================================================
// PRD-0093: ContinuousAutomationLaneView — renders ONE continuous lane
// (owner + parameterId) from its ContinuousLane ValueTree node.
//
// Rendering (§1.5.2):
//   - A #2d2d2d polyline (1-2px) through the ordered breakpoints.
//   - A small filled #2d2d2d SQUARE dot at each breakpoint.
//   - A DITHERED #2d2d2d fill beneath the curve of CONSTANT checkerboard density
//     (a single tonal layer — never value-proportional).
//   - Per-segment interpolation: linear -> diagonal; step/hold -> horizontal run
//     to the next x then vertical jump (read from the LEFT breakpoint).
//   - A read-only HOLLOW playhead marker riding the curve at the playhead x
//     (visually distinct from the FILLED editable breakpoint squares), suppressed
//     when bypassed.
//
// Editing is PRD-0094; this view is read-only apart from the inherited bypass.
//==============================================================================

#include <algorithm>
#include <vector>

#include "AutomationLaneViewBase.h"
#include "AutomationLaneMetrics.h"
#include "../ContinuousLane.h"

namespace Daw
{

class ContinuousAutomationLaneView final : public AutomationLaneViewBase
{
public:
    ContinuousAutomationLaneView (juce::ValueTree laneNode,
                                  const TimelineTransform& transform,
                                  AutomationModel* model,
                                  const juce::String& parameterId)
        : AutomationLaneViewBase (std::move (laneNode), transform, model),
          range_ (AutomationValueRange::forContinuousParameter (parameterId))
    {
        paramLabel_ = range_.paramLabel;
        minLabel_   = range_.minLabel;
        maxLabel_   = range_.maxLabel;

        // PRD-0094: seed the lane key from the node so editing dispatches to the
        // correct (owner, parameterId) pair without a separate setLaneKey() call.
        setLaneKey (getLaneNode().getProperty (AutomationIDs::owner).toString(), parameterId);
        setWantsKeyboardFocus (true);
    }

    const AutomationValueRange& getValueRange() const noexcept { return range_; }

    //--------------------------------------------------------------------------
    // Test/inspection geometry: the ordered (x,y) polyline points the lane WOULD
    // draw for the current body bounds + transform. Step segments insert the
    // intermediate "corner" point so step vs linear geometry differs.
    //--------------------------------------------------------------------------
    struct Point { double x; double y; };

    std::vector<Point> computePolyline() const
    {
        std::vector<Point> pts;

        ContinuousLane lane (getLaneNode());
        if (! lane.isValid())
            return pts;

        const auto body = getBodyBounds();
        const int n = lane.getNumBreakpoints();
        if (n == 0)
            return pts;

        for (int i = 0; i < n; ++i)
        {
            auto bp = lane.getBreakpoint (i);
            const double x = sampleToBodyX (ContinuousLane::sampleOfNode (bp));
            const double y = valueToY (ContinuousLane::valueOfNode (bp), body);

            // For a step segment, the run is horizontal to the NEXT breakpoint's x
            // at the CURRENT y, then a vertical jump. Insert the corner point.
            if (i > 0)
            {
                auto prev = lane.getBreakpoint (i - 1);
                if (ContinuousLane::interpolationOfNode (prev) == Interpolation::Step)
                {
                    const double prevY = valueToY (ContinuousLane::valueOfNode (prev), body);
                    pts.push_back ({ x, prevY }); // horizontal run end (corner)
                }
            }

            pts.push_back ({ x, y });
        }

        return pts;
    }

    int getBreakpointCount() const
    {
        ContinuousLane lane (getLaneNode());
        return lane.isValid() ? lane.getNumBreakpoints() : 0;
    }

    //--------------------------------------------------------------------------
    // PRD-0094 editing — test/inspection hooks.
    //--------------------------------------------------------------------------
    juce::ValueTree getSelectedBreakpoint() const noexcept { return selected_; }

    // Map a click/drag y to a raw value, clamped to the display window. The
    // command layer re-clamps to the full native DSP range (§1.5.6).
    double yToValue (double y) const
    {
        const auto body = getBodyBounds();
        const double top    = (double) body.getY() + 1.0;
        const double bottom = (double) body.getBottom() - 1.0;
        const double span   = bottom - top;
        const double norm   = span <= 0.0 ? 0.5 : juce::jlimit (0.0, 1.0, (bottom - y) / span);
        return range_.denormalise (norm);
    }

protected:
    //--------------------------------------------------------------------------
    // PRD-0094 (revised) — Logic-style editing gestures wired into the seams:
    //   * click an empty spot   → create a node there and arm it for the same-
    //                             gesture drag (Logic's pointer-tool behaviour);
    //   * click a node          → select it;
    //   * drag a node           → the node AND its connecting segments follow
    //                             the cursor live (preview); ONE model write
    //                             lands on mouse-up (single undo step);
    //   * double-click a node   → delete it;
    //   * hover a node          → hollow highlight ring (instant feedback);
    //   * while dragging        → a value readout chip rides the node.
    //--------------------------------------------------------------------------

    // DOUBLE-CLICK on a node → DeleteBreakpoint (Logic). Empty double-clicks are
    // covered by the click-to-create path of the first click.
    void onBodyMouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (editDispatcher() == nullptr) return;

        auto bp = hitTestBreakpoint ((double) event.x, (double) event.y);
        if (bp.isValid())
        {
            editDispatcher()->deleteBreakpoint (owner(), parameterId(), bp);
            if (bp == selected_)
                selected_ = juce::ValueTree();
            hovered_ = juce::ValueTree();
            repaint();
        }
    }

    // PRESS — select the node under the cursor, or CREATE one on an empty spot
    // (snapped time, clamped value) and arm it for the drag that may follow.
    void onBodyMouseDown (const juce::MouseEvent& event) override
    {
        dragging_ = false;
        if (event.mods.isPopupMenu())
        {
            showContextMenu (event);
            return;
        }

        selected_ = hitTestBreakpoint ((double) event.x, (double) event.y);

        if (! selected_.isValid() && editDispatcher() != nullptr)
        {
            const std::int64_t sample = bodyXToSample ((double) event.x, event);
            const double value = yToValue ((double) event.y);
            selected_ = editDispatcher()->addBreakpoint (owner(), parameterId(), sample, value);
        }
        repaint();
    }

    // Live drag preview of the selected breakpoint — the node and its segments
    // follow the cursor each frame; the model write lands on mouse-up (§1.4).
    void onBodyMouseDrag (const juce::MouseEvent& event) override
    {
        if (editDispatcher() == nullptr || ! selected_.isValid())
            return;

        dragging_ = true;
        previewSample_ = bodyXToSample ((double) event.x, event);
        previewValue_  = yToValue ((double) event.y);
        repaint();
    }

    // ONE MoveBreakpoint on drag end (snapped time, clamped value, §1.4 / §1.5.6).
    void onBodyMouseUp (const juce::MouseEvent& event) override
    {
        if (! dragging_ || editDispatcher() == nullptr || ! selected_.isValid())
            return;

        const std::int64_t sample = bodyXToSample ((double) event.x, event);
        const double value = yToValue ((double) event.y);
        editDispatcher()->moveBreakpoint (owner(), parameterId(), selected_, sample, value);
        dragging_ = false;
        repaint();
    }

    // HOVER — highlight the node under the cursor and hint the two affordances:
    // pointing hand over a node (drag it), crosshair over empty lane (create).
    void onBodyMouseMove (const juce::MouseEvent& event) override
    {
        auto hit = hitTestBreakpoint ((double) event.x, (double) event.y);
        setMouseCursor (hit.isValid() ? juce::MouseCursor::PointingHandCursor
                                      : juce::MouseCursor::CrosshairCursor);
        if (hit != hovered_)
        {
            hovered_ = hit;
            repaint();
        }
    }

    void onBodyMouseExit (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (hovered_.isValid())
        {
            hovered_ = juce::ValueTree();
            repaint();
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // Keyboard DELETE / BACKSPACE on the selected breakpoint (§1.4).
        if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            && editDispatcher() != nullptr && selected_.isValid())
        {
            editDispatcher()->deleteBreakpoint (owner(), parameterId(), selected_);
            selected_ = juce::ValueTree();
            repaint();
            return true;
        }
        return juce::Component::keyPressed (key);
    }


    void paintLaneBody (juce::Graphics& g, juce::Rectangle<int> /*body*/, bool /*enabled*/) override
    {
        const auto body = getBodyBounds();
        const auto dps  = computeDisplayBreakpoints();   // preview-substituted
        if (dps.empty())
            return;

        // Build the polyline from the DISPLAY breakpoints so a dragged node and
        // both its connecting segments follow the cursor live (Logic feel).
        std::vector<Point> pts;
        for (size_t i = 0; i < dps.size(); ++i)
        {
            if (i > 0 && dps[i - 1].step)
                pts.push_back ({ dps[i].x, dps[i - 1].y }); // step corner
            pts.push_back ({ dps[i].x, dps[i].y });
        }

        // ---- Dithered fill beneath the curve (constant density) ----------
        paintDitherUnderCurve (g, pts, body);

        // ---- Polyline (2-px ink) -----------------------------------------
        g.setColour (kInk);
        juce::Path path;
        path.startNewSubPath ((float) pts.front().x, (float) pts.front().y);
        for (size_t i = 1; i < pts.size(); ++i)
            path.lineTo ((float) pts[i].x, (float) pts[i].y);
        g.strokePath (path, juce::PathStrokeType (2.0f));

        // ---- Breakpoint dots: filled squares; the selected node is larger,
        // the hovered node carries a hollow highlight ring (instant feedback).
        for (const auto& dp : dps)
        {
            const float s = dp.selected ? 7.0f : 5.0f;
            g.setColour (kInk);
            g.fillRect ((float) dp.x - s * 0.5f, (float) dp.y - s * 0.5f, s, s);

            if (dp.hovered && ! dp.selected)
            {
                const float r = 11.0f;
                g.drawRect ((float) dp.x - r * 0.5f, (float) dp.y - r * 0.5f, r, r, 1.0f);
            }
        }

        // ---- Read-only HOLLOW live marker at the playhead -----------------
        ContinuousLane lane (getLaneNode());
        paintLiveMarker (g, lane, body);

        // ---- Value readout chip riding the dragged node --------------------
        if (dragging_ && selected_.isValid())
            paintValueReadout (g, body);
    }

private:
    juce::Rectangle<int> getBodyBounds() const
    {
        return getLocalBounds()
                   .withTrimmedLeft (DawLayout::kTrackHeaderWidth)
                   .reduced (0, AutomationLaneMetrics::kBodyInsetY);
    }

    //--------------------------------------------------------------------------
    // Display geometry: the model breakpoints with the live drag preview
    // substituted for the selected node, re-sorted by x so dragging a node past
    // its neighbour previews the same reorder the model commit performs.
    //--------------------------------------------------------------------------
    struct DisplayBp { double x; double y; bool step; bool selected; bool hovered; };

    std::vector<DisplayBp> computeDisplayBreakpoints() const
    {
        std::vector<DisplayBp> out;

        ContinuousLane lane (getLaneNode());
        if (! lane.isValid())
            return out;

        const auto body = getBodyBounds();
        const int n = lane.getNumBreakpoints();
        out.reserve ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            auto bp = lane.getBreakpoint (i);
            const bool sel = (bp == selected_);

            std::int64_t sample = ContinuousLane::sampleOfNode (bp);
            double       value  = ContinuousLane::valueOfNode (bp);
            if (sel && dragging_)
            {
                sample = previewSample_;
                value  = range_.clampToWindow (previewValue_);
            }

            out.push_back ({ sampleToBodyX (sample),
                             valueToY (value, body),
                             ContinuousLane::interpolationOfNode (bp) == Interpolation::Step,
                             sel,
                             bp == hovered_ });
        }

        std::stable_sort (out.begin(), out.end(),
                          [] (const DisplayBp& a, const DisplayBp& b) { return a.x < b.x; });
        return out;
    }

    // Formats the dragged value for the readout chip (native parameter units).
    juce::String formattedPreviewValue() const
    {
        const double v = range_.clampToWindow (previewValue_);
        juce::String text (v, std::abs (v) < 10.0 ? 2 : 1);
        if (v > 0.0 && range_.minValue < 0.0)
            text = "+" + text;
        return paramLabel_ + " " + text;
    }

    void paintValueReadout (juce::Graphics& g, juce::Rectangle<int> body)
    {
        const double x = sampleToBodyX (previewSample_);
        const double y = valueToY (range_.clampToWindow (previewValue_), body);

        const juce::String text = formattedPreviewValue();
        const juce::Font font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::bold));
        const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, text)) + 8;
        const int h = 14;

        // Place above-right of the node, flipped/clamped to stay inside the body.
        int bx = (int) std::llround (x) + 8;
        int by = (int) std::llround (y) - h - 6;
        if (bx + w > body.getRight()) bx = (int) std::llround (x) - w - 8;
        by = juce::jlimit (body.getY(), juce::jmax (body.getY(), body.getBottom() - h), by);
        bx = juce::jlimit (body.getX(), juce::jmax (body.getX(), body.getRight() - w), bx);

        const juce::Rectangle<int> chip (bx, by, w, h);
        g.setColour (kSurface);
        g.fillRect (chip);
        g.setColour (kInk);
        g.drawRect (chip, 1);
        g.setFont (font);
        g.drawText (text, chip, juce::Justification::centred, false);
    }

    //--------------------------------------------------------------------------
    // PRD-0094 editing helpers.
    //--------------------------------------------------------------------------

    // Nearest breakpoint within the hit radius (in px), else an invalid tree.
    juce::ValueTree hitTestBreakpoint (double px, double py) const
    {
        ContinuousLane lane (getLaneNode());
        if (! lane.isValid())
            return {};

        const auto body = getBodyBounds();
        constexpr double kHitRadius = 7.0;

        juce::ValueTree best;
        double bestDist = kHitRadius * kHitRadius;
        const int n = lane.getNumBreakpoints();
        for (int i = 0; i < n; ++i)
        {
            auto bp = lane.getBreakpoint (i);
            const double x = sampleToBodyX (ContinuousLane::sampleOfNode (bp));
            const double y = valueToY (ContinuousLane::valueOfNode (bp), body);
            const double dx = x - px, dy = y - py;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= bestDist)
            {
                bestDist = d2;
                best = bp;
            }
        }
        return best;
    }

    // The breakpoint whose OUTGOING segment contains px (the left endpoint of the
    // segment under the cursor), for the per-segment interpolation menu (§1.5.3).
    juce::ValueTree segmentLeftBreakpointAt (double px) const
    {
        ContinuousLane lane (getLaneNode());
        if (! lane.isValid())
            return {};

        const int n = lane.getNumBreakpoints();
        if (n < 2)
            return {};

        for (int i = 0; i + 1 < n; ++i)
        {
            const double x0 = sampleToBodyX (ContinuousLane::sampleOfNode (lane.getBreakpoint (i)));
            const double x1 = sampleToBodyX (ContinuousLane::sampleOfNode (lane.getBreakpoint (i + 1)));
            if (px >= x0 && px < x1)
                return lane.getBreakpoint (i);
        }
        return lane.getBreakpoint (n - 2);
    }

    // Right-click context menu: per-segment interpolation + delete (§1.4 / §1.5.3).
    void showContextMenu (const juce::MouseEvent& event)
    {
        if (editDispatcher() == nullptr)
            return;

        auto bp      = hitTestBreakpoint ((double) event.x, (double) event.y);
        auto segLeft = segmentLeftBreakpointAt ((double) event.x);

        juce::PopupMenu menu;
        menu.addItem (1, "Linear",     segLeft.isValid());
        menu.addItem (2, "Step / Hold", segLeft.isValid());
        menu.addSeparator();
        menu.addItem (3, "Delete Breakpoint", bp.isValid());

        const auto owner = this->owner();
        const auto param = this->parameterId();
        auto* dispatcher = editDispatcher();

        menu.showMenuAsync (juce::PopupMenu::Options(),
            [this, dispatcher, owner, param, segLeft, bp] (int result)
            {
                if (result == 1)
                    dispatcher->setBreakpointInterpolation (owner, param, segLeft, Interpolation::Linear);
                else if (result == 2)
                    dispatcher->setBreakpointInterpolation (owner, param, segLeft, Interpolation::Step);
                else if (result == 3)
                {
                    dispatcher->deleteBreakpoint (owner, param, bp);
                    if (bp == selected_)
                        selected_ = juce::ValueTree();
                }
                repaint();
            });
    }

    double valueToY (double rawValue, juce::Rectangle<int> body) const
    {
        const double norm = range_.normalise (rawValue); // 0 = min (bottom), 1 = top
        const double top    = (double) body.getY() + 1.0;
        const double bottom = (double) body.getBottom() - 1.0;
        return TimelineTransform::alignToPixelGrid (bottom - norm * (bottom - top));
    }

    void paintDitherUnderCurve (juce::Graphics& g,
                                const std::vector<Point>& pts,
                                juce::Rectangle<int> body)
    {
        // Constant-density checkerboard beneath the polyline. For each x column
        // inside the body we find the curve's y (piecewise-linear over pts) and
        // stipple a 1-px-on-2-px-grid pattern from there to the body bottom.
        if (pts.size() < 1)
            return;

        g.setColour (kInk);

        const int x0 = juce::jmax (body.getX(), (int) std::floor (pts.front().x));
        const int x1 = juce::jmin (body.getRight(), (int) std::ceil (pts.back().x));

        for (int x = x0; x < x1; x += 2)
        {
            const double curveY = curveYAt ((double) x, pts);
            const int yStart = juce::jmax (body.getY(), (int) std::ceil (curveY) + 1);
            for (int y = yStart + ((x / 2) % 2); y < body.getBottom(); y += 2)
                g.fillRect (x, y, 1, 1);
        }
    }

    static double curveYAt (double x, const std::vector<Point>& pts)
    {
        if (x <= pts.front().x) return pts.front().y;
        if (x >= pts.back().x)  return pts.back().y;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            if (x >= pts[i].x && x <= pts[i + 1].x)
            {
                const double span = pts[i + 1].x - pts[i].x;
                if (span <= 0.0) return pts[i].y;
                const double f = (x - pts[i].x) / span;
                return pts[i].y + (pts[i + 1].y - pts[i].y) * f;
            }
        }
        return pts.back().y;
    }

    void paintLiveMarker (juce::Graphics& g, const ContinuousLane& lane,
                          juce::Rectangle<int> body)
    {
        const std::int64_t sample = currentPlayheadSample();
        if (sample < 0)
            return;

        const auto applied = lane.evaluateAt (sample);
        if (! applied.has_value())
            return;

        const double x = sampleToBodyX (sample);
        if (x < (double) body.getX() || x > (double) body.getRight())
            return;
        const double y = valueToY (*applied, body);

        // HOLLOW square (2-px ink outline, surface centre) — distinct from the
        // FILLED breakpoint squares so the live cursor is never mistaken for an
        // editable point (§1.5.7).
        const float s = 7.0f;
        juce::Rectangle<float> marker ((float) x - s * 0.5f, (float) y - s * 0.5f, s, s);
        g.setColour (kSurface);
        g.fillRect (marker);
        g.setColour (kInk);
        g.drawRect (marker, 2.0f);
    }

    AutomationValueRange range_;

    // PRD-0094 editing state (message thread only).
    juce::ValueTree selected_;
    juce::ValueTree hovered_;
    bool            dragging_      { false };
    std::int64_t    previewSample_ { 0 };
    double          previewValue_  { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContinuousAutomationLaneView)
};

} // namespace Daw
