#include "MidiMessageBridge.h"

namespace sonik::midi
{
    MidiMessageBridge::MidiMessageBridge()
    {
        // Diagnostics tick: 1 Hz, Message-thread only (juce::Timer guarantees it).
        // DBG() is permitted here per PRD-0041 §1.3.5.
        startTimer (1000);
    }

    MidiMessageBridge::~MidiMessageBridge()
    {
        stopTimer();
        // Storage is freed by the std::array destructor. Any events remaining
        // in the FIFO are discarded; PRD-0040's producers have already stopped
        // by the time this destructor runs (see SonikApplication::shutdown).
    }

    //--------------------------------------------------------------------------
    BridgeWriteResult MidiMessageBridge::dispatch (MidiTargetCategory category,
                                                   std::uint8_t       deckIndex,
                                                   float              normalisedValue,
                                                   std::int16_t       intDelta,
                                                   std::uint64_t      deviceId,
                                                   SoftTakeoverPolicy softTakeover,
                                                   TargetIndex        targetIndex) noexcept
    {
        // Bounds-check the enum: a producer using a category value outside the
        // table (e.g. via a buggy cast) drops silently rather than indexing
        // out of bounds. We treat the table lookup as the source of truth.
        const auto idx = static_cast<std::size_t> (category);
        if (idx >= MidiTargetCategoryCount)
        {
            droppedFullCount.fetch_add (1, std::memory_order_relaxed);
            return BridgeWriteResult::DroppedFull;
        }

        const RoutingClass cls = routingTable[idx];

        if (cls == RoutingClass::AudioThread)
        {
            // Lock-free SPSC write. Note: PRD-0040 guarantees at most one
            // active MIDI callback thread at a time (JUCE serialises callbacks
            // per device, and our bridge has a single dispatcher), so single-
            // producer is the actual contract.
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            audioFifo.prepareToWrite (1, start1, size1, start2, size2);

            if (size1 + size2 < 1)
            {
                droppedFullCount.fetch_add (1, std::memory_order_relaxed);
                return BridgeWriteResult::DroppedFull;
            }

            // Exactly one slot reserved; pick whichever block holds it.
            const int slot = (size1 > 0 ? start1 : start2);
            audioFifoStorage[static_cast<std::size_t> (slot)] = MidiAudioEvent {
                category,
                deckIndex,
                normalisedValue,
                intDelta,
                0 // sampleTimestamp filled later if/when PRD-0044 has frame info
            };
            audioFifo.finishedWrite (1);
            return BridgeWriteResult::Ok;
        }

        // Message-thread routing. callAsync allocates internally; that is
        // explicitly permitted on the MIDI callback thread by PRD-0041 §1.3.2
        // step 6. The lambda captures POD by value, so no shared state escapes.
        auto* sink = messageThreadSink.load (std::memory_order_acquire);
        if (sink == nullptr)
        {
            // No sink wired yet — silently drop. PRD-0044 will register one;
            // this case only occurs during startup before that wiring.
            return BridgeWriteResult::Ok;
        }

        const MidiMessageEvent event { category, deckIndex, normalisedValue, intDelta, deviceId, softTakeover, targetIndex };
        juce::MessageManager::callAsync ([sink, event]
        {
            sink->onMidiMessageThreadEvent (event);
        });

        return BridgeWriteResult::Ok;
    }

    //--------------------------------------------------------------------------
    int MidiMessageBridge::drainAudioThreadFifo (AudioMidiEventHandler& handler) noexcept
    {
        const int ready = audioFifo.getNumReady();
        if (ready <= 0)
            return 0;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        audioFifo.prepareToRead (ready, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            handler.applyAudioMidiEvent (audioFifoStorage[static_cast<std::size_t> (start1 + i)]);

        for (int i = 0; i < size2; ++i)
            handler.applyAudioMidiEvent (audioFifoStorage[static_cast<std::size_t> (start2 + i)]);

        const int total = size1 + size2;
        audioFifo.finishedRead (total);
        return total;
    }

    //--------------------------------------------------------------------------
    void MidiMessageBridge::setMessageThreadSink (MessageThreadSink* sink)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        messageThreadSink.store (sink, std::memory_order_release);
    }

    //--------------------------------------------------------------------------
    void MidiMessageBridge::timerCallback()
    {
        // Message-thread only (juce::Timer contract). Logging is permitted.
        const auto current = droppedFullCount.load (std::memory_order_relaxed);
        if (current != lastReportedDropCount)
        {
            const auto delta = current - lastReportedDropCount;
            DBG ("[MIDI bridge] FIFO drops since last tick: " << (juce::int64) delta
                 << "  (total=" << (juce::int64) current << ")");
            lastReportedDropCount = current;
        }
    }
} // namespace sonik::midi
