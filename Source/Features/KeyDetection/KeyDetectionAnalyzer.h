#pragma once

#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/Database/TrackDatabase.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

class KeyDetectionAnalyzer final
{
public:
    using Callback = std::function<void (const juce::String& contentHash,
                                         int keyIndex,
                                         float confidence)>;
    using CancellationFlag = std::shared_ptr<std::atomic<bool>>;

    explicit KeyDetectionAnalyzer (TrackDatabase& database);
    ~KeyDetectionAnalyzer();

    KeyDetectionAnalyzer (const KeyDetectionAnalyzer&) = delete;
    KeyDetectionAnalyzer& operator= (const KeyDetectionAnalyzer&) = delete;

    void analyze (const juce::String& contentHash,
                  const juce::String& filePath,
                  AudioBufferHolder::Ptr buffer,
                  Callback callback,
                  CancellationFlag cancelFlag = nullptr);

private:
    class AnalysisJob;

    TrackDatabase& db;
    juce::ThreadPool threadPool { 1 };
};
