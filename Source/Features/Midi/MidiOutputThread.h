#pragma once
//==============================================================================
// PRD-0047: MidiOutputThread
//
// Dedicated worker thread that drains the OutboundMidiFifo and calls
// juce::MidiOutput::sendMessageNow on its own thread. Keeps `sendMessageNow`
// (which may briefly block on driver-level mutexes on some platforms) off
// both the Message thread and the audio thread.
//
// THREAD MODEL: producers are Message-thread (MidiFeedbackEngine listener
// callbacks + boot-dump + blackout-dump + blink timer). Consumer is this
// thread. Wakeups via juce::WaitableEvent.
//
// FEATURES:
//   * Per-device token-bucket throttle: capacity 1000 tokens, refill 1000
//     tokens/sec (1 token per ms). Drops are visible only as delayed sends;
//     no message is dropped solely due to throttling — they wait until a
//     token is available.
//   * Consecutive-same-key coalescing: when the next ready event in the
//     FIFO shares the same (deviceId, status, channel, data1) as the
//     current head, the older event is discarded.
//   * Per-device epoch dropping: incrementing a device's epoch invalidates
//     every queued event currently in the FIFO for that device.
//   * earliestSendTime gating: an event whose earliestSendTime is in the
//     future is held in a small sorted "pending" buffer until its time
//     arrives; the worker sleeps with a bounded timeout in the meantime.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace sonik::midi
{
    struct OutboundMidiEvent
    {
        std::uint64_t deviceId       { 0 };
        std::uint32_t deviceEpoch    { 0 };   // Stamped at push time. Dropped if mismatched on pop.
        std::uint8_t  status         { 0 };   // 0x90 (note) or 0xB0 (cc).
        std::uint8_t  channel        { 1 };   // 1..16
        std::uint8_t  data1          { 0 };
        std::uint8_t  value          { 0 };
        std::chrono::steady_clock::time_point earliestSendTime {};
    };

    //--------------------------------------------------------------------------
    /** Lock-free single-producer-single-consumer outbound FIFO.
        Producers (Message thread) MUST serialise externally — the PRD-0047
        engine only writes from the Message thread, so this holds by design. */
    class OutboundMidiFifo
    {
    public:
        static constexpr int Capacity = 1024;

        OutboundMidiFifo() : fifo (Capacity) {}

        /** Returns false if the FIFO is full (event dropped). */
        bool push (const OutboundMidiEvent& ev);

        /** Pops one event into `out`; returns false if empty. */
        bool pop (OutboundMidiEvent& out);

        /** Peek the next ready event without removing it.
            Returns false if empty. */
        bool peek (OutboundMidiEvent& out) const;

        int  numReady() const noexcept { return fifo.getNumReady(); }

    private:
        juce::AbstractFifo                            fifo;
        std::array<OutboundMidiEvent, Capacity>       buffer {};
    };

    //--------------------------------------------------------------------------
    class MidiOutputThread final : public juce::Thread
    {
    public:
        /** Send callback: invoked on this thread when an event is ready to
            transmit. Must call MidiOutput::sendMessageNow for the matching
            device. Returning false indicates the device is no longer open
            (event is silently discarded). */
        using SendFn = std::function<bool (std::uint64_t deviceId,
                                           const juce::MidiMessage&)>;

        explicit MidiOutputThread (SendFn sender);
        ~MidiOutputThread() override;

        OutboundMidiFifo& fifo() noexcept { return outFifo; }

        /** Wake the worker. Cheap; idempotent. */
        void notify() noexcept { wakeUp.signal(); }

        /** Bump the epoch for a deviceId; any subsequent pop matching an
            earlier epoch is dropped. Returns the new epoch value. */
        std::uint32_t bumpDeviceEpoch (std::uint64_t deviceId);

        /** Current epoch for a deviceId (0 if never bumped). */
        std::uint32_t currentDeviceEpoch (std::uint64_t deviceId) const;

        /** Test helper: counter incremented every time sendMessageNow is
            actually invoked (after throttling/coalescing/epoch checks). */
        std::uint64_t sendsPerformed() const noexcept { return sendCount.load(); }

        /** Test helper: counter for events discarded by coalescing. */
        std::uint64_t coalesced() const noexcept { return coalesceCount.load(); }

        /** Test helper: counter for events discarded by epoch mismatch. */
        std::uint64_t epochDropped() const noexcept { return epochDropCount.load(); }

    private:
        void run() override;
        bool acquireToken (std::uint64_t deviceId);

        struct DeviceEpochSlot
        {
            std::uint64_t            deviceId { 0 };
            std::atomic<std::uint32_t> epoch  { 0 };
        };

        struct TokenBucket
        {
            std::uint64_t                              deviceId { 0 };
            int                                        tokens   { 1000 };
            std::chrono::steady_clock::time_point      lastRefill { std::chrono::steady_clock::now() };
        };

        SendFn                                  send;
        OutboundMidiFifo                        outFifo;
        juce::WaitableEvent                     wakeUp { false };

        // Tiny per-device tables. Size cap: enough for any realistic number of
        // simultaneously connected controllers (matches PRD-0040 MaxDevices=32).
        static constexpr int                    MaxDevices = 32;
        std::array<DeviceEpochSlot, MaxDevices> epochs {};
        std::array<TokenBucket,     MaxDevices> buckets {};

        std::atomic<std::uint64_t>              sendCount     { 0 };
        std::atomic<std::uint64_t>              coalesceCount { 0 };
        std::atomic<std::uint64_t>              epochDropCount { 0 };
    };
} // namespace sonik::midi
