#pragma once

#include "../AudioEngine/AudioBufferHolder.h"
#include <juce_core/juce_core.h>
#include <array>

/// Holds the 4 separated stem buffers (vocals, drums, bass, other).
/// Each stem is an AudioBufferHolder::Ptr — ref-counted, message-thread owned.
struct StemData : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<StemData>;

    StemData() = default;

    enum StemIndex
    {
        Vocals = 0,
        Drums  = 1,
        Bass   = 2,
        Other  = 3,
        NumStems = 4
    };

    std::array<AudioBufferHolder::Ptr, NumStems> stems;

    AudioBufferHolder::Ptr getVocals() const { return stems[Vocals]; }
    AudioBufferHolder::Ptr getDrums()  const { return stems[Drums]; }
    AudioBufferHolder::Ptr getBass()   const { return stems[Bass]; }
    AudioBufferHolder::Ptr getOther()  const { return stems[Other]; }

    static constexpr const char* stemName (int index)
    {
        constexpr const char* names[] = { "vocals", "drums", "bass", "other" };
        return (index >= 0 && index < NumStems) ? names[index] : "unknown";
    }

    static constexpr const char* stemFilename (int index)
    {
        constexpr const char* names[] = { "vocals.wav", "drums.wav", "bass.wav", "other.wav" };
        return (index >= 0 && index < NumStems) ? names[index] : "unknown.wav";
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemData)
};

/// Metadata for a cached stem separation result.
struct StemCacheRecord
{
    juce::String contentHash;
    juce::String modelVersion;
    juce::String vocalPath;
    juce::String drumsPath;
    juce::String bassPath;
    juce::String otherPath;
    juce::String status;      // "pending" or "complete"
    int64_t      createdAt    = 0;
    int64_t      fileSizeBytes = 0;
};
