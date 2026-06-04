#pragma once
//==============================================================================
// PRD-0091: Per-Channel Boolean Automation Lanes — Key-Lock / Pitch-Stretch /
// Key-Stepper (capture only).
//
// Owns the message-thread observation of the authoritative DECK ValueTree
// parameters (PRD-0011 `keyLockEnabled`, PRD-0025 `keyShift`) and captures three
// DERIVED boolean lanes per channel/deck into the PRD-0087 model via the shared
// AutomationAppendSink bridge:
//
//   - keyLock      ⇒ the deck's `keyLockEnabled` bool (direct, §1.5.5).
//   - keyStepper   ⇒ `keyShift != 0` — the neutral⇄engaged flag (§1.5.1). A step
//                    is emitted ONLY when keyShift crosses neutral; magnitude is
//                    never stored. Moves staying on one side of 0 emit nothing.
//   - pitchStretch ⇒ `keyLockEnabled || keyShift != 0` — the "stretcher engaged"
//                    condition (§1.5.5). Recomputed on either source change; a
//                    step is emitted only when this derived boolean transitions.
//
// Capture is GATED on the record-arm predicate (§1.5.5): while disarmed nothing
// is appended. On record start, captureInitialValues() seeds one step per lane
// at `timelineSample = recordStart` reflecting the derived value sampled then
// (§1.5.4). Rapid opposite transitions inside a short debounce window coalesce
// to nothing (§1.5.6): the earlier (degenerate) step is removed and the running
// state reverted, so the lane retains neither.
//
// THREADING: message thread only. This component attaches a
// juce::ValueTree::Listener to each deck node; no audio-thread code, no locks,
// no allocation on any audio path (there is none here). processBlock untouched.
//==============================================================================

#include "AutomationCaptureTaps.h"
#include "AutomationIds.h"

#include "../../Deck/DeckIdentifiers.h"

#include <array>
#include <cstdint>
#include <functional>

namespace Daw
{

class ChannelBooleanAutomationCapture final : private juce::ValueTree::Listener
{
public:
    // The three boolean parameter ids captured per channel, in a fixed order.
    static constexpr int kNumChannels = 4;

    // Default debounce window (§1.5.6): ≈ 5 ms at the 44.1 kHz project rate.
    // Configurable so tests can make coalescing deterministic.
    static constexpr std::int64_t kDefaultDebounceSamples = 220;

    // Dependencies are injected (no singletons, per CLAUDE.md):
    //   resolveDeckTree  : maps a channel index 0..3 to the authoritative deck
    //                      ValueTree it observes (identity channel↔deck, §1.5.2).
    //   isRecordingArmed : the PRD-0071 gate — true while Armed OR Recording.
    //   recordPlayhead   : the PRD-0071 record playhead (project samples) read
    //                      at the change instant.
    //   sink             : the append bridge (PRD-0087 model / spy).
    ChannelBooleanAutomationCapture (std::function<juce::ValueTree (int)> resolveDeckTree,
                                     std::function<bool()>                isRecordingArmed,
                                     std::function<std::int64_t()>        recordPlayhead,
                                     AutomationAppendSink&                sink)
        : resolveDeckTree_  (std::move (resolveDeckTree)),
          isRecordingArmed_ (std::move (isRecordingArmed)),
          recordPlayhead_   (std::move (recordPlayhead)),
          sink_             (sink)
    {
        for (int ch = 0; ch < kNumChannels; ++ch)
        {
            auto& lane = channels_[(size_t) ch];
            lane.owner    = ownerFor (ch);
            lane.deckTree = resolveDeckTree_ ? resolveDeckTree_ (ch) : juce::ValueTree {};

            // Prime the running derived state from the deck's current values so a
            // change BEFORE seeding (or before arming) does not look like a
            // spurious transition once we are live. captureInitialValues() will
            // re-prime exactly at record start.
            lane.params[Param::keyLock].state      = readKeyLock (lane.deckTree);
            lane.params[Param::keyStepper].state   = readKeyStepper (lane.deckTree);
            lane.params[Param::pitchStretch].state = readPitchStretch (lane.deckTree);

            // Register on the PERSISTENT member node (never a by-value copy): a
            // ValueTree's listener registration is tracked by the wrapper that
            // added it, so a temporary copy would silently drop it on
            // destruction.
            if (lane.deckTree.isValid())
                lane.deckTree.addListener (this);
        }
    }

    ~ChannelBooleanAutomationCapture() override
    {
        for (auto& lane : channels_)
            if (lane.deckTree.isValid())
                lane.deckTree.removeListener (this);
    }

    ChannelBooleanAutomationCapture (const ChannelBooleanAutomationCapture&)            = delete;
    ChannelBooleanAutomationCapture& operator= (const ChannelBooleanAutomationCapture&) = delete;

    //--------------------------------------------------------------------------
    // Record-start seeding (§1.5.4). Writes ONE initial step into each of the
    // twelve lanes (4 channels × 3 params) at `timelineSample`, with `state`
    // equal to the derived boolean sampled at that instant, and primes the
    // running state + debounce bookkeeping so subsequent transitions are
    // measured against the seed.
    void captureInitialValues (std::int64_t timelineSample)
    {
        for (auto& lane : channels_)
        {
            const bool kl = readKeyLock (lane.deckTree);
            const bool ks = readKeyStepper (lane.deckTree);
            const bool ps = readPitchStretch (lane.deckTree);

            seedParam (lane, Param::keyLock,      "keyLock",      timelineSample, kl);
            seedParam (lane, Param::pitchStretch, "pitchStretch", timelineSample, ps);
            seedParam (lane, Param::keyStepper,   "keyStepper",   timelineSample, ks);
        }
    }

    //--------------------------------------------------------------------------
    // Debounce-window configuration (§1.5.6). Test-overridable so coalescing is
    // deterministic; production uses the ≈5 ms default.
    void  setDebounceSamples (std::int64_t samples) noexcept { debounceSamples_ = samples; }
    std::int64_t getDebounceSamples() const noexcept         { return debounceSamples_; }

    //--------------------------------------------------------------------------
    // PRD-0092 re-entrancy guard. When set and returning true, deck-property
    // changes are treated as automation-originated (the playback applier is
    // writing the deck tree) and NOT captured. Default (null) ⇒ unchanged.
    void setApplyingAutomationGuard (std::function<bool()> guard)
    {
        applyingAutomationGuard_ = std::move (guard);
    }

private:
    //--------------------------------------------------------------------------
    enum Param { keyLock = 0, pitchStretch = 1, keyStepper = 2, numParams = 3 };

    struct ParamState
    {
        // Running derived boolean (last value we believe the lane holds).
        bool state { false };

        // Bookkeeping for debounce coalescing: the most recently APPENDED step,
        // its timeline sample, and the state BEFORE that step (so we can revert
        // when we drop a degenerate pair).
        juce::ValueTree lastStep;
        std::int64_t    lastStepSample { 0 };
        bool            hasLastStep { false };
        bool            stateBeforeLastStep { false };
    };

    struct ChannelLanes
    {
        juce::String    owner;
        juce::ValueTree deckTree;
        std::array<ParamState, (size_t) numParams> params {};
    };

    //--------------------------------------------------------------------------
    static juce::String ownerFor (int ch)
    {
        static const char* const owners[kNumChannels] = { "A", "B", "C", "D" };
        return juce::String (owners[ch]);
    }

    static bool readKeyLock (const juce::ValueTree& deck)
    {
        return deck.isValid() && (bool) deck.getProperty (IDs::keyLockEnabled, false);
    }

    static bool readKeyStepper (const juce::ValueTree& deck)
    {
        return deck.isValid() && (int) deck.getProperty (IDs::keyShift, 0) != 0;
    }

    static bool readPitchStretch (const juce::ValueTree& deck)
    {
        return readKeyLock (deck) || readKeyStepper (deck);
    }

    //--------------------------------------------------------------------------
    void seedParam (ChannelLanes&       lane,
                    Param               param,
                    const juce::String& parameterId,
                    std::int64_t        timelineSample,
                    bool                value)
    {
        auto& ps = lane.params[(size_t) param];
        ps.state               = value;
        ps.lastStep            = sink_.appendStep (lane.owner, parameterId, timelineSample, value);
        ps.lastStepSample      = timelineSample;
        ps.stateBeforeLastStep = value; // seed is its own predecessor: never coalesced away
        ps.hasLastStep         = true;
    }

    //--------------------------------------------------------------------------
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        // PRD-0092 re-entrancy guard: an automation-originated deck write is not
        // a live gesture — ignore it to prevent a capture⇄playback feedback loop.
        if (applyingAutomationGuard_ && applyingAutomationGuard_())
            return;

        // Gate (§1.5.5): while not armed, observe nothing / append nothing.
        if (isRecordingArmed_ && ! isRecordingArmed_())
            return;

        // We only care about the two authoritative source properties.
        const bool isKeyLock = (property == IDs::keyLockEnabled);
        const bool isKeyShift = (property == IDs::keyShift);
        if (! isKeyLock && ! isKeyShift)
            return;

        // Locate the channel whose deck node changed (per-deck independence).
        for (auto& lane : channels_)
        {
            if (! (lane.deckTree == tree))
                continue;

            const std::int64_t playhead = recordPlayhead_ ? recordPlayhead_() : 0;

            // keyLock follows keyLockEnabled directly.
            considerTransition (lane, Param::keyLock, "keyLock",
                                readKeyLock (lane.deckTree), playhead);

            // keyStepper is the neutral⇄engaged flag of keyShift.
            considerTransition (lane, Param::keyStepper, "keyStepper",
                                readKeyStepper (lane.deckTree), playhead);

            // pitchStretch is the derived "stretcher engaged" condition; it must
            // be recomputed on ANY change to either source property.
            considerTransition (lane, Param::pitchStretch, "pitchStretch",
                                readPitchStretch (lane.deckTree), playhead);

            return; // a deck node belongs to exactly one channel
        }
    }

    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    //--------------------------------------------------------------------------
    // Append a step only when the derived boolean actually transitions, applying
    // debounce coalescing (§1.5.6).
    void considerTransition (ChannelLanes&       lane,
                             Param               param,
                             const juce::String& parameterId,
                             bool                newValue,
                             std::int64_t        playhead)
    {
        auto& ps = lane.params[(size_t) param];

        // Same-direction repeat (state unchanged) is a no-op (§1.5.6).
        if (newValue == ps.state)
            return;

        // Debounce (§1.5.6): if this transition is the OPPOSITE of the most
        // recent appended step and arrives within the window, the prior step and
        // this one form a degenerate zero-duration pair. Drop the earlier step
        // and revert the running state to what it was before that step — the
        // lane retains neither.
        if (ps.hasLastStep
            && ps.lastStep.isValid()
            && (playhead - ps.lastStepSample) <= debounceSamples_
            && newValue == ps.stateBeforeLastStep)
        {
            sink_.removeStep (ps.lastStep);
            ps.state       = ps.stateBeforeLastStep;
            ps.lastStep    = juce::ValueTree {};
            ps.hasLastStep = false;
            return;
        }

        // Genuine transition: append a step at the current playhead.
        const bool previousState = ps.state;
        ps.lastStep            = sink_.appendStep (lane.owner, parameterId, playhead, newValue);
        ps.lastStepSample      = playhead;
        ps.stateBeforeLastStep = previousState;
        ps.hasLastStep         = true;
        ps.state               = newValue;
    }

    //--------------------------------------------------------------------------
    std::function<juce::ValueTree (int)> resolveDeckTree_;
    std::function<bool()>                isRecordingArmed_;
    std::function<std::int64_t()>        recordPlayhead_;
    AutomationAppendSink&                sink_;
    std::function<bool()>                applyingAutomationGuard_; // PRD-0092, optional

    std::array<ChannelLanes, (size_t) kNumChannels> channels_ {};
    std::int64_t                                     debounceSamples_ { kDefaultDebounceSamples };
};

} // namespace Daw
