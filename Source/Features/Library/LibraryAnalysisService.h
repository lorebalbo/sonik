#pragma once

#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/BeatGrid/BeatGridAnalyzer.h"
#include "Features/KeyDetection/KeyDetectionAnalyzer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

/// Library-owned one-shot analysis service for context-menu analysis.
/// Decoding happens on a private background pool; BPM/key DSP reuse the existing
/// analyzers and project their results back into library_tracks on completion.
class LibraryAnalysisService final
{
public:
    using CompletionCallback = std::function<void (const juce::String& filePath, bool succeeded)>;
    using CancellationFlag = std::shared_ptr<std::atomic<bool>>;
    using ProgressCallback = std::function<void (int percent)>;

    explicit LibraryAnalysisService (TrackDatabase& database);
    ~LibraryAnalysisService();

    LibraryAnalysisService (const LibraryAnalysisService&) = delete;
    LibraryAnalysisService& operator= (const LibraryAnalysisService&) = delete;

    void analyzeTrack (const juce::String& filePath,
                       const juce::String& contentHash,
                       CompletionCallback callback);

    void analyzeTrack (const juce::String& filePath,
                       const juce::String& contentHash,
                       CompletionCallback callback,
                       CancellationFlag cancelFlag,
                       ProgressCallback progressCallback);

private:
    class DecodeJob;
    struct AnalysisState;

    void runAnalyzers (const juce::String& filePath,
                       const juce::String& contentHash,
                       AudioBufferHolder::Ptr holder,
                       CompletionCallback callback,
                       CancellationFlag cancelFlag,
                       ProgressCallback progressCallback);

    TrackDatabase&       db;
    BeatGridAnalyzer     beatGridAnalyzer;
    KeyDetectionAnalyzer keyDetectionAnalyzer;
    juce::ThreadPool     decodePool { 1 };
    juce::AudioFormatManager formatManager;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LibraryAnalysisService)
};