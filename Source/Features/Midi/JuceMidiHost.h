#pragma once

#include "MidiHostInterface.h"

namespace sonik::midi
{
    /** Production binding: delegates straight to juce::MidiInput / juce::MidiOutput. */
    class JuceMidiHost final : public MidiHostInterface
    {
    public:
        JuceMidiHost() = default;
        ~JuceMidiHost() override = default;

        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override;
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override;

        std::unique_ptr<juce::MidiInput> openInputDevice (
            const juce::String& identifier,
            juce::MidiInputCallback* callback) override;

        std::unique_ptr<juce::MidiOutput> openOutputDevice (
            const juce::String& identifier) override;
    };
}
