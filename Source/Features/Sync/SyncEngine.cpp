#include "SyncEngine.h"
#include "MasterClockPublisher.h"

void SyncEngine::process (DeckAudioState& state) noexcept
{
    // 1. Guard: not synced → nothing to do
    if (! state.isSynced.load (std::memory_order_relaxed))
        return;

    // 2. Read clock snapshot via SeqLock (lock-free, no allocation, no I/O)
    const auto snapshot = publisher_.read();

    // 3a. Guard: master not playing → hold last computed speed
    if (! snapshot.masterIsPlaying)
        return;

    // 3b. Guard: master BPM invalid → avoid division by zero
    if (snapshot.masterBPM <= 0.0)
        return;

    // 3c. Guard: deck BPM not yet known → leave speedMultiplier unchanged
    const double deckBPM = state.deckBPM.load (std::memory_order_relaxed);
    if (deckBPM <= 0.0)
        return;

    // 4. Compute sync ratio
    double ratio = snapshot.masterBPM / deckBPM;

    // 5. 2:1 normalisation: fold ratio into [0.667, 1.5] to prevent half/double-time
    while (ratio > 1.5)   ratio *= 0.5;
    while (ratio < 0.667) ratio *= 2.0;

    // 6. Publish to audio thread state (float cast; precision sufficient for tempo)
    state.speedMultiplier.store (static_cast<float> (ratio), std::memory_order_relaxed);
}
