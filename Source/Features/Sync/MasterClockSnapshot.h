#pragma once

#include <cstdint>

/// Plain POD snapshot of the master clock state.
/// Published by MasterClockPublisher via SeqLock; consumed lock-free by the audio thread.
struct MasterClockSnapshot
{
    double  masterBPM              = 0.0;
    int64_t masterPhaseOriginSample = 0;
    bool    masterIsPlaying        = false;
};
