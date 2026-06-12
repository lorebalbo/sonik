#pragma once

#include "WaveformData.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/Database/TrackDatabase.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <vector>

class WaveformAnalyzer final
{
public:
    using Callback = std::function<void (const juce::String& contentHash, WaveformData::Ptr data)>;

    explicit WaveformAnalyzer (TrackDatabase& database);
    ~WaveformAnalyzer();

    WaveformAnalyzer (const WaveformAnalyzer&) = delete;
    WaveformAnalyzer& operator= (const WaveformAnalyzer&) = delete;

    void analyze (const juce::String& contentHash,
                  AudioBufferHolder::Ptr buffer,
                  Callback callback);

    /// Analyse the SUM of several buffers under a single key, computing the mix on
    /// the background thread (no message-thread allocation/copy of multi-minute
    /// audio). Used for the instrumental stem waveform (drums + bass + other).
    /// Null entries are skipped; the sum's length is the shortest contributor.
    void analyzeSum (const juce::String& cacheKey,
                     std::vector<AudioBufferHolder::Ptr> buffers,
                     Callback callback);

private:
    class AnalysisJob;

    TrackDatabase& db;
    juce::ThreadPool threadPool { 1 };
};
