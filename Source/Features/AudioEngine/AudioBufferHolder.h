#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>

/// Ref-counted wrapper for decoded PCM data.
/// Ownership is message-thread only; the audio thread reads raw pointers
/// extracted from this holder's buffer.
class AudioBufferHolder : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<AudioBufferHolder>;

    AudioBufferHolder (juce::AudioBuffer<float>&& buf, double sr, int64_t frames)
        : audioBuffer (std::move (buf)), sampleRate (sr), numFrames (frames) {}

    const juce::AudioBuffer<float>& getBuffer() const { return audioBuffer; }
    double   getSampleRate()  const { return sampleRate; }
    int64_t  getNumFrames()   const { return numFrames; }

private:
    juce::AudioBuffer<float> audioBuffer;
    double   sampleRate;
    int64_t  numFrames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioBufferHolder)
};
