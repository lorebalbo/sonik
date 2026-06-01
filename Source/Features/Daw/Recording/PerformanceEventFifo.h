#pragma once
//==============================================================================
// PRD-0072: Performance-Event Bridge.
//
// A single, canonical, real-time-safe transport that carries deck/mixer
// *structural* performance events from any producer thread (audio or message)
// to the RecordingSessionController (PRD-0071) drain on the message thread, as
// one ordered, lossless stream.
//
// Mirrors PRD-0041's MidiMessageBridge FIFO pattern verbatim: a pre-allocated
// std::array<PerformanceEvent, N> backed by a juce::AbstractFifo(N) index,
// reserved/written with prepareToWrite + finishedWrite on the producer and
// drained with prepareToRead + finishedRead on the consumer. The whole path is
// noexcept, allocation-free, lock-free, and I/O-free.
//
// SCOPE: transport only. This file does NOT create, grow, align, or finalise
// clips and does not mutate the `daw` ValueTree. Interpretation is PRD-0073 /
// 0075 / 0076 / 0077.
//==============================================================================

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace Daw
{

//==============================================================================
// Closed taxonomy of structural events EPIC-0009 captures. Continuous /
// boolean automation is explicitly excluded (EPIC-0011).
enum class PerformanceEventType : std::uint8_t
{
    DeckPlay = 0,
    DeckStop,
    ChannelMute,
    ChannelUnmute,
    CueJumpIn,
    CueJumpOut,
    BeatJump,
    LoopIn,
    LoopOut,
    SourceModeChange,
    HotCueJump,         // PRD-0075: single jump event; out=sourceSamplePosition, in=payload
    LoopPass            // PRD-0076: one completed loop pass; out=loopOut (sourceSamplePosition), in=loopIn (payload)
};

//==============================================================================
// Flat POD event. No pointers, no strings, no variable-length payloads.
//   type                 : structural event kind.
//   deckIndex            : originating deck (0..3, room to grow).
//   sourceSamplePosition : deck source position captured AT the event instant
//                          (frozen by the producer; never re-read at drain).
//   timestamp            : monotonic global ordering counter assigned at enqueue.
//   payload              : optional fixed-width scalar interpreted per `type`
//                          by the consumer (e.g. beat-jump beat count,
//                          source-mode enum value). 0 when unused.
struct PerformanceEvent
{
    PerformanceEventType type                 = PerformanceEventType::DeckPlay;
    std::uint8_t         deckIndex            = 0;
    std::int64_t         sourceSamplePosition = 0;
    std::int64_t         timestamp            = 0;
    std::int64_t         payload              = 0;
};

static_assert (std::is_trivially_copyable_v<PerformanceEvent>,
               "PerformanceEvent must be trivially copyable for lock-free FIFO use");

//==============================================================================
// Consumer interface. PRD-0071's controller (and PRD-0073 onward) implement it.
struct PerformanceEventHandler
{
    virtual ~PerformanceEventHandler() = default;
    virtual void onPerformanceEvent (const PerformanceEvent&) = 0;
};

//==============================================================================
class PerformanceEventFifo
{
public:
    // Generous capacity per PRD-0072 §1.5.1: structural events arrive at human
    // cadence (low tens/s across all decks), orders of magnitude below the
    // drain rate, so a genuine overflow signals a defect rather than load.
    static constexpr int Capacity = 1024;
    // juce::AbstractFifo keeps one slot unused to distinguish full from empty,
    // so the physical storage must be one larger than the logical capacity.
    static constexpr int StorageSize = Capacity + 1;

    PerformanceEventFifo() = default;
    ~PerformanceEventFifo() = default;

    PerformanceEventFifo (const PerformanceEventFifo&)            = delete;
    PerformanceEventFifo& operator= (const PerformanceEventFifo&) = delete;

    //--------------------------------------------------------------------------
    // Producer (any thread). Reserves one slot, stamps a monotonic timestamp,
    // writes the POD, and publishes it. On a full FIFO it applies the
    // drop-newest policy and increments the overflow counter (relaxed) without
    // blocking, allocating, locking, or logging. Returns true if enqueued.
    bool enqueue (const PerformanceEvent& eventIn) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 < 1)
        {
            // Drop-newest: the queued prefix stays an intact ordered stream.
            overflowCount_.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const int slot = (size1 > 0 ? start1 : start2);

        PerformanceEvent stored = eventIn;
        stored.timestamp = nextTimestamp_.fetch_add (1, std::memory_order_relaxed);

        storage_[static_cast<std::size_t> (slot)] = stored;
        fifo_.finishedWrite (1);
        return true;
    }

    //--------------------------------------------------------------------------
    // Consumer (message thread only). Drains every ready event in FIFO order,
    // including the two-block wrap-around boundary, calling
    // handler.onPerformanceEvent synchronously for each. Returns the count
    // delivered. Performs no allocation and takes no lock.
    int drain (PerformanceEventHandler& handler) noexcept
    {
        const int ready = fifo_.getNumReady();
        if (ready <= 0)
            return 0;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToRead (ready, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            handler.onPerformanceEvent (storage_[static_cast<std::size_t> (start1 + i)]);

        for (int i = 0; i < size2; ++i)
            handler.onPerformanceEvent (storage_[static_cast<std::size_t> (start2 + i)]);

        fifo_.finishedRead (size1 + size2);
        return size1 + size2;
    }

    //--------------------------------------------------------------------------
    // Diagnostics (any thread). Non-zero outside a stress test is a bug to
    // investigate (PRD-0072 §1.3.6): structural events must not be lost.
    std::uint64_t getOverflowCount() const noexcept
    {
        return overflowCount_.load (std::memory_order_relaxed);
    }

    // Number of events currently waiting to be drained (consumer-side view).
    int getNumReady() const noexcept { return fifo_.getNumReady(); }

private:
    std::array<PerformanceEvent, StorageSize> storage_ {};
    juce::AbstractFifo                        fifo_ { StorageSize };

    std::atomic<std::uint64_t> overflowCount_ { 0 };
    std::atomic<std::int64_t>  nextTimestamp_ { 0 };
};

} // namespace Daw
