#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioBufferHolder.h"
#include "../Deck/DeckStateManager.h"
#include <deque>
#include <map>
#include <mutex>

class AudioEngine;

/// Background audio file decoder and loader (PRD-0003).
/// Validates, decodes, resamples, and delivers PCM buffers to the audio engine.
class AudioFileLoader final
{
public:
    AudioFileLoader (DeckStateManager& deckState,
                     AudioEngine& engine,
                     double deviceSampleRate);
    ~AudioFileLoader();

    AudioFileLoader (const AudioFileLoader&) = delete;
    AudioFileLoader& operator= (const AudioFileLoader&) = delete;

    /// Start loading a file onto the given deck (message thread).
    void loadFile (const juce::String& deckId, const juce::File& file);

    /// Cancel an in-progress decode for the given deck (message thread).
    void cancelLoad (const juce::String& deckId);

    /// Retrieve cached album art by content hash. Returns a null image if not found.
    juce::Image getAlbumArt (const juce::String& contentHash) const;

    /// Update the target device sample rate (e.g. after audio device change).
    void setDeviceSampleRate (double newRate);

    /// Supported audio file extensions.
    static bool isSupportedExtension (const juce::String& ext);

private:
    class LoadJob;

    void deliverBuffer (const juce::String& deckId,
                        AudioBufferHolder::Ptr holder,
                        const TrackMetadata& metadata);

    void storeAlbumArt (const juce::String& contentHash, juce::Image image);

    static juce::String computeContentHash (const juce::File& file);
    static juce::Image  extractAlbumArt (const juce::File& file);

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    std::atomic<double> targetSampleRate;

    juce::AudioFormatManager formatManager;
    juce::ThreadPool threadPool { 2 };

    // Album art LRU cache (message-thread only)
    mutable std::mutex artCacheMutex;
    std::map<juce::String, juce::Image> artCache;
    std::deque<juce::String> artCacheLRU;
    static constexpr int maxArtCacheSize = 200;

    // Active jobs per deck (message-thread only)
    std::map<juce::String, LoadJob*> activeJobs;
};
