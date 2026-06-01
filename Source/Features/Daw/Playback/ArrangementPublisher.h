#pragma once
//==============================================================================
// PRD-0079: ArrangementPublisher — SeqLock double-buffer publication of
// ArrangementSnapshot from the message thread to the audio thread.
//
// Pattern: double-buffer + atomic activeIndex under an odd/even sequence guard.
//   publish() copies into the INACTIVE buffer, then flips activeIndex inside
//   the odd window (minimising the spin window for readers).
//   read()    copies from the ACTIVE buffer; retries if sequence is odd or
//             changed (write raced the read).
//
// AUDIO THREAD CONTRACT (AGENTS.md §"The Audio Thread"):
//   read()   → no allocation, no lock, no I/O. Pure atomic ops + memcpy.
//   publish() → message thread only; no real-time constraint.
//
// Both snapshot buffers are non-heap members (value members of the publisher).
// The publisher is constructed once and injected by reference; no singleton.
//==============================================================================

#include <atomic>
#include <cstring>

#include "ArrangementSnapshot.h"

namespace Daw
{

class ArrangementPublisher
{
public:
    ArrangementPublisher()
    {
        // Both buffers start as empty snapshots; sequence_ is even (stable).
        buffers_[0] = ArrangementSnapshot{};
        buffers_[1] = ArrangementSnapshot{};
        activeIndex_.store (0, std::memory_order_relaxed);
        sequence_.store (0, std::memory_order_relaxed);
    }

    ~ArrangementPublisher() = default;

    ArrangementPublisher (const ArrangementPublisher&)            = delete;
    ArrangementPublisher& operator= (const ArrangementPublisher&) = delete;

    //==========================================================================
    // Message-thread API
    //==========================================================================

    /// Publish a new snapshot.  Called exclusively on the message thread.
    ///
    /// Steps:
    ///   1. Choose the inactive buffer (the one NOT currently pointed to by activeIndex_).
    ///   2. Copy the new snapshot into the inactive buffer (all copy work is done OUTSIDE
    ///      the odd window — the reader's potential spin is as short as a single atomic store).
    ///   3. Increment sequence_ to ODD → signal write-in-progress.
    ///   4. Flip activeIndex_ to the just-written buffer.
    ///   5. Increment sequence_ to EVEN → signal write-complete.
    void publish (const ArrangementSnapshot& snapshot)
    {
        // 1. Determine which buffer is inactive.
        const uint32_t currentActive = activeIndex_.load (std::memory_order_relaxed);
        const uint32_t inactiveIdx   = 1u - currentActive;

        // 2. Copy into the inactive buffer BEFORE entering the odd window.
        buffers_[inactiveIdx] = snapshot;

        // 3. Enter odd window — signals write-in-progress to readers.
        std::atomic_thread_fence (std::memory_order_release);
        sequence_.fetch_add (1u, std::memory_order_relaxed);

        // 4. Flip activeIndex_ to the freshly-written buffer.
        activeIndex_.store (inactiveIdx, std::memory_order_relaxed);

        // 5. Leave odd window — signals write-complete.
        std::atomic_thread_fence (std::memory_order_release);
        sequence_.fetch_add (1u, std::memory_order_relaxed);
    }

    //==========================================================================
    // Audio-thread API
    //==========================================================================

    /// Read the current snapshot.  Real-time safe: no allocation, no lock, no I/O.
    ///
    /// Retries if:
    ///   - seq1 is odd  (writer is mid-publish)
    ///   - seq1 != seq2 (writer completed a new publish during our copy)
    ///
    /// Under no concurrent write this loop executes exactly once.
    void read (ArrangementSnapshot& out) const
    {
        while (true)
        {
            // 1. Sample sequence; retry if odd.
            const uint32_t seq1 = sequence_.load (std::memory_order_relaxed);
            if (seq1 & 1u)
                continue;

            // 2. Acquire fence: orders field reads below after seq1 load.
            std::atomic_thread_fence (std::memory_order_acquire);

            // 3. Copy the active buffer.
            const uint32_t idx = activeIndex_.load (std::memory_order_relaxed);
            out = buffers_[idx];

            // 4. Acquire fence: orders seq2 load after the copy.
            std::atomic_thread_fence (std::memory_order_acquire);

            // 5. Verify sequence is unchanged; if not, retry.
            const uint32_t seq2 = sequence_.load (std::memory_order_relaxed);
            if (seq1 == seq2)
                break;
        }
    }

    //==========================================================================
    // Test / diagnostic helpers (message thread)
    //==========================================================================

    /// Returns the sequence counter.  Used only in unit tests.
    uint32_t sequenceForTest() const noexcept
    {
        return sequence_.load (std::memory_order_relaxed);
    }

private:
    // Even = stable, odd = write-in-progress.
    std::atomic<uint32_t> sequence_    { 0 };

    // Index of the currently-live buffer (0 or 1).
    std::atomic<uint32_t> activeIndex_ { 0 };

    // Two pre-allocated, non-heap snapshot buffers.
    ArrangementSnapshot buffers_[2];
};

} // namespace Daw
