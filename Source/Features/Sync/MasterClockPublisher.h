#pragma once

#include <atomic>
#include "MasterClockSnapshot.h"

/// Lock-free SeqLock publisher for MasterClockSnapshot.
///
/// Write path (message thread):
///   sequence_ is incremented to ODD before writing buffer_ fields (signals write-in-progress),
///   then incremented to EVEN once done (signals stable).
///
/// Read path (audio thread):
///   Spins while sequence_ is odd, copies fields, then verifies sequence_ didn't change.
///   No memory allocation, no locks, no I/O — safe to call from processBlock.
///
/// Only one writer is assumed (MasterClockManager, on the message thread).
class MasterClockPublisher
{
public:
    MasterClockPublisher()  = default;
    ~MasterClockPublisher() = default;

    MasterClockPublisher (const MasterClockPublisher&)            = delete;
    MasterClockPublisher& operator= (const MasterClockPublisher&) = delete;

    /// Publish a new snapshot. Called exclusively on the message thread.
    void publish (const MasterClockSnapshot& snapshot);

    /// Read the current snapshot. Safe to call from the audio thread.
    /// Retries if a write is in progress or completed mid-read.
    MasterClockSnapshot read() const;

private:
    // Even = stable, odd = write-in-progress.
    std::atomic<uint32_t> sequence_ { 0 };

    // Unprotected buffer written field-by-field; correctness guaranteed by the sequence counter.
    MasterClockSnapshot buffer_ {};

public:
    // ---------------------------------------------------------------------------
    // Audio-thread helpers (separate from the SeqLock — written by audio thread)
    // ---------------------------------------------------------------------------

    /// Index (0–3) of the deck slot that is currently the master.
    /// Written by MasterClockManager (message thread) whenever the master changes.
    /// Read by AudioEngine (audio thread) to locate the master deck's playhead.
    std::atomic<int> masterSlotIndex { -1 };

    /// Current playhead position of the master deck (samples from start of its file).
    /// Written by AudioEngine (audio thread) once per callback, before PhaseLockEngine runs.
    /// Read by PhaseLockEngine to compute accurate inter-track phase.
    std::atomic<int64_t> masterPlayheadSample { 0 };
};
