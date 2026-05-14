#pragma once

#include "MidiInboundEvent.h"

namespace sonik::midi
{
    /** Subscriber interface for inbound MIDI events. The single callback
        runs on the JUCE MIDI callback thread; implementations MUST be
        real-time-safe (no allocations, no locks, no logging). */
    class MidiInputSubscriber
    {
    public:
        virtual ~MidiInputSubscriber() = default;
        virtual void onMidiInbound (const MidiInboundEvent& event) noexcept = 0;
    };
}
