#pragma once
//==============================================================================
// PRD-0053 / PRD-0057: CrossfaderSnapshot — per-block crossfader parameter
// snapshot. Populated once per block from MixerAtomicSnapshot.
//==============================================================================

//------------------------------------------------------------------------------
// PRD-0057 §1.5.1: curve enum.
//
// `Smooth` and `Sharp` are the two values exposed through the ValueTree string
// field `mixer.crossfader.curve` (PRD-0052 amendment). The integer values
// 0 and 1 are the wire format used by the audio-thread atomic mirror.
//
// `Linear` is an INTERNAL value, never written by the message thread and
// never reachable from the ValueTree validator. It is intentionally provided
// so that pure-routing unit tests (e.g. SignalFlowTests) can drive the
// pipeline with a 1:1 (1-p, p) law that preserves the PRD-0053 direct-sum
// invariant when assignA == assignB == true on every channel.
//------------------------------------------------------------------------------
enum class CrossfaderCurve : int
{
    Smooth = 0,   ///< Equal-power: gainA = cos(p·π/2), gainB = sin(p·π/2).
    Sharp  = 1,   ///< Hard-cut piecewise linear, half-width 0.02 around p=0.5.
    Linear = 2    ///< Internal/test: gainA = 1-p, gainB = p (constant-sum).
};

struct CrossfaderSnapshot
{
    float           crossfader = 0.5f;                       ///< [0, 1]; 0=A, 1=B, 0.5=centred
    CrossfaderCurve curve      = CrossfaderCurve::Smooth;    ///< PRD-0057 §1.5.5
};
