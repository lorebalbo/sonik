#pragma once
//==============================================================================
// PRD-0053: MasterSnapshot — per-block master parameter snapshot.
//
// Populated once per block from MixerAtomicSnapshot::masterGain.
//==============================================================================

struct MasterSnapshot
{
    float masterGain = 1.0f;   ///< linear amplitude (converted from dB)
};
