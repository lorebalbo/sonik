#pragma once
//==============================================================================
// PRD-0053: ChannelStripSnapshot — per-block, per-channel parameter snapshot.
//
// Populated once per block, per channel by reading from the ValueTree-backed
// std::atomic values in MixerAtomicSnapshot (PRD-0052). The snapshot is a
// stack-local POD struct; it is NOT heap-allocated and involves no atomic
// stores.
//
// All values are already in their audio-thread native form (linear amplitudes,
// bipolar filter, etc.) exactly as stored in MixerAtomicSnapshot.
//==============================================================================

struct ChannelStripSnapshot
{
    float gain     = 1.0f;   ///< linear amplitude (PRD-0052 atomic, converted from dB)
    float eqHigh   = 1.0f;   ///< linear amplitude
    float eqMid    = 1.0f;   ///< linear amplitude
    float eqLow    = 1.0f;   ///< linear amplitude
    float filter   = 0.0f;   ///< bipolar [-1, +1]
    float fader    = 1.0f;   ///< [0, 1]
    bool  killHigh = false;
    bool  killMid  = false;
    bool  killLow  = false;
    bool  assignA  = false;
    bool  assignB  = false;
};
