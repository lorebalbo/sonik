#pragma once

#include <memory>
#include <juce_audio_devices/juce_audio_devices.h>

namespace sonik::midi
{
    /** Abstract seam between MidiDeviceManager and the JUCE/OS MIDI host.
        Production binding is `JuceMidiHost`; test fake is `MidiHostFake`. */
    class MidiHostInterface
    {
    public:
        virtual ~MidiHostInterface() = default;

        virtual juce::Array<juce::MidiDeviceInfo> getAvailableInputs() = 0;
        virtual juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() = 0;

        virtual std::unique_ptr<juce::MidiInput> openInputDevice (
            const juce::String& identifier,
            juce::MidiInputCallback* callback) = 0;

        virtual std::unique_ptr<juce::MidiOutput> openOutputDevice (
            const juce::String& identifier) = 0;
    };
}
