#include "JuceMidiHost.h"

namespace sonik::midi
{
    juce::Array<juce::MidiDeviceInfo> JuceMidiHost::getAvailableInputs()
    {
        return juce::MidiInput::getAvailableDevices();
    }

    juce::Array<juce::MidiDeviceInfo> JuceMidiHost::getAvailableOutputs()
    {
        return juce::MidiOutput::getAvailableDevices();
    }

    std::unique_ptr<juce::MidiInput> JuceMidiHost::openInputDevice (
        const juce::String& identifier, juce::MidiInputCallback* callback)
    {
        return juce::MidiInput::openDevice (identifier, callback);
    }

    std::unique_ptr<juce::MidiOutput> JuceMidiHost::openOutputDevice (
        const juce::String& identifier)
    {
        return juce::MidiOutput::openDevice (identifier);
    }
}
