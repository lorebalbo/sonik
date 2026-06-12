#pragma once
//==============================================================================
// PRD-0093: AutomationLaneStackView — the vertical list of one owner's automation
// lanes, revealed beneath a channel group's three source lanes (§1.5.1).
//
// Lane order (§1.5.4): continuous filter / high / mid / low / gain, then boolean
// keyLock / pitchStretch / keyStepper. Populated lanes (lane node present in the
// model) are listed FIRST in that relative order, then empty ones — so the DJ
// sees recorded lanes without scrolling. Lane nodes are resolved-or-created from
// the AutomationModel for the owner (channel letter from deckIndex via the
// identity A..D, §1.5.4 of PRD-0090).
//
// Each lane is a fixed modest height (kAutomationLaneHeight). The stack reports
// its preferred height so the owning group/panel can reflow. View-only; observes
// the model via the lane views' own listeners; no audio-thread code.
//==============================================================================

#include <array>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AutomationLaneMetrics.h"
#include "ContinuousAutomationLaneView.h"
#include "BooleanAutomationLaneView.h"
#include "../AutomationModel.h"
#include "../AutomationIds.h"
#include "../../Transform/TimelineTransform.h"

namespace Daw
{

class AutomationLaneStackView final : public juce::Component
{
public:
    using PlayheadProvider = AutomationLaneViewBase::PlayheadProvider;

    // owner       — "A".."D" | "master".
    // model       — non-owning; the lane container backing the lanes.
    // transform   — shared horizontal time axis.
    AutomationLaneStackView (juce::String owner,
                             AutomationModel& model,
                             const TimelineTransform& transform)
        : owner_ (std::move (owner)),
          model_ (model),
          transform_ (transform)
    {
        buildLaneViews();
    }

    int getNumLaneViews() const noexcept { return static_cast<int> (laneViews_.size()); }

    //--------------------------------------------------------------------------
    // Grouped-tracks: the per-row height of this stack's lanes. Defaults to the
    // compact automation metric; a channel group sets it to the source-lane
    // height so its deck automation lane reads as a first-class track row.
    //--------------------------------------------------------------------------
    void setLaneRowHeight (int newHeight)
    {
        const int clamped = juce::jmax (AutomationLaneMetrics::kAutomationLaneHeight, newHeight);
        if (laneRowHeight_ == clamped)
            return;
        laneRowHeight_ = clamped;
        resized();
    }

    int getLaneRowHeight() const noexcept { return laneRowHeight_; }

    //--------------------------------------------------------------------------
    // Logic-style single-parameter display: when a parameterId is set, only that
    // lane is shown (the track header dropdown drives this); an empty id shows
    // every lane (the original PRD-0093 stack behaviour, kept for the master
    // lane / tests).
    //--------------------------------------------------------------------------
    void setVisibleParameter (const juce::String& parameterId)
    {
        if (visibleParam_ == parameterId)
            return;
        visibleParam_ = parameterId;
        for (size_t i = 0; i < laneViews_.size(); ++i)
            laneViews_[i]->setVisible (isLaneShown ((int) i));
        resized();
    }

    const juce::String& getVisibleParameter() const noexcept { return visibleParam_; }

    int getNumVisibleLaneViews() const noexcept
    {
        int n = 0;
        for (size_t i = 0; i < laneViews_.size(); ++i)
            if (isLaneShown ((int) i))
                ++n;
        return n;
    }

    // The canonical (parameterId, isBoolean) list this stack builds, in display
    // order — the track header dropdown menu is built from this.
    struct ParameterInfo { juce::String parameterId; bool isBoolean; };

    static std::vector<ParameterInfo> getAvailableParameters()
    {
        std::vector<ParameterInfo> out;
        for (const auto& spec : parameterSpecs())
            out.push_back ({ juce::String (spec.parameterId), spec.isBoolean });
        return out;
    }

    int getPreferredHeight() const noexcept
    {
        return getNumVisibleLaneViews() * laneRowHeight_;
    }

    // Inject the shared playhead-sample provider into every lane view.
    void setPlayheadProvider (PlayheadProvider provider)
    {
        for (auto& v : laneViews_)
            v->setPlayheadProvider (provider);
    }

    //--------------------------------------------------------------------------
    // PRD-0094: inject the shared EPIC-0010 edit dispatcher + the global snap
    // provider into every lane view so editing gestures route through the one
    // undo history. Retained so lanes built later receive them too.
    //--------------------------------------------------------------------------
    void setEditDispatcher (EditCommandDispatcher* dispatcher)
    {
        dispatcher_ = dispatcher;
        for (auto& v : laneViews_)
            v->setEditDispatcher (dispatcher);
    }

    void setSnapEnabledProvider (AutomationLaneViewBase::SnapEnabledProvider provider)
    {
        snapProvider_ = provider;
        for (auto& v : laneViews_)
            v->setSnapEnabledProvider (provider);
    }

    void refreshTransform()
    {
        for (auto& v : laneViews_)
            v->refreshTransform();
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        for (size_t i = 0; i < laneViews_.size(); ++i)
            if (isLaneShown ((int) i))
                laneViews_[i]->setBounds (bounds.removeFromTop (laneRowHeight_));
    }

    // Test access.
    AutomationLaneViewBase* getLaneView (int index) const
    {
        return (index >= 0 && index < (int) laneViews_.size())
                   ? laneViews_[static_cast<size_t> (index)].get()
                   : nullptr;
    }

private:
    struct LaneSpec { const char* parameterId; bool isBoolean; };

    // Canonical relative order (§1.5.4). One definition shared by the stack
    // builder and the public getAvailableParameters() menu source.
    static const std::array<LaneSpec, 8>& parameterSpecs()
    {
        static const std::array<LaneSpec, 8> kSpecs { {
            { "gain",         false },
            { "filter",       false },
            { "eq.high",      false },
            { "eq.mid",       false },
            { "eq.low",       false },
            { "keyLock",      true  },
            { "pitchStretch", true  },
            { "keyStepper",   true  },
        } };
        return kSpecs;
    }

    bool isLaneShown (int index) const noexcept
    {
        if (visibleParam_.isEmpty())
            return true;
        return index >= 0 && index < (int) laneParams_.size()
            && laneParams_[(size_t) index] == visibleParam_;
    }

    void buildLaneViews()
    {
        // Two passes: populated lanes first, then empty ones (§1.5.4). A lane is
        // "populated" if its node exists AND has at least one point.
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool wantPopulated = (pass == 0);
            for (const auto& spec : parameterSpecs())
            {
                const juce::String pid (spec.parameterId);
                const bool populated = laneHasData (pid, spec.isBoolean);
                if (populated != wantPopulated)
                    continue;
                addLaneView (pid, spec.isBoolean);
            }
        }
    }

    bool laneHasData (const juce::String& parameterId, bool isBoolean) const
    {
        auto node = model_.getLaneNode (owner_, parameterId);
        if (! node.isValid())
            return false;
        if (isBoolean)
            return BooleanLane (node).getNumSteps() > 0;
        return ContinuousLane (node).getNumBreakpoints() > 0;
    }

    void addLaneView (const juce::String& parameterId, bool isBoolean)
    {
        // Resolve-or-create the lane node so empty lanes are still listed.
        juce::ValueTree node;
        if (isBoolean)
            node = model_.getOrCreateBooleanLane (owner_, parameterId).getState();
        else
            node = model_.getOrCreateContinuousLane (owner_, parameterId).getState();

        std::unique_ptr<AutomationLaneViewBase> view;
        if (isBoolean)
            view = std::make_unique<BooleanAutomationLaneView> (node, transform_, &model_, parameterId);
        else
            view = std::make_unique<ContinuousAutomationLaneView> (node, transform_, &model_, parameterId);

        // PRD-0094: propagate the shared editing wiring to each lane as it builds.
        if (dispatcher_ != nullptr)
            view->setEditDispatcher (dispatcher_);
        if (snapProvider_)
            view->setSnapEnabledProvider (snapProvider_);

        addAndMakeVisible (*view);
        laneViews_.push_back (std::move (view));
        laneParams_.push_back (parameterId);

        // Honour an already-active single-parameter filter.
        laneViews_.back()->setVisible (isLaneShown (static_cast<int> (laneViews_.size()) - 1));
    }

    juce::String              owner_;
    AutomationModel&          model_;
    const TimelineTransform&  transform_;
    int                       laneRowHeight_ { AutomationLaneMetrics::kAutomationLaneHeight };
    juce::String              visibleParam_;   // empty = show all lanes
    std::vector<juce::String> laneParams_;     // parameterId per lane view
    std::vector<std::unique_ptr<AutomationLaneViewBase>> laneViews_;
    EditCommandDispatcher*    dispatcher_ { nullptr };
    AutomationLaneViewBase::SnapEnabledProvider snapProvider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneStackView)
};

} // namespace Daw
