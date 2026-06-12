#pragma once
//==============================================================================
// PRD-0088: Automation Capture Taps.
//
// A generic, message-thread observation layer that watches the AUTHORITATIVE
// parameter ValueTrees (mixer channel/eq nodes, master-clock node, deck nodes)
// and, WHILE AND ONLY WHILE recording is armed, appends the matching automation
// event (continuous breakpoint or boolean step) to the PRD-0087 model via an
// injected append "bridge" sink.
//
// This component is deliberately ignorant of WHICH concrete parameters exist:
// the parameter-specific PRDs (tempo 0089, continuous 0090, boolean 0091)
// populate the registration table. The tap owns only: observe, gate, timestamp,
// thin, coalesce, append.
//
// THREADING: message thread only. It attaches juce::ValueTree::Listeners to the
// authoritative trees; no audio-thread code, no locks, no I/O. processBlock is
// untouched.
//==============================================================================

#include "AutomationIds.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <list>
#include <vector>

namespace Daw
{

//==============================================================================
// The append "bridge". AutomationCaptureTaps never mutates the PRD-0087 model
// directly — every append flows through this sink (production: writes the model
// on the message thread; tests: a spy). Methods return the created node so the
// tap can coalesce a same-sample burst by updating the last event in place.
struct AutomationAppendSink
{
    virtual ~AutomationAppendSink() = default;

    virtual juce::ValueTree appendBreakpoint (const juce::String& owner,
                                              const juce::String& parameterId,
                                              std::int64_t        timelineSample,
                                              double              value,
                                              Interpolation       interpolation) = 0;

    virtual void updateBreakpoint (juce::ValueTree breakpoint,
                                   std::int64_t    timelineSample,
                                   double          value) = 0;

    // Set the per-segment interpolation on an existing breakpoint (the segment
    // STARTS at that breakpoint). Used by tempo capture to mark a master-handover
    // segment as step/hold (PRD-0089 §1.5.6).
    virtual void setBreakpointInterpolation (juce::ValueTree breakpoint,
                                             Interpolation   interpolation) = 0;

    virtual juce::ValueTree appendStep (const juce::String& owner,
                                        const juce::String& parameterId,
                                        std::int64_t        timelineSample,
                                        bool                value) = 0;

    // Remove a previously-appended step node from its boolean lane. Used by the
    // PRD-0091 debounce coalescer to drop a degenerate zero-duration on/off pair
    // (the earlier step is removed and the running state reverted). A no-op for
    // an invalid / already-detached node.
    virtual void removeStep (juce::ValueTree step) = 0;
};

//==============================================================================
// Continuous-lane thinning policy (§1.5.2). A new value is appended only when it
// departs from the last appended value by more than `valueDeadband`. The first
// value of a lane is always appended. Boolean lanes ignore this policy.
struct ThinningPolicy
{
    double valueDeadband = 0.0; // raw parameter units; 0 disables value thinning
};

//==============================================================================
class AutomationCaptureTaps final : private juce::ValueTree::Listener
{
public:
    // Dependencies are injected (no singletons, per CLAUDE.md):
    //   isRecordingArmed : the PRD-0071 gate — true while Armed OR Recording.
    //   recordPlayhead   : the PRD-0071 record playhead (project samples) read
    //                      at the change instant.
    //   sink             : the append bridge (PRD-0072 path / PRD-0087 model).
    AutomationCaptureTaps (std::function<bool()>         isRecordingArmed,
                           std::function<std::int64_t()> recordPlayhead,
                           AutomationAppendSink&         sink);

    ~AutomationCaptureTaps() override;

    AutomationCaptureTaps (const AutomationCaptureTaps&)            = delete;
    AutomationCaptureTaps& operator= (const AutomationCaptureTaps&) = delete;

    //--------------------------------------------------------------------------
    // Registration (§1.5.7). Each call binds one authoritative (sourceTree,
    // property) to one lane key (owner, parameterId). The parameter-specific
    // PRDs call these; the generic contract here never changes.

    void registerContinuousTap (juce::ValueTree         sourceTree,
                                const juce::Identifier& property,
                                const juce::String&     owner,
                                const juce::String&     parameterId,
                                Interpolation           interpolation = Interpolation::Linear,
                                ThinningPolicy          thinning      = {});

    void registerBooleanTap (juce::ValueTree         sourceTree,
                             const juce::Identifier& property,
                             const juce::String&     owner,
                             const juce::String&     parameterId);

    //--------------------------------------------------------------------------
    // PRD-0092 re-entrancy guard. When set and returning true, the tap treats
    // every observed property change as automation-originated (the playback
    // applier is currently writing the authoritative tree) and records NOTHING,
    // preventing a capture⇄playback feedback loop. Default (null) ⇒ unchanged
    // behaviour. Message-thread only — a plain predicate, no atomics needed.
    void setApplyingAutomationGuard (std::function<bool()> guard)
    {
        applyingAutomationGuard_ = std::move (guard);
    }

    //--------------------------------------------------------------------------
    // Capture the current authoritative value of every registered tap as that
    // lane's initial breakpoint/step at `timelineSample`, and prime each tap's
    // running state so subsequent thinning is measured against the seed. Driven
    // by the record-arm transition (PRD-0090 §1.5.6 / PRD-0091 record-start
    // seeding) — a one-shot snapshot, not a per-parameter callback.
    void captureInitialValues (std::int64_t timelineSample);

    // Append each continuous tap's current resting value as a final breakpoint
    // so a decimated sweep terminates on the exact value the control came to
    // rest at, rather than a thinned approximation (PRD-0090 §1.5.1). Called on
    // the record-stop edge. Boolean taps are unaffected (every toggle is kept).
    void flush (std::int64_t timelineSample);

    int getNumTaps() const noexcept { return (int) taps_.size(); }

    //--------------------------------------------------------------------------
    // Rest-gap discontinuity threshold. When a continuous change arrives after
    // the control sat untouched for at least this many timeline samples, the
    // control was AT REST: the previous breakpoint's outgoing segment is marked
    // step/hold so playback holds the resting value until the change instant and
    // then jumps — instead of drawing a slow linear ramp across the whole idle
    // span (the double-click-reset / re-touch artifact). Within an active
    // gesture appends arrive far more often than this, so sweeps stay linear.
    // Default: 0.5 s at the 44.1 kHz project rate breakpoints are stamped in.
    static constexpr std::int64_t kDefaultRestGapSamples = 22050;

    void setRestGapSamples (std::int64_t samples) noexcept { restGapSamples_ = samples; }
    std::int64_t getRestGapSamples() const noexcept        { return restGapSamples_; }

private:
    //--------------------------------------------------------------------------
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    struct Tap
    {
        juce::ValueTree   sourceTree;
        juce::Identifier  property;
        juce::String      owner;
        juce::String      parameterId;
        LaneKind          kind { LaneKind::Continuous };
        Interpolation     interpolation { Interpolation::Linear };
        ThinningPolicy    thinning;

        // Per-lane running capture state (continuous thinning / coalescing).
        bool              hasLast { false };
        std::int64_t      lastSample { 0 };
        double            lastValue { 0.0 };
        juce::ValueTree   lastBreakpoint;
    };

    void ensureListenerOn (juce::ValueTree tree);
    void handleContinuous (Tap& tap, double value, std::int64_t playhead);
    void handleBoolean   (Tap& tap, bool value, std::int64_t playhead);

    std::function<bool()>         isRecordingArmed_;
    std::function<std::int64_t()> recordPlayhead_;
    AutomationAppendSink&         sink_;
    std::function<bool()>         applyingAutomationGuard_; // PRD-0092, optional

    std::int64_t                 restGapSamples_ { kDefaultRestGapSamples };

    std::vector<Tap>             taps_;
    std::list<juce::ValueTree>   listenedTrees_; // stable addresses: a moved
                                                 // ValueTree loses its listener
                                                 // registration, so never store
                                                 // these in a reallocating vector.
};

//==============================================================================
// Production bridge: appends straight into the PRD-0087 AutomationModel on the
// message thread. Defined in the .cpp to avoid a hard header dependency here.
class AutomationModel;

class ModelAutomationAppendSink final : public AutomationAppendSink
{
public:
    explicit ModelAutomationAppendSink (AutomationModel& model, juce::UndoManager* undo = nullptr)
        : model_ (model), undo_ (undo) {}

    juce::ValueTree appendBreakpoint (const juce::String& owner,
                                      const juce::String& parameterId,
                                      std::int64_t        timelineSample,
                                      double              value,
                                      Interpolation       interpolation) override;

    void updateBreakpoint (juce::ValueTree breakpoint,
                           std::int64_t    timelineSample,
                           double          value) override;

    void setBreakpointInterpolation (juce::ValueTree breakpoint,
                                     Interpolation   interpolation) override;

    juce::ValueTree appendStep (const juce::String& owner,
                                const juce::String& parameterId,
                                std::int64_t        timelineSample,
                                bool                value) override;

    void removeStep (juce::ValueTree step) override;

private:
    AutomationModel&   model_;
    juce::UndoManager* undo_ { nullptr };
};

} // namespace Daw
