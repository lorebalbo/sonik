#pragma once
//==============================================================================
// PRD-0092: AutomationApplier — drives the live system from recorded automation
// during DAW timeline playback (EPIC-0011).
//
// On each message-thread tick, while (and only while) the transport is playing,
// the applier evaluates every ENABLED automation lane (PRD-0087) at the current
// transport playhead and writes the resulting value to the correct target
// THROUGH THE SAME SINGLE-SOURCE-OF-TRUTH PATH the live UI / MIDI controls use —
// never a parallel back door:
//
//   - Continuous mixer params (filter / gain / eq.high|mid|low) → the mixer
//     ValueTree property (EPIC-0007 smoothing downstream delivers them
//     click-free; the applier adds NO second smoother).
//   - Master tempo (owner "master", param "tempo") → an injected tempo sink,
//     bound in production to MasterClockManager::setAutomationTempoOverride so
//     the clock remains the ONE tempo authority (PRD-0026). The DAW never forks
//     tempo and never writes a second tempo value anywhere else.
//   - Deck booleans (keyLock / pitchStretch / keyStepper) → an injected boolean
//     sink keyed by (channel, paramId, state). The production binding routes
//     keyLock → deck IDs::keyLockEnabled; keyStepper / pitchStretch are derived
//     deck conditions (owned by PRD-0025), so the applier exposes the evaluated
//     boolean through the sink for production to route to the correct deck
//     control rather than inventing new deck state here.
//
// DELIVERY-PATH DECISION (PRD-0092 §1.5.1) — message thread only:
//   The DEFAULT (and, for the entire enumerated parameter set, the ONLY) path is
//   the message-thread single-source-of-truth path. None of the enumerated
//   mixer / tempo / boolean params require boundary sample-accuracy:
//     * mixer params are perceptually-continuous controls already protected by
//       EPIC-0007 smoothing,
//     * tempo publishes to the audio thread coherently via MasterClockPublisher,
//     * booleans are discrete events handled click-free by the deck DSP.
//   Therefore the applier evaluates lanes ONLY on the message thread; the audio
//   thread NEVER walks, evaluates, or writes a lane. Cross-thread delivery uses
//   the EXISTING lock-free snapshots (mixer atomic snapshot, master-clock
//   SeqLock). No automation field is added to ArrangementSnapshot (PRD-0079).
//   This satisfies the audio-thread-safety acceptance: nothing this PRD adds
//   allocates, locks, or does I/O on the audio thread.
//
// RE-ENTRANCY GUARD (PRD-0092 §1.5.2): before its writes each tick the applier
// raises an "applying automation" bool and lowers it after. Capture components
// (AutomationCaptureTaps / ChannelBooleanAutomationCapture /
// MasterTempoAutomationCapture) consult this bool via
// setApplyingAutomationGuard, so a value the applier writes is recognised as
// automation-originated and is NOT re-recorded — no capture⇄playback feedback
// loop. Both sides run on the message thread, so the bool is plain (non-atomic).
//
// WRITE-ON-CHANGE: boolean lanes write the sink ONLY when the evaluated state
// differs from the last state the applier wrote for that lane (so the deck does
// not receive a stream of identical toggles, §1.5.7). Continuous lanes write
// only when the evaluated value departs from the last written value by more than
// a tiny epsilon, to reduce ValueTree-listener churn (the downstream smoother
// dedups regardless). Tempo is also write-on-change.
//
// THREADING: message thread only. No singletons; all dependencies injected.
//==============================================================================

#include "AutomationModel.h"
#include "AutomationIds.h"
#include "ContinuousLane.h"
#include "BooleanLane.h"

#include "../Playback/DawTransport.h"
#include "../../Mixer/State/MixerStateSchema.h"
#include "../../Mixer/State/MixerIdentifiers.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace Daw
{

class AutomationApplier
{
public:
    //--------------------------------------------------------------------------
    // Sinks injected for the non-mixer targets (no singletons, per CLAUDE.md).
    //
    //   tempoSink   : receives the evaluated "master/tempo" BPM. Production binds
    //                 it to MasterClockManager::setAutomationTempoOverride.
    //   booleanSink : receives an evaluated deck boolean as (channelIndex,
    //                 paramId, state). Production routes keyLock → deck
    //                 IDs::keyLockEnabled and keyStepper / pitchStretch to the
    //                 appropriate deck control (PRD-0025).
    using TempoSink   = std::function<void (double bpm)>;
    using BooleanSink = std::function<void (int channel, const juce::String& paramId, bool state)>;

    // Maps the transport playhead into the TIMELINE-SAMPLE domain the recorded
    // breakpoints live in. Breakpoints are captured at the DAW now-line in
    // PROJECT-rate samples (44.1 kHz), but the DawTransport playhead advances in
    // RUNTIME/DEVICE-rate samples (EPIC-0010 scales clip positions to the device
    // rate). Without this conversion the applier evaluates lanes at the wrong
    // position whenever device rate != project rate, so automation plays back out
    // of sync (audio AND the UI controls that mirror it). Production binds this to
    // (runtime ÷ sampleRateScale); the default (empty) is identity for tests and
    // for paths that already work in the project-rate domain (offline export).
    using PlayheadDomainMap = std::function<std::int64_t (std::int64_t runtimePlayhead)>;

    //--------------------------------------------------------------------------
    AutomationApplier (AutomationModel&  model,
                       MixerStateSchema& mixer,
                       DawTransport&     transport,
                       TempoSink         tempoSink,
                       BooleanSink       booleanSink,
                       PlayheadDomainMap playheadDomainMap = {})
        : model_             (model),
          mixer_             (mixer),
          transport_         (transport),
          tempoSink_         (std::move (tempoSink)),
          booleanSink_       (std::move (booleanSink)),
          playheadDomainMap_ (std::move (playheadDomainMap))
    {
    }

    AutomationApplier (const AutomationApplier&)            = delete;
    AutomationApplier& operator= (const AutomationApplier&) = delete;

    //--------------------------------------------------------------------------
    // The re-entrancy guard predicate the capture components bind to. Returns
    // true only while tick() is mid-write. Pass this to each capture's
    // setApplyingAutomationGuard().
    std::function<bool()> makeApplyingGuard()
    {
        return [this] { return applying_; };
    }

    bool isApplying() const noexcept { return applying_; }

    //--------------------------------------------------------------------------
    // The applier's single unit of work. Call from a message-thread cadence
    // (production: a juce::Timer or the DAW playback tick). Separately callable
    // so tests drive it directly.
    //
    //   - Transport NOT playing ⇒ no evaluation, no writes, fully inert.
    //   - Else evaluate every enabled lane at the playhead and write its target.
    void tick()
    {
        if (! transport_.isPlaying())
            return;

        std::int64_t playhead = transport_.getPlayheadSample();
        if (playhead < 0)
            return; // defensive: stopped sentinel

        // Convert the runtime-rate transport playhead into the project-rate
        // timeline domain the breakpoints were recorded in, so lanes evaluate at
        // the correct musical position regardless of the device sample rate.
        if (playheadDomainMap_)
            playhead = playheadDomainMap_ (playhead);

        // Raise the guard for the whole write batch so capture ignores our writes.
        applying_ = true;

        const auto lanes = model_.getAllLanes();
        for (const auto& laneNode : lanes)
        {
            if (! AutomationModel::isLaneEnabled (laneNode))
                continue; // bypassed: no evaluation, no write (§1.5.3)

            const juce::String owner = laneNode.getProperty (AutomationIDs::owner).toString();
            const juce::String param = laneNode.getProperty (AutomationIDs::parameterId).toString();
            const LaneKind     kind  = model_.getLaneKind (laneNode);

            if (kind == LaneKind::Continuous)
                applyContinuous (laneNode, owner, param, playhead);
            else
                applyBoolean (laneNode, owner, param, playhead);
        }

        applying_ = false;
    }

private:
    //--------------------------------------------------------------------------
    // A continuous lane: evaluate then route to the correct target.
    void applyContinuous (const juce::ValueTree& laneNode,
                          const juce::String&    owner,
                          const juce::String&    param,
                          std::int64_t           playhead)
    {
        const ContinuousLane lane { laneNode };
        if (! lane.isValid())
            return;

        const auto value = lane.evaluateAt (playhead);
        if (! value.has_value())
            return; // empty lane: leave the live control untouched (§1.5.3)

        if (owner == AutomationStrings::kOwnerMaster && param == "tempo")
        {
            writeTempo (laneNode, *value);
            return;
        }

        writeMixerContinuous (owner, param, *value);
    }

    //--------------------------------------------------------------------------
    // Master tempo → injected sink, write-on-change (epsilon-keyed).
    void writeTempo (const juce::ValueTree& laneNode, double bpm)
    {
        const std::string key = laneKey (laneNode);
        auto it = lastContinuous_.find (key);
        if (it != lastContinuous_.end() && std::abs (it->second - bpm) <= kContinuousEpsilon)
            return;

        lastContinuous_[key] = bpm;
        if (tempoSink_)
            tempoSink_ (bpm);
    }

    //--------------------------------------------------------------------------
    // Mixer continuous param → the authoritative mixer ValueTree property (the
    // same path live UI / MIDI use; EPIC-0007 smoothing applies downstream).
    void writeMixerContinuous (const juce::String& owner,
                               const juce::String& param,
                               double              value)
    {
        const int ch = MixerIDs::letterToChannelIndex (juce::StringRef (owner.toRawUTF8()));
        if (ch < 0)
            return; // unknown owner: not a mixer channel

        juce::ValueTree  tree;
        juce::Identifier prop;

        if (param == "filter")       { tree = mixer_.getChannelTree (ch);   prop = MixerIDs::filter; }
        else if (param == "gain")    { tree = mixer_.getChannelTree (ch);   prop = MixerIDs::gain;   }
        else if (param == "eq.high" || param == "high")
                                     { tree = mixer_.getChannelEqTree (ch); prop = MixerIDs::high;   }
        else if (param == "eq.mid"  || param == "mid")
                                     { tree = mixer_.getChannelEqTree (ch); prop = MixerIDs::mid;    }
        else if (param == "eq.low"  || param == "low")
                                     { tree = mixer_.getChannelEqTree (ch); prop = MixerIDs::low;    }
        else
            return; // not an enumerated continuous mixer param

        if (! tree.isValid())
            return;

        // Write-on-change beyond a tiny epsilon to reduce listener churn. Read the
        // CURRENT property so we only mutate when the value actually moves (and so
        // we never re-write a value a live touch already set to the same point).
        const double current = (double) tree.getProperty (prop, value);
        if (std::abs (current - value) <= kContinuousEpsilon)
            return;

        // Stored verbatim in native units (filter bipolar [-1,1]; eq/gain dB).
        tree.setProperty (prop, value, nullptr);
    }

    //--------------------------------------------------------------------------
    // A boolean lane: evaluate held-step state, write the sink ONLY on change vs
    // the last state the applier wrote for THIS lane (§1.5.7).
    void applyBoolean (const juce::ValueTree& laneNode,
                       const juce::String&    owner,
                       const juce::String&    param,
                       std::int64_t           playhead)
    {
        const BooleanLane lane { laneNode };
        if (! lane.isValid())
            return;

        const int ch = MixerIDs::letterToChannelIndex (juce::StringRef (owner.toRawUTF8()));
        if (ch < 0)
            return; // boolean lanes are per-deck channels A..D only

        const bool state = lane.stateAt (playhead);

        const std::string key = laneKey (laneNode);
        auto it = lastBoolean_.find (key);
        if (it != lastBoolean_.end() && it->second == state)
            return; // unchanged since the last applier write: no redundant write

        lastBoolean_[key] = state;
        if (booleanSink_)
            booleanSink_ (ch, param, state);
    }

    //--------------------------------------------------------------------------
    static std::string laneKey (const juce::ValueTree& laneNode)
    {
        return (laneNode.getProperty (AutomationIDs::owner).toString()
                + "/"
                + laneNode.getProperty (AutomationIDs::parameterId).toString())
                   .toStdString();
    }

    //--------------------------------------------------------------------------
    AutomationModel&  model_;
    MixerStateSchema& mixer_;
    DawTransport&     transport_;
    TempoSink         tempoSink_;
    BooleanSink       booleanSink_;
    PlayheadDomainMap playheadDomainMap_;

    // The re-entrancy bool the capture guards read (message thread, non-atomic).
    bool applying_ { false };

    // Per-lane last-written state (write-on-change). Keyed by "owner/param".
    std::unordered_map<std::string, double> lastContinuous_;
    std::unordered_map<std::string, bool>   lastBoolean_;

    // Continuous write epsilon (native units). Small enough to be inaudible after
    // smoothing, large enough to drop sub-quantum jitter.
    static constexpr double kContinuousEpsilon = 1.0e-6;
};

} // namespace Daw
