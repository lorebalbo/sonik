#pragma once

#include "BeatGridData.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/Database/TrackDatabase.h"
#include <juce_core/juce_core.h>
#include <functional>

class BeatGridAnalyzer final
{
public:
    using Callback = std::function<void (const juce::String& contentHash, BeatGridData::Ptr data)>;

    explicit BeatGridAnalyzer (TrackDatabase& database);
    ~BeatGridAnalyzer();

    BeatGridAnalyzer (const BeatGridAnalyzer&) = delete;
    BeatGridAnalyzer& operator= (const BeatGridAnalyzer&) = delete;

    void analyze (const juce::String& contentHash,
                  const juce::String& filePath,
                  AudioBufferHolder::Ptr buffer,
                  Callback callback);

private:
    class AnalysisJob;

    TrackDatabase& db;
    juce::ThreadPool threadPool { 1 };
};
