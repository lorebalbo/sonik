#pragma once

#include "WaveformData.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/Database/TrackDatabase.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

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

private:
    class AnalysisJob;

    TrackDatabase& db;
    juce::ThreadPool threadPool { 1 };
};
