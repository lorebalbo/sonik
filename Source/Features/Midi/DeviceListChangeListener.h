#pragma once

#include <cstdint>

namespace sonik::midi
{
    /** All callbacks are dispatched on the JUCE Message thread. */
    class DeviceListChangeListener
    {
    public:
        virtual ~DeviceListChangeListener() = default;

        virtual void midiDeviceAdded   (std::uint64_t /*deviceId*/) {}
        virtual void midiDeviceRemoved (std::uint64_t /*deviceId*/) {}
        virtual void midiDeviceOpened  (std::uint64_t /*deviceId*/) {}
        virtual void midiDeviceClosed  (std::uint64_t /*deviceId*/) {}
    };
}
