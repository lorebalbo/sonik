#pragma once
//==============================================================================
// PRD-0093: BooleanAutomationLaneView — renders ONE boolean lane
// (keyLock / pitchStretch / keyStepper) as solid #2d2d2d step blocks spanning
// each "on" interval against the #fdfdfd canvas (§1.5.3).
//
// Each "on" interval (a true step to the next false step, or to the lane's right
// edge) renders as a solid ink block, inset from the 2-px lane frame so the frame
// stays legible. Off intervals are bare surface. Shares the value-axis gutter +
// bypass button + read-only playhead treatment from AutomationLaneViewBase.
//==============================================================================

#include <cmath>
#include <vector>

#include "AutomationLaneViewBase.h"
#include "AutomationLaneMetrics.h"
#include "../BooleanLane.h"

namespace Daw
{

class BooleanAutomationLaneView final : public AutomationLaneViewBase
{
public:
    BooleanAutomationLaneView (juce::ValueTree laneNode,
                               const TimelineTransform& transform,
                               AutomationModel* model,
                               const juce::String& parameterId)
        : AutomationLaneViewBase (std::move (laneNode), transform, model)
    {
        paramLabel_ = AutomationValueRange::booleanParamLabel (parameterId);
        minLabel_   = "OFF";
        maxLabel_   = "ON";

        // PRD-0094: seed the lane key for editing dispatch.
        setLaneKey (getLaneNode().getProperty (AutomationIDs::owner).toString(), parameterId);
        setWantsKeyboardFocus (true);
    }

    //--------------------------------------------------------------------------
    // Test/inspection geometry: the on-interval blocks [xStart, xEnd] in this
    // component's coordinate space for the current transform.
    //--------------------------------------------------------------------------
    struct Block { double xStart; double xEnd; };

    std::vector<Block> computeBlocks() const
    {
        std::vector<Block> blocks;

        BooleanLane lane (getLaneNode());
        if (! lane.isValid())
            return blocks;

        const auto body = getBodyBounds();
        const int n = lane.getNumSteps();

        bool        on     = false;
        std::int64_t onFrom = 0;

        for (int i = 0; i < n; ++i)
        {
            auto st = lane.getStep (i);
            const std::int64_t s = BooleanLane::sampleOfNode (st);
            const bool v = BooleanLane::valueOfNode (st);

            if (v && ! on)
            {
                on = true;
                onFrom = s;
            }
            else if (! v && on)
            {
                on = false;
                pushBlock (blocks, onFrom, s, body);
            }
        }

        // A trailing "on" runs to the right edge of the body.
        if (on)
        {
            // body.getRight() is component-x; the content axis starts at the gutter,
            // so subtract it before inverting the transform (mirrors sampleToBodyX).
            const std::int64_t endSample = transform_.xToSample (
                (double) body.getRight() - (double) DawLayout::kTrackHeaderWidth);
            pushBlock (blocks, onFrom, endSample, body);
        }

        return blocks;
    }

    int getStepCount() const
    {
        BooleanLane lane (getLaneNode());
        return lane.isValid() ? lane.getNumSteps() : 0;
    }

    //--------------------------------------------------------------------------
    // PRD-0094 editing — test/inspection hook.
    //--------------------------------------------------------------------------
    juce::ValueTree getSelectedStep() const noexcept { return selected_; }

protected:
    //--------------------------------------------------------------------------
    // PRD-0094 editing gestures (§1.5.5: horizontal-only step drags).
    //--------------------------------------------------------------------------

    // DOUBLE-CLICK adds a toggle. The new step's state ALTERNATES the held state
    // at that sample, so adding a toggle flips the lane from that point (§1.5.5).
    void onBodyMouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (editDispatcher() == nullptr) return;

        const std::int64_t sample = bodyXToSample ((double) event.x, event);
        BooleanLane lane (getLaneNode());
        const bool priorState = lane.isValid() ? lane.stateAt (sample) : false;
        selected_ = editDispatcher()->addBooleanStep (owner(), parameterId(), sample, ! priorState);
        repaint();
    }

    void onBodyMouseDown (const juce::MouseEvent& event) override
    {
        dragging_ = false;
        if (event.mods.isPopupMenu())
        {
            showContextMenu (event);
            return;
        }
        selected_ = hitTestStep ((double) event.x);
        repaint();
    }

    // Horizontal-only drag preview; vertical movement ignored (§1.5.5).
    void onBodyMouseDrag (const juce::MouseEvent& event) override
    {
        if (editDispatcher() == nullptr || ! selected_.isValid())
            return;
        dragging_ = true;
        previewSample_ = bodyXToSample ((double) event.x, event);
        repaint();
    }

    // ONE MoveBooleanStep on drag end (grid-snapped time only, §1.5.5).
    void onBodyMouseUp (const juce::MouseEvent& event) override
    {
        if (! dragging_ || editDispatcher() == nullptr || ! selected_.isValid())
            return;
        const std::int64_t sample = bodyXToSample ((double) event.x, event);
        editDispatcher()->moveBooleanStep (owner(), parameterId(), selected_, sample);
        dragging_ = false;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            && editDispatcher() != nullptr && selected_.isValid())
        {
            editDispatcher()->deleteBooleanStep (owner(), parameterId(), selected_);
            selected_ = juce::ValueTree();
            repaint();
            return true;
        }
        return juce::Component::keyPressed (key);
    }

    void paintLaneBody (juce::Graphics& g, juce::Rectangle<int> /*body*/, bool /*enabled*/) override
    {
        const auto body = getBodyBounds();

        // Blocks are inset from the lane frame top/bottom so the 2-px frame stays
        // visible (§1.5.3).
        const int blockTop    = body.getY() + 2;
        const int blockBottom = body.getBottom() - 2;
        const int blockH      = juce::jmax (1, blockBottom - blockTop);

        g.setColour (kInk);
        for (const auto& b : computeBlocks())
        {
            const double xs = juce::jmax ((double) body.getX(), b.xStart);
            const double xe = juce::jmin ((double) body.getRight(), b.xEnd);
            const double w  = xe - xs;
            if (w <= 0.0)
                continue;
            g.fillRect ((float) xs, (float) blockTop, (float) w, (float) blockH);
        }
    }

private:
    juce::Rectangle<int> getBodyBounds() const
    {
        return getLocalBounds()
                   .withTrimmedLeft (DawLayout::kTrackHeaderWidth)
                   .reduced (0, AutomationLaneMetrics::kBodyInsetY);
    }

    void pushBlock (std::vector<Block>& blocks,
                    std::int64_t fromSample, std::int64_t toSample,
                    juce::Rectangle<int> /*body*/) const
    {
        const double xs = sampleToBodyX (fromSample);
        const double xe = sampleToBodyX (toSample);
        blocks.push_back ({ xs, xe });
    }

    //--------------------------------------------------------------------------
    // PRD-0094 editing helpers.
    //--------------------------------------------------------------------------
    juce::ValueTree hitTestStep (double px) const
    {
        BooleanLane lane (getLaneNode());
        if (! lane.isValid())
            return {};

        constexpr double kHitRadius = 8.0;
        juce::ValueTree best;
        double bestDist = kHitRadius;
        const int n = lane.getNumSteps();
        for (int i = 0; i < n; ++i)
        {
            auto st = lane.getStep (i);
            const double x = sampleToBodyX (BooleanLane::sampleOfNode (st));
            const double d = std::abs (x - px);
            if (d <= bestDist)
            {
                bestDist = d;
                best = st;
            }
        }
        return best;
    }

    void showContextMenu (const juce::MouseEvent& event)
    {
        if (editDispatcher() == nullptr)
            return;

        auto st = hitTestStep ((double) event.x);

        juce::PopupMenu menu;
        menu.addItem (1, "Delete Toggle", st.isValid());

        const auto owner = this->owner();
        const auto param = this->parameterId();
        auto* dispatcher = editDispatcher();

        menu.showMenuAsync (juce::PopupMenu::Options(),
            [this, dispatcher, owner, param, st] (int result)
            {
                if (result == 1)
                {
                    dispatcher->deleteBooleanStep (owner, param, st);
                    if (st == selected_)
                        selected_ = juce::ValueTree();
                    repaint();
                }
            });
    }

    // PRD-0094 editing state.
    juce::ValueTree selected_;
    bool            dragging_      { false };
    std::int64_t    previewSample_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BooleanAutomationLaneView)
};

} // namespace Daw
