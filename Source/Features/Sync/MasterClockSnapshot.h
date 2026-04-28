#pragma once

#include <cstdint>

/// Plain POD snapshot of the master clock state.
/// Published by MasterClockPublisher via SeqLock; consumed lock-free by the audio thread.
struct MasterClockSnapshot
{
    // Effective playback BPM (beatgrid BPM * speed multiplier) used by SyncEngine.
    double  masterBPM               = 0.0;
    // Native beat-grid BPM in source-sample domain used by PhaseLockEngine.
    double  masterNativeBPM         = 0.0;
    int64_t masterPhaseOriginSample = 0;
    bool    masterIsPlaying         = false;
};
