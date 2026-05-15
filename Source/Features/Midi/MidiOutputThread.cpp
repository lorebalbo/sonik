#include "MidiOutputThread.h"

#include <algorithm>

namespace sonik::midi
{
    //==========================================================================
    // OutboundMidiFifo
    //==========================================================================
    bool OutboundMidiFifo::push (const OutboundMidiEvent& ev)
    {
        const auto scope = fifo.write (1);
        if (scope.blockSize1 == 0)
            return false;
        buffer[static_cast<std::size_t> (scope.startIndex1)] = ev;
        return true;
    }

    bool OutboundMidiFifo::pop (OutboundMidiEvent& out)
    {
        const auto scope = fifo.read (1);
        if (scope.blockSize1 == 0)
            return false;
        out = buffer[static_cast<std::size_t> (scope.startIndex1)];
        return true;
    }

    bool OutboundMidiFifo::peek (OutboundMidiEvent& out) const
    {
        // juce::AbstractFifo offers prepareToRead via a const-friendly API:
        // since we know it's SPSC and we only need a snapshot, read state and
        // index the buffer directly. Cast away const on the fifo for the
        // prepareToRead — JUCE's API is non-const but it does not modify
        // logical state until finishedRead is called.
        int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
        const_cast<juce::AbstractFifo&> (fifo).prepareToRead (1, s1, b1, s2, b2);
        if (b1 == 0)
        {
            const_cast<juce::AbstractFifo&> (fifo).finishedRead (0);
            return false;
        }
        out = buffer[static_cast<std::size_t> (s1)];
        const_cast<juce::AbstractFifo&> (fifo).finishedRead (0); // don't advance
        return true;
    }

    //==========================================================================
    // MidiOutputThread
    //==========================================================================
    MidiOutputThread::MidiOutputThread (SendFn sender)
        : juce::Thread ("Sonik MIDI Output"), send (std::move (sender))
    {
        startThread (juce::Thread::Priority::high);
    }

    MidiOutputThread::~MidiOutputThread()
    {
        signalThreadShouldExit();
        wakeUp.signal();
        stopThread (1000);
    }

    std::uint32_t MidiOutputThread::bumpDeviceEpoch (std::uint64_t deviceId)
    {
        // Find existing slot, else allocate (Message-thread side; safe under
        // single-producer write contract).
        for (auto& slot : epochs)
        {
            if (slot.deviceId == deviceId)
                return slot.epoch.fetch_add (1) + 1;
        }
        for (auto& slot : epochs)
        {
            if (slot.deviceId == 0)
            {
                slot.deviceId = deviceId;
                return slot.epoch.fetch_add (1) + 1;
            }
        }
        return 0;
    }

    std::uint32_t MidiOutputThread::currentDeviceEpoch (std::uint64_t deviceId) const
    {
        for (const auto& slot : epochs)
            if (slot.deviceId == deviceId)
                return slot.epoch.load();
        return 0;
    }

    bool MidiOutputThread::acquireToken (std::uint64_t deviceId)
    {
        // Find or allocate bucket.
        TokenBucket* bucket = nullptr;
        for (auto& b : buckets)
        {
            if (b.deviceId == deviceId) { bucket = &b; break; }
        }
        if (bucket == nullptr)
        {
            for (auto& b : buckets)
            {
                if (b.deviceId == 0)
                {
                    b.deviceId  = deviceId;
                    b.tokens    = 1000;
                    b.lastRefill = std::chrono::steady_clock::now();
                    bucket = &b;
                    break;
                }
            }
        }
        if (bucket == nullptr)
            return true; // No bucket slot available; bypass throttle.

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>
                                    (now - bucket->lastRefill).count();
        if (elapsed_ms > 0)
        {
            // Refill 1 token per ms, cap at 1000.
            bucket->tokens = std::min (1000, bucket->tokens + static_cast<int> (elapsed_ms));
            bucket->lastRefill = now;
        }
        if (bucket->tokens <= 0)
            return false;
        --bucket->tokens;
        return true;
    }

    void MidiOutputThread::run()
    {
        while (! threadShouldExit())
        {
            OutboundMidiEvent ev {};
            if (! outFifo.pop (ev))
            {
                wakeUp.wait (100); // Up to 100 ms idle.
                continue;
            }

            // Epoch check — drop if a midiDeviceRemoved happened after push.
            const auto curEpoch = currentDeviceEpoch (ev.deviceId);
            if (ev.deviceEpoch != curEpoch)
            {
                ++epochDropCount;
                continue;
            }

            // Coalesce with the next event in the FIFO if they share key.
            for (;;)
            {
                OutboundMidiEvent next {};
                if (! outFifo.peek (next))
                    break;
                if (next.deviceId == ev.deviceId
                    && next.status  == ev.status
                    && next.channel == ev.channel
                    && next.data1   == ev.data1)
                {
                    // Drop the current event in favour of the newer one.
                    OutboundMidiEvent skipped {};
                    outFifo.pop (skipped);
                    ev = skipped;
                    ++coalesceCount;
                    continue;
                }
                break;
            }

            // earliestSendTime gating.
            const auto now = std::chrono::steady_clock::now();
            if (ev.earliestSendTime > now)
            {
                const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>
                                        (ev.earliestSendTime - now).count();
                wakeUp.wait (static_cast<int> (std::clamp<std::int64_t> (delta, 1, 100)));
                // Re-check epoch after wait.
                if (ev.deviceEpoch != currentDeviceEpoch (ev.deviceId))
                {
                    ++epochDropCount;
                    continue;
                }
            }

            // Token-bucket throttle.
            if (! acquireToken (ev.deviceId))
            {
                wakeUp.wait (1);
                if (ev.deviceEpoch != currentDeviceEpoch (ev.deviceId))
                {
                    ++epochDropCount;
                    continue;
                }
                // Optimistic re-try (drop the throttle if we still can't acquire).
                acquireToken (ev.deviceId);
            }

            juce::MidiMessage msg;
            if (ev.status == 0x90)
                msg = juce::MidiMessage::noteOn (ev.channel, ev.data1, static_cast<juce::uint8> (ev.value));
            else if (ev.status == 0xB0)
                msg = juce::MidiMessage::controllerEvent (ev.channel, ev.data1, ev.value);
            else
                continue;

            if (send)
                send (ev.deviceId, msg);
            ++sendCount;
        }
    }
} // namespace sonik::midi
