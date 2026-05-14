#pragma once

#include <cstdint>
#include <juce_core/juce_core.h>

namespace sonik::midi
{
    /** Public, copyable snapshot of one MIDI endpoint as known to the
        device manager. Returned by `MidiDeviceManager::getDevices()` and
        passed by value to listeners. The actual RAII-owning `unique_ptr`s
        live inside `MidiDeviceManager`'s internal storage. */
    struct MidiDeviceRecord
    {
        std::uint64_t deviceId       = 0;     // stable, SHA-1-derived
        juce::String  juceIdentifier;          // identifier from juce::MidiDeviceInfo
        juce::String  manufacturer;            // may be empty on platforms that don't expose it
        juce::String  productName;             // device's human-readable name
        int           ordinal        = 0;      // disambiguator for duplicate (manufacturer, productName)
        bool          isInput        = true;
        bool          isConnected    = true;
        bool          isOpen         = false;
    };
}
