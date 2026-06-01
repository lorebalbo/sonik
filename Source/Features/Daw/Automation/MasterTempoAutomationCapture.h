#pragma once
//==============================================================================
// PRD-0089: Master-Tempo Automation Lane & Capture.
//
// The master tempo (BPM) is NOT a plain ValueTree property — it is computed by
// MasterClockManager (PRD-0026) and published through the MasterClockSnapshot
// SeqLock. So unlike the per-channel ValueTree-listener taps (PRD-0088), tempo
// capture OBSERVES the authoritative published BPM and writes breakpoints to the
// single continuous lane keyed (master, tempo).
//
// This component never writes MasterClockManager, never computes a tempo, and
// never becomes a second tempo source: it only reads the authoritative BPM and
// records it. It is message-thread only and appends through the same
// AutomationAppendSink bridge used by PRD-0088.
//
// Driving: call seedAtRecordStart() once when recording begins, then captureTick()
// on the message-thread cadence (the projection / recording tick). Capture is
// idempotent against an unchanged BPM, so polling each tick produces no spurious
// breakpoints (PRD-0089 §1.5.7).
//==============================================================================

#include "AutomationCaptureTaps.h" // AutomationAppendSink + Interpolation
#include "AutomationIds.h"

#include <cmath>
#include <cstdint>
#include <functional>

namespace Daw
{

class MasterTempoAutomationCapture
{
public:
    // Dependencies injected (no singletons):
    //   masterBpm        : reads the authoritative published BPM (MasterClockSnapshot).
    //   isRecordingArmed : PRD-0071 gate (true while Armed OR Recording).
    //   recordPlayhead   : PRD-0071 record playhead (project samples).
    //   sink             : append bridge (PRD-0087 model).
    //   masterDeckIndex  : optional — current master deck index; a change of
    //                      identity at a BPM jump marks a handover (§1.5.6).
    MasterTempoAutomationCapture (std::function<double()>       masterBpm,
                                  std::function<bool()>         isRecordingArmed,
                                  std::function<std::int64_t()> recordPlayhead,
                                  AutomationAppendSink&         sink,
                                  std::function<int()>          masterDeckIndex = {})
        : masterBpm_        (std::move (masterBpm)),
          isRecordingArmed_ (std::move (isRecordingArmed)),
          recordPlayhead_   (std::move (recordPlayhead)),
          sink_             (sink),
          masterDeckIndex_  (std::move (masterDeckIndex))
    {
    }

    // Tunables (§1.5.1 / §1.5.4).
    void setChangeEpsilonBpm (double e)        { changeEpsilonBpm_ = e; }
    void setCollinearDeadbandBpm (double d)    { collinearDeadbandBpm_ = d; }

    //--------------------------------------------------------------------------
    // Seed the lane with the initial breakpoint at the record-start sample whose
    // value is the BPM in force at that instant (§1.5.5). Call once at the
    // arm/record-start edge. The authoritative snapshot retains the last-known
    // non-zero BPM while dormant, so the seed is never 0.0 (§1.5.1).
    void seedAtRecordStart()
    {
        const double bpm      = masterBpm_ ? masterBpm_() : 0.0;
        const std::int64_t s  = recordPlayhead_ ? recordPlayhead_() : 0;

        lastNode_   = sink_.appendBreakpoint (kOwner, kParam, s, bpm, Interpolation::Linear);
        lastSample_ = s;
        lastValue_  = bpm;
        hasLast_    = true;
        hasPrev_    = false;
        lastDeck_   = masterDeckIndex_ ? masterDeckIndex_() : -1;
    }

    //--------------------------------------------------------------------------
    // Sample the authoritative BPM and record it if it changed. No-op when
    // disarmed. Idempotent against an unchanged BPM (§1.5.7).
    void captureTick()
    {
        if (isRecordingArmed_ && ! isRecordingArmed_())
            return;

        const double bpm = masterBpm_ ? masterBpm_() : 0.0;
        const int    deck = masterDeckIndex_ ? masterDeckIndex_() : lastDeck_;

        if (! hasLast_)
        {
            // Armed without an explicit seed: treat the first observed BPM as the
            // seed so the lane is always defined from its first sample.
            const std::int64_t s = recordPlayhead_ ? recordPlayhead_() : 0;
            lastNode_   = sink_.appendBreakpoint (kOwner, kParam, s, bpm, Interpolation::Linear);
            lastSample_ = s;
            lastValue_  = bpm;
            hasLast_    = true;
            lastDeck_   = deck;
            return;
        }

        const bool deckChanged = (deck != lastDeck_);
        const bool bpmChanged  = std::abs (bpm - lastValue_) >= changeEpsilonBpm_;

        if (! bpmChanged)
        {
            lastDeck_ = deck; // track identity even when BPM held (pause/resume churn)
            return;           // §1.5.7: value-keyed — no breakpoint for unchanged BPM
        }

        const std::int64_t s = recordPlayhead_ ? recordPlayhead_() : 0;

        // Same-sample coalescing (§1.5.4): never two breakpoints on one sample.
        if (s == lastSample_)
        {
            if (lastNode_.isValid())
                sink_.updateBreakpoint (lastNode_, s, bpm);
            lastValue_ = bpm;
            lastDeck_  = deck;
            return;
        }

        if (deckChanged)
        {
            // Master handover (§1.5.6): the prior tempo holds until the handover
            // sample, then steps. Mark the predecessor segment step/hold and
            // append the handover breakpoint (no collinear thinning across a
            // discontinuity).
            if (lastNode_.isValid())
                sink_.setBreakpointInterpolation (lastNode_, Interpolation::Step);
            appendNew (s, bpm, Interpolation::Step);
            lastDeck_ = deck;
            return;
        }

        // Collinearity / deadband thinning (§1.5.4): if the incoming point lies
        // on the line through the previous two retained breakpoints (within the
        // deadband), it adds no shape — extend the last segment by moving the
        // last breakpoint to the new position instead of appending.
        if (hasPrev_)
        {
            const double span = static_cast<double> (lastSample_ - prevSample_);
            if (span > 0.0)
            {
                const double slope     = (lastValue_ - prevValue_) / span;
                const double predicted = prevValue_ + slope * static_cast<double> (s - prevSample_);
                if (std::abs (bpm - predicted) <= collinearDeadbandBpm_)
                {
                    if (lastNode_.isValid())
                        sink_.updateBreakpoint (lastNode_, s, bpm);
                    lastSample_ = s;
                    lastValue_  = bpm;
                    lastDeck_   = deck;
                    return;
                }
            }
        }

        appendNew (s, bpm, Interpolation::Linear);
        lastDeck_ = deck;
    }

private:
    void appendNew (std::int64_t s, double bpm, Interpolation interp)
    {
        prevSample_ = lastSample_;
        prevValue_  = lastValue_;
        hasPrev_    = true;

        lastNode_   = sink_.appendBreakpoint (kOwner, kParam, s, bpm, interp);
        lastSample_ = s;
        lastValue_  = bpm;
    }

    static constexpr const char* kOwner = "master";
    static constexpr const char* kParam = "tempo";

    std::function<double()>       masterBpm_;
    std::function<bool()>         isRecordingArmed_;
    std::function<std::int64_t()> recordPlayhead_;
    AutomationAppendSink&         sink_;
    std::function<int()>          masterDeckIndex_;

    double changeEpsilonBpm_     = 0.001;
    double collinearDeadbandBpm_ = 0.05;

    bool            hasLast_   { false };
    bool            hasPrev_   { false };
    juce::ValueTree lastNode_;
    std::int64_t    lastSample_ { 0 };
    double          lastValue_  { 0.0 };
    std::int64_t    prevSample_ { 0 };
    double          prevValue_  { 0.0 };
    int             lastDeck_   { -1 };
};

} // namespace Daw
