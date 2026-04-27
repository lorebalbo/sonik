#include "MasterClockPublisher.h"

void MasterClockPublisher::publish (const MasterClockSnapshot& snapshot)
{
    // Step 1: increment to odd — signals write-in-progress to readers.
    sequence_.fetch_add (1u, std::memory_order_relaxed);

    // Step 2: release fence — all subsequent stores are ordered after the sequence increment.
    std::atomic_thread_fence (std::memory_order_release);

    // Step 3: write fields one by one (plain stores, guarded by the fences).
    buffer_.masterBPM               = snapshot.masterBPM;
    buffer_.masterPhaseOriginSample  = snapshot.masterPhaseOriginSample;
    buffer_.masterIsPlaying          = snapshot.masterIsPlaying;

    // Step 4: release fence — ensures all field writes are visible before the next sequence store.
    std::atomic_thread_fence (std::memory_order_release);

    // Step 5: increment to even — signals write-complete; sequence_ is now stable.
    sequence_.fetch_add (1u, std::memory_order_relaxed);
}

MasterClockSnapshot MasterClockPublisher::read() const
{
    MasterClockSnapshot snapshot;

    while (true)
    {
        // Step 1: read the sequence counter; retry if odd (write in progress).
        const uint32_t seq1 = sequence_.load (std::memory_order_relaxed);
        if (seq1 & 1u)
            continue;

        // Step 2: acquire fence — ensures field reads below are ordered after seq1 load.
        std::atomic_thread_fence (std::memory_order_acquire);

        // Step 3: copy fields.
        snapshot.masterBPM               = buffer_.masterBPM;
        snapshot.masterPhaseOriginSample  = buffer_.masterPhaseOriginSample;
        snapshot.masterIsPlaying          = buffer_.masterIsPlaying;

        // Step 4: acquire fence — ensures the final sequence load is ordered after the field reads.
        std::atomic_thread_fence (std::memory_order_acquire);

        // Step 5: verify sequence is unchanged; if not, a write raced us — retry.
        const uint32_t seq2 = sequence_.load (std::memory_order_relaxed);
        if (seq1 == seq2)
            break;
    }

    return snapshot;
}
