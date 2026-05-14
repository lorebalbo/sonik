#pragma once
//==============================================================================
// PRD-0041: RT-Safe MIDI Message Bridge.
//
// Single canonical bridge between PRD-0040 (MIDI callback thread producers)
// and the two downstream threads:
//   - Audio thread (low-latency jog/scratch via lock-free FIFO)
//   - Message thread (UI/transport via juce::MessageManager::callAsync)
//
// THREAD CONTRACT
// ---------------
// dispatch(...)                : MIDI callback thread (producer).
//                                noexcept, allocation-free on the AudioThread
//                                path, never blocks.
// drainAudioThreadFifo(handler): Audio thread (consumer).
//                                noexcept, allocation-free, lock-free.
// setMessageThreadSink(...)    : Message thread only (JUCE_ASSERT_MESSAGE_THREAD).
// getDroppedFullCount()        : Any thread (relaxed atomic load).
//
// CAPACITY
// --------
// FIFO is 1024 events. Derivation: 80 jog ticks/s × 8 simultaneous decks
// × 0.5 s scratch burst = 320 events; 3× safety margin → 1024.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "MidiAudioEvent.h"
#include "MidiMessageEvent.h"
#include "MidiTargetCategory.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sonik::midi
{
    //--------------------------------------------------------------------------
    // Result of `dispatch`. Drop is observable via `getDroppedFullCount()`.
    enum class BridgeWriteResult : std::uint8_t
    {
        Ok,
        DroppedFull,
    };

    //--------------------------------------------------------------------------
    // Audio-thread consumer interface.
    struct AudioMidiEventHandler
    {
        virtual ~AudioMidiEventHandler() = default;
        virtual void applyAudioMidiEvent (const MidiAudioEvent&) noexcept = 0;
    };

    //--------------------------------------------------------------------------
    // Message-thread consumer interface (PRD-0044 implements this).
    struct MessageThreadSink
    {
        virtual ~MessageThreadSink() = default;
        virtual void onMidiMessageThreadEvent (const MidiMessageEvent&) = 0;
    };

    //--------------------------------------------------------------------------
    // Compile-time routing table. Index by `static_cast<size_t>(category)`.
    //
    // RULES:
    //   * Jog* targets → AudioThread (must reach processBlock within one buffer).
    //   * Everything else → MessageThread (UI/transport, deferred is fine).
    //
    // Adding a new MidiTargetCategory without an entry here triggers the
    // static_assert below — a fail-fast guardrail.
    inline constexpr std::array<RoutingClass, MidiTargetCategoryCount> routingTable = {
        /* JogScratch          */ RoutingClass::AudioThread,
        /* JogBend             */ RoutingClass::AudioThread,
        /* JogTouch            */ RoutingClass::AudioThread,
        /* TransportPlay       */ RoutingClass::MessageThread,
        /* TransportCue        */ RoutingClass::MessageThread,
        /* TransportSync       */ RoutingClass::MessageThread,
        /* PitchFader          */ RoutingClass::MessageThread,
        /* Gain                */ RoutingClass::MessageThread,
        /* EqHigh              */ RoutingClass::MessageThread,
        /* EqMid               */ RoutingClass::MessageThread,
        /* EqLow               */ RoutingClass::MessageThread,
        /* Crossfader          */ RoutingClass::MessageThread,
        /* LoopIn              */ RoutingClass::MessageThread,
        /* LoopOut             */ RoutingClass::MessageThread,
        /* LoopSizeHalve       */ RoutingClass::MessageThread,
        /* LoopSizeDouble      */ RoutingClass::MessageThread,
        /* LoopToggle          */ RoutingClass::MessageThread,
        /* HotCueTrigger       */ RoutingClass::MessageThread,
        /* BeatJumpMinus       */ RoutingClass::MessageThread,
        /* BeatJumpPlus        */ RoutingClass::MessageThread,
        /* LibraryScrollUp     */ RoutingClass::MessageThread,
        /* LibraryScrollDown   */ RoutingClass::MessageThread,
        /* LibraryLoadDeck     */ RoutingClass::MessageThread,
        /* LibraryFocusSearch  */ RoutingClass::MessageThread,
    };
    static_assert (routingTable.size() == MidiTargetCategoryCount,
                   "routingTable must cover every MidiTargetCategory; add a row when extending the enum");

    //--------------------------------------------------------------------------
    class MidiMessageBridge final : private juce::Timer
    {
    public:
        static constexpr int FifoCapacity = 1024;
        // juce::AbstractFifo is a ring buffer that keeps one slot unused to
        // distinguish full from empty, so the physical storage must be one
        // larger than the logical capacity to actually hold FifoCapacity items.
        static constexpr int FifoStorageSize = FifoCapacity + 1;

        MidiMessageBridge();
        ~MidiMessageBridge() override;

        MidiMessageBridge (const MidiMessageBridge&)            = delete;
        MidiMessageBridge& operator= (const MidiMessageBridge&) = delete;

        // ---- Producer (MIDI callback thread) --------------------------------
        BridgeWriteResult dispatch (MidiTargetCategory category,
                                    std::uint8_t       deckIndex,
                                    float              normalisedValue,
                                    std::int16_t       intDelta,
                                    std::uint64_t      deviceId) noexcept;

        // ---- Consumer (Audio thread) ---------------------------------------
        // Drains every ready event in FIFO order, including the wrap-around
        // boundary. Returns the number of events dispatched to `handler`.
        int drainAudioThreadFifo (AudioMidiEventHandler& handler) noexcept;

        // ---- Wiring (Message thread) ---------------------------------------
        void setMessageThreadSink (MessageThreadSink* sink);

        // ---- Diagnostics (any thread) --------------------------------------
        std::uint64_t getDroppedFullCount() const noexcept
        {
            return droppedFullCount.load (std::memory_order_relaxed);
        }

    private:
        void timerCallback() override;

        // Pre-allocated FIFO storage. Capacity is fixed at construction.
        std::array<MidiAudioEvent, FifoStorageSize> audioFifoStorage {};
        juce::AbstractFifo                          audioFifo { FifoStorageSize };

        // Atomic so the MIDI callback thread can publish a sink set by the
        // Message thread without locks. Read with acquire on the producer,
        // written with release on the Message thread.
        std::atomic<MessageThreadSink*>          messageThreadSink { nullptr };

        std::atomic<std::uint64_t>               droppedFullCount { 0 };
        std::uint64_t                            lastReportedDropCount { 0 };
    };
} // namespace sonik::midi
