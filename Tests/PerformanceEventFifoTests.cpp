#include <juce_core/juce_core.h>

#include "Features/Daw/Recording/PerformanceEventFifo.h"

#include <type_traits>
#include <vector>

namespace
{

// Records every delivered event so tests can assert order and content.
struct CapturingHandler final : public Daw::PerformanceEventHandler
{
    std::vector<Daw::PerformanceEvent> received;
    void onPerformanceEvent (const Daw::PerformanceEvent& e) override { received.push_back (e); }
};

Daw::PerformanceEvent makeEvent (Daw::PerformanceEventType type,
                                 std::uint8_t deck,
                                 std::int64_t srcPos,
                                 std::int64_t payload = 0)
{
    Daw::PerformanceEvent e;
    e.type                 = type;
    e.deckIndex            = deck;
    e.sourceSamplePosition = srcPos;
    e.payload              = payload;
    return e;
}

} // namespace

//==============================================================================
class PerformanceEventFifoTests : public juce::UnitTest
{
public:
    PerformanceEventFifoTests()
        : juce::UnitTest ("Performance Event Fifo", "Sonik") {}

    void runTest() override
    {
        using namespace Daw;

        beginTest ("PerformanceEvent is trivially copyable");
        {
            expect (std::is_trivially_copyable_v<PerformanceEvent>);
        }

        beginTest ("Event taxonomy contains the required structural types");
        {
            // Compile-time existence + a runtime ordering sanity check.
            expect ((int) PerformanceEventType::DeckPlay         == 0);
            expect ((int) PerformanceEventType::DeckStop         >  (int) PerformanceEventType::DeckPlay);
            expect ((int) PerformanceEventType::ChannelMute      != (int) PerformanceEventType::ChannelUnmute);
            expect ((int) PerformanceEventType::CueJumpIn        != (int) PerformanceEventType::CueJumpOut);
            // Reference every required enumerator so a removal fails to compile.
            const PerformanceEventType all[] = {
                PerformanceEventType::DeckPlay,   PerformanceEventType::DeckStop,
                PerformanceEventType::ChannelMute, PerformanceEventType::ChannelUnmute,
                PerformanceEventType::CueJumpIn,  PerformanceEventType::CueJumpOut,
                PerformanceEventType::BeatJump,   PerformanceEventType::LoopIn,
                PerformanceEventType::LoopOut,    PerformanceEventType::SourceModeChange
            };
            expectEquals ((int) std::size (all), 10);
        }

        beginTest ("enqueue then drain reads events back in order");
        {
            PerformanceEventFifo fifo;
            CapturingHandler handler;

            expect (fifo.enqueue (makeEvent (PerformanceEventType::DeckPlay,   0, 100)));
            expect (fifo.enqueue (makeEvent (PerformanceEventType::CueJumpIn,  0, 200)));
            expect (fifo.enqueue (makeEvent (PerformanceEventType::DeckStop,   0, 300)));

            const int drained = fifo.drain (handler);
            expectEquals (drained, 3);
            expectEquals ((int) handler.received.size(), 3);

            expect (handler.received[0].type == PerformanceEventType::DeckPlay);
            expect (handler.received[1].type == PerformanceEventType::CueJumpIn);
            expect (handler.received[2].type == PerformanceEventType::DeckStop);

            expectEquals (handler.received[0].sourceSamplePosition, (std::int64_t) 100);
            expectEquals (handler.received[1].sourceSamplePosition, (std::int64_t) 200);
            expectEquals (handler.received[2].sourceSamplePosition, (std::int64_t) 300);

            // Timestamps are a monotonic global order assigned at enqueue.
            expect (handler.received[0].timestamp < handler.received[1].timestamp);
            expect (handler.received[1].timestamp < handler.received[2].timestamp);
        }

        beginTest ("Draining an empty FIFO is a no-op");
        {
            PerformanceEventFifo fifo;
            CapturingHandler handler;
            expectEquals (fifo.drain (handler), 0);
            expect (handler.received.empty());
        }

        beginTest ("Full FIFO drops newest and increments overflow counter without blocking");
        {
            PerformanceEventFifo fifo;
            int accepted = 0;
            for (int i = 0; i < PerformanceEventFifo::Capacity + 50; ++i)
                if (fifo.enqueue (makeEvent (PerformanceEventType::BeatJump, 1, i)))
                    ++accepted;

            expectEquals (accepted, PerformanceEventFifo::Capacity);
            expectEquals ((int) fifo.getOverflowCount(), 50);

            // The intact ordered prefix is still fully drainable.
            CapturingHandler handler;
            const int drained = fifo.drain (handler);
            expectEquals (drained, PerformanceEventFifo::Capacity);
            // Drop-newest: the retained events are the first Capacity enqueued.
            expectEquals (handler.received.front().sourceSamplePosition, (std::int64_t) 0);
            expectEquals (handler.received.back().sourceSamplePosition,
                          (std::int64_t) (PerformanceEventFifo::Capacity - 1));
        }

        beginTest ("drain processes events across a wrap-around boundary");
        {
            PerformanceEventFifo fifo;
            CapturingHandler handler;

            // Advance the read/write indices near the end of the ring so the
            // next batch straddles the wrap boundary (two non-contiguous blocks).
            const int prime = PerformanceEventFifo::Capacity - 3;
            for (int i = 0; i < prime; ++i)
                expect (fifo.enqueue (makeEvent (PerformanceEventType::LoopIn, 2, i)));
            expectEquals (fifo.drain (handler), prime); // empties, leaving indices advanced
            handler.received.clear();

            // Now enqueue a batch that wraps around the physical array end.
            std::vector<std::int64_t> expectedOrder;
            for (int i = 0; i < 8; ++i)
            {
                const std::int64_t pos = 1000 + i;
                expect (fifo.enqueue (makeEvent (PerformanceEventType::LoopOut, 2, pos)));
                expectedOrder.push_back (pos);
            }

            const int drained = fifo.drain (handler);
            expectEquals (drained, 8);
            expectEquals ((int) handler.received.size(), 8);
            for (int i = 0; i < 8; ++i)
                expectEquals (handler.received[(std::size_t) i].sourceSamplePosition, expectedOrder[(std::size_t) i]);
        }

        beginTest ("Interleaved producers drain in a single deterministic order");
        {
            PerformanceEventFifo fifo;
            CapturingHandler handler;

            // Simulate audio- and message-originated enqueues interleaved; the
            // single FIFO preserves the exact enqueue order across decks.
            expect (fifo.enqueue (makeEvent (PerformanceEventType::DeckPlay,    0, 10))); // audio, deck A
            expect (fifo.enqueue (makeEvent (PerformanceEventType::ChannelMute, 1, 20))); // message, deck B
            expect (fifo.enqueue (makeEvent (PerformanceEventType::CueJumpIn,   0, 30))); // audio, deck A
            expect (fifo.enqueue (makeEvent (PerformanceEventType::DeckStop,    1, 40))); // message, deck B

            fifo.drain (handler);
            expectEquals ((int) handler.received.size(), 4);
            expect (handler.received[0].type == PerformanceEventType::DeckPlay);
            expect (handler.received[1].type == PerformanceEventType::ChannelMute);
            expect (handler.received[2].type == PerformanceEventType::CueJumpIn);
            expect (handler.received[3].type == PerformanceEventType::DeckStop);
            // Strictly increasing global timestamps prove the total order.
            for (std::size_t i = 1; i < handler.received.size(); ++i)
                expect (handler.received[i - 1].timestamp < handler.received[i].timestamp);
        }

        beginTest ("payload field round-trips for consumer interpretation");
        {
            PerformanceEventFifo fifo;
            CapturingHandler handler;
            expect (fifo.enqueue (makeEvent (PerformanceEventType::BeatJump, 3, 500, /*payload*/ -4)));
            fifo.drain (handler);
            expectEquals ((int) handler.received.size(), 1);
            expectEquals (handler.received[0].payload, (std::int64_t) -4);
        }
    }
};

static PerformanceEventFifoTests performanceEventFifoTests;
