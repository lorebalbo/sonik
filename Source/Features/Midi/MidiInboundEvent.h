#pragma once

#include <cstdint>
#include <type_traits>

namespace sonik::midi
{
    /** POD event delivered to every MidiInputSubscriber on the JUCE MIDI
        callback thread. Trivially copyable so it can live on the callback
        stack with zero allocation. */
    struct MidiInboundEvent
    {
        std::uint64_t deviceId;
        double        timestampSeconds;
        std::uint8_t  statusByte;
        std::uint8_t  data1;
        std::uint8_t  data2;
    };

    static_assert (std::is_trivially_copyable_v<MidiInboundEvent>,
                   "MidiInboundEvent must be trivially copyable so it can be "
                   "passed by value on the MIDI callback thread with no heap "
                   "involvement.");
}
