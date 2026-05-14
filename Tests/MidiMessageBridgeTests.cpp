// Tests for PRD-0041: RT-Safe MIDI Message Bridge.
//
// Verified behaviours (mapped to PRD §1.4 acceptance criteria):
//   (a) AudioThread routing pushes to the FIFO and drainAudioThreadFifo
//       delivers events in order.
//   (b) MessageThread routing schedules a callAsync to the sink.
//   (c) Full FIFO returns DroppedFull and increments the counter.
//   (d) Drain handles wrap-around (two non-contiguous blocks) correctly.
//   (e) static_asserts in headers compile (this file's mere inclusion proves
//       it — verified by compilation, no runtime test needed).
//   (f) Stress test: 100,000 events on a producer thread; counter accounts
//       for every event (delivered + dropped == total injected).

#include "Features/Midi/MidiMessageBridge.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <thread>
#include <vector>

namespace
{
    using namespace sonik::midi;

    struct RecordingAudioHandler final : public AudioMidiEventHandler
    {
        std::vector<MidiAudioEvent> events;
        void applyAudioMidiEvent (const MidiAudioEvent& e) noexcept override
        {
            // Note: in production this is on the audio thread and must not
            // allocate. In tests we run on the test thread and a vector
            // push_back is acceptable; we are validating ordering/semantics.
            events.push_back (e);
        }
    };

    struct RecordingMessageSink final : public MessageThreadSink
    {
        std::vector<MidiMessageEvent> events;
        void onMidiMessageThreadEvent (const MidiMessageEvent& e) override
        {
            events.push_back (e);
        }
    };

    template <typename Pred>
    bool pumpUntil (Pred predicate, int timeoutMs = 1500)
    {
        auto* mm = juce::MessageManager::getInstance();
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (predicate())
                return true;
            mm->runDispatchLoopUntil (20);
        }
        return predicate();
    }
} // namespace

class MidiMessageBridgeTests final : public juce::UnitTest
{
public:
    MidiMessageBridgeTests() : juce::UnitTest ("MIDI Message Bridge (PRD-0041)", "Sonik") {}

    void runTest() override
    {
        testRoutingTableCoverage();
        testAudioThreadRouting();
        testAudioThreadDrainOrder();
        testMessageThreadRouting();
        testDropOnFull();
        testWrapAroundDrain();
        testMixedTrafficInterleaved();
        testStressInjection();
        testNullSinkDoesNotCrash();
        testPodInvariants();
    }

private:
    //----------------------------------------------------------------------
    void testRoutingTableCoverage()
    {
        beginTest ("Routing table covers every category and classifies Jog* as AudioThread");
        for (std::size_t i = 0; i < MidiTargetCategoryCount; ++i)
        {
            const auto cls = routingTable[i];
            expect (cls == RoutingClass::AudioThread || cls == RoutingClass::MessageThread,
                    "every entry must be a valid RoutingClass");
        }
        expect (routingTable[(std::size_t) MidiTargetCategory::JogScratch] == RoutingClass::AudioThread);
        expect (routingTable[(std::size_t) MidiTargetCategory::JogBend]    == RoutingClass::AudioThread);
        expect (routingTable[(std::size_t) MidiTargetCategory::JogTouch]   == RoutingClass::AudioThread);
        expect (routingTable[(std::size_t) MidiTargetCategory::PitchFader] == RoutingClass::MessageThread);
        expect (routingTable[(std::size_t) MidiTargetCategory::HotCueTrigger] == RoutingClass::MessageThread);
    }

    //----------------------------------------------------------------------
    void testAudioThreadRouting()
    {
        beginTest ("AudioThread routing writes to the FIFO and dispatch returns Ok");
        MidiMessageBridge bridge;
        const auto r = bridge.dispatch (MidiTargetCategory::JogScratch, 0, 0.5f, 7, 0xabcdULL);
        expect (r == BridgeWriteResult::Ok);

        RecordingAudioHandler h;
        const int drained = bridge.drainAudioThreadFifo (h);
        expectEquals (drained, 1);
        expectEquals ((int) h.events.size(), 1);
        expect (h.events.front().category == MidiTargetCategory::JogScratch);
        expectWithinAbsoluteError (h.events.front().normalisedValue, 0.5f, 1.0e-6f);
        expectEquals ((int) h.events.front().intDelta, 7);
        expectEquals ((int) h.events.front().deckIndex, 0);
    }

    //----------------------------------------------------------------------
    void testAudioThreadDrainOrder()
    {
        beginTest ("Drain delivers AudioThread events in FIFO order");
        MidiMessageBridge bridge;
        for (int i = 0; i < 64; ++i)
            expect (bridge.dispatch (MidiTargetCategory::JogBend, (std::uint8_t) (i % 4),
                                     (float) i, (std::int16_t) i, 0)
                    == BridgeWriteResult::Ok);

        RecordingAudioHandler h;
        const int drained = bridge.drainAudioThreadFifo (h);
        expectEquals (drained, 64);
        for (int i = 0; i < 64; ++i)
            expectEquals ((int) h.events[(std::size_t) i].intDelta, i,
                          "events drain in order");
    }

    //----------------------------------------------------------------------
    void testMessageThreadRouting()
    {
        beginTest ("MessageThread routing reaches the registered sink via callAsync");
        MidiMessageBridge bridge;
        RecordingMessageSink sink;
        bridge.setMessageThreadSink (&sink);

        expect (bridge.dispatch (MidiTargetCategory::TransportPlay, 1, 1.0f, 0, 0xdeadbeefULL)
                == BridgeWriteResult::Ok);
        expect (bridge.dispatch (MidiTargetCategory::HotCueTrigger, 2, 0.0f, 0, 0xdeadbeefULL)
                == BridgeWriteResult::Ok);

        expect (pumpUntil ([&] { return sink.events.size() >= 2; }, 1000),
                "callAsync delivered both events on the message thread");
        expectEquals ((int) sink.events.size(), 2);
        expect (sink.events[0].category == MidiTargetCategory::TransportPlay);
        expectEquals ((int) sink.events[0].deckIndex, 1);
        expect (sink.events[1].category == MidiTargetCategory::HotCueTrigger);
        expectEquals ((int) sink.events[1].deckIndex, 2);

        // Tear down before sink goes out of scope.
        bridge.setMessageThreadSink (nullptr);
    }

    //----------------------------------------------------------------------
    void testDropOnFull()
    {
        beginTest ("FIFO full returns DroppedFull and counter increments");
        MidiMessageBridge bridge;

        // Fill the FIFO (1024 slots).
        for (int i = 0; i < MidiMessageBridge::FifoCapacity; ++i)
            expect (bridge.dispatch (MidiTargetCategory::JogScratch, 0, 0.0f, 0, 0)
                    == BridgeWriteResult::Ok);

        // 1025th must drop.
        const auto r = bridge.dispatch (MidiTargetCategory::JogScratch, 0, 0.0f, 0, 0);
        expect (r == BridgeWriteResult::DroppedFull);
        expectEquals ((juce::int64) bridge.getDroppedFullCount(), (juce::int64) 1);

        // Several more drops accumulate.
        for (int i = 0; i < 16; ++i)
            bridge.dispatch (MidiTargetCategory::JogScratch, 0, 0.0f, 0, 0);
        expectEquals ((juce::int64) bridge.getDroppedFullCount(), (juce::int64) 17);

        // Draining frees slots; subsequent writes succeed.
        RecordingAudioHandler h;
        const int drained = bridge.drainAudioThreadFifo (h);
        expectEquals (drained, MidiMessageBridge::FifoCapacity);
        expect (bridge.dispatch (MidiTargetCategory::JogScratch, 0, 0.0f, 0, 0)
                == BridgeWriteResult::Ok);
    }

    //----------------------------------------------------------------------
    void testWrapAroundDrain()
    {
        beginTest ("Drain handles wrap-around: two non-contiguous blocks processed in order");
        MidiMessageBridge bridge;

        // Step 1: push ~700, drain ~700. This advances the FIFO's read/write
        // indices so the next write spans the buffer end.
        for (int i = 0; i < 700; ++i)
            bridge.dispatch (MidiTargetCategory::JogBend, 0, 0.0f, (std::int16_t) i, 0);
        RecordingAudioHandler warmup;
        bridge.drainAudioThreadFifo (warmup);
        expectEquals ((int) warmup.events.size(), 700);

        // Step 2: push another 800 events. With FIFO capacity 1024 and write
        // index at 700, this spans the wrap boundary at slot 1024 -> slot 0.
        for (int i = 0; i < 800; ++i)
            bridge.dispatch (MidiTargetCategory::JogBend, 0, 0.0f, (std::int16_t) (1000 + i), 0);

        // Step 3: drain. juce::AbstractFifo::prepareToRead must return two
        // non-zero blocks here. Our drain must process them in order.
        RecordingAudioHandler h;
        const int drained = bridge.drainAudioThreadFifo (h);
        expectEquals (drained, 800);
        for (int i = 0; i < 800; ++i)
            expectEquals ((int) h.events[(std::size_t) i].intDelta, 1000 + i,
                          "wrap-around preserves FIFO order");
    }

    //----------------------------------------------------------------------
    void testMixedTrafficInterleaved()
    {
        beginTest ("Mixed AudioThread + MessageThread traffic routes independently");
        MidiMessageBridge bridge;
        RecordingMessageSink sink;
        bridge.setMessageThreadSink (&sink);

        bridge.dispatch (MidiTargetCategory::JogScratch,   0, 0.1f, 1, 0);
        bridge.dispatch (MidiTargetCategory::TransportPlay, 0, 1.0f, 0, 0);
        bridge.dispatch (MidiTargetCategory::JogBend,      0, 0.2f, 2, 0);
        bridge.dispatch (MidiTargetCategory::PitchFader,   0, 0.5f, 0, 0);

        RecordingAudioHandler h;
        const int drained = bridge.drainAudioThreadFifo (h);
        expectEquals (drained, 2);
        expect (h.events[0].category == MidiTargetCategory::JogScratch);
        expect (h.events[1].category == MidiTargetCategory::JogBend);

        expect (pumpUntil ([&] { return sink.events.size() >= 2; }, 1000));
        expectEquals ((int) sink.events.size(), 2);
        expect (sink.events[0].category == MidiTargetCategory::TransportPlay);
        expect (sink.events[1].category == MidiTargetCategory::PitchFader);

        bridge.setMessageThreadSink (nullptr);
    }

    //----------------------------------------------------------------------
    void testStressInjection()
    {
        beginTest ("Stress: 100k AudioThread injections accounted for (delivered + dropped == total)");
        MidiMessageBridge bridge;

        constexpr int total = 100'000;
        std::atomic<int> okCount { 0 };
        std::atomic<int> dropCount { 0 };

        // Consumer: drain every ~1 ms on this thread (simulating audio cb).
        std::atomic<bool> producerDone { false };
        std::atomic<int>  consumedCount { 0 };

        std::thread producer ([&] {
            for (int i = 0; i < total; ++i)
            {
                const auto r = bridge.dispatch (MidiTargetCategory::JogScratch,
                                                (std::uint8_t) (i & 3),
                                                0.0f,
                                                (std::int16_t) (i & 0x7fff),
                                                0);
                if (r == BridgeWriteResult::Ok)
                    okCount.fetch_add (1, std::memory_order_relaxed);
                else
                    dropCount.fetch_add (1, std::memory_order_relaxed);
            }
            producerDone.store (true, std::memory_order_release);
        });

        RecordingAudioHandler h;
        while (! producerDone.load (std::memory_order_acquire)
               || bridge.getDroppedFullCount() + (std::uint64_t) consumedCount.load()
                  < (std::uint64_t) total)
        {
            const int n = bridge.drainAudioThreadFifo (h);
            consumedCount.fetch_add (n, std::memory_order_relaxed);
            // Reuse vector capacity to avoid unbounded growth.
            if (h.events.size() > 16384) h.events.clear();
            std::this_thread::yield();
        }
        producer.join();
        // Final drain in case any straggler events remain.
        bridge.drainAudioThreadFifo (h);

        const auto ok    = okCount.load();
        const auto drop  = dropCount.load();
        expectEquals (ok + drop, total, "every injection accounted for");
        expectEquals ((juce::int64) bridge.getDroppedFullCount(), (juce::int64) drop,
                      "atomic drop counter matches producer-observed drops");
        // We expect SOME successful events (FIFO did its job under load).
        expect (ok > 0, "FIFO delivered at least some events under stress");
    }

    //----------------------------------------------------------------------
    void testNullSinkDoesNotCrash()
    {
        beginTest ("MessageThread dispatch without a sink is a silent no-op (no crash)");
        MidiMessageBridge bridge;
        // No setMessageThreadSink call.
        const auto r = bridge.dispatch (MidiTargetCategory::TransportPlay, 0, 1.0f, 0, 0);
        expect (r == BridgeWriteResult::Ok, "dispatch still returns Ok when no sink");
        // Pump the loop briefly; nothing should be delivered, but also
        // nothing should crash.
        auto* mm = juce::MessageManager::getInstance();
        mm->runDispatchLoopUntil (50);
        expect (true, "no crash");
    }

    //----------------------------------------------------------------------
    void testPodInvariants()
    {
        beginTest ("MidiAudioEvent and MidiMessageEvent are trivially copyable PODs");
        // Compile-time guarantees verified by static_assert in headers; we
        // re-check at runtime as a tripwire against accidental future changes.
        expect (std::is_trivially_copyable_v<MidiAudioEvent>);
        expect (std::is_trivially_copyable_v<MidiMessageEvent>);
        expect (std::is_standard_layout_v<MidiAudioEvent>);
        expect (std::is_standard_layout_v<MidiMessageEvent>);
    }
};

static MidiMessageBridgeTests midiMessageBridgeTests;
