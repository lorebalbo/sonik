#pragma once

#include "StemData.h"
#include "StemCache.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/DeckIdentifiers.h"
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>

/// Background ThreadPoolJob that performs stem separation on a single track.
///
/// Pipeline: resample → write temp WAV → Python subprocess (BS-RoFormer)
///           → read output WAVs → assemble 4-slot StemData → cache.
/// Cooperative cancellation via shouldExit(), checked after every major step.
class StemSeparationEngine : public juce::ThreadPoolJob
{
public:
    using CompletionCallback = std::function<void (const juce::String& deckId,
                                                    StemData::Ptr result,
                                                    const juce::String& error)>;

    /// @param deckId        The deck requesting separation.
    /// @param contentHash   Content hash of the source track.
    /// @param sourceBuffer  Decoded PCM of the full track (stereo, device rate).
    /// @param cache         Reference to the stem cache manager.
    /// @param stemsNode     ValueTree Stems node for this deck (for progress updates).
    /// @param deviceRate    The device sample rate.
    /// @param pythonPath    Path to the Python 3 interpreter.
    /// @param scriptPath    Path to the separation helper script.
    /// @param modelDir      Path to the model directory.
    /// @param callback      Called on completion (success or failure).
    StemSeparationEngine (const juce::String& deckId,
                           const juce::String& contentHash,
                           AudioBufferHolder::Ptr sourceBuffer,
                           StemCache& cache,
                           juce::ValueTree stemsNode,
                           double deviceRate,
                           const juce::String& pythonPath,
                           const juce::File& scriptPath,
                           const juce::File& modelDir,
                           CompletionCallback callback);

    ~StemSeparationEngine() override = default;

    JobStatus runJob() override;

private:
    // Pipeline stages
    bool resampleToModelRate (juce::AudioBuffer<float>& output);
    bool writeSourceToWav (const juce::AudioBuffer<float>& buffer,
                           const juce::File& outputFile);
    bool runPythonSeparation (const juce::File& inputWav,
                              const juce::File& outputDir,
                              juce::File& vocalsFile,
                              juce::File& instrumentalFile);
    AudioBufferHolder::Ptr readWavFile (const juce::File& file);
    bool assembleStemData (AudioBufferHolder::Ptr vocals,
                           AudioBufferHolder::Ptr instrumental,
                           int numSamples,
                           StemData& output);

    void reportProgress (float prog);
    void reportError (const juce::String& message);

    static constexpr double kModelSampleRate = 44100.0;

    juce::String         deckId;
    juce::String         contentHash;
    AudioBufferHolder::Ptr sourceBuffer;
    StemCache&           stemCache;
    juce::ValueTree      stemsNode;
    double               deviceSampleRate;
    juce::String         pythonPath;
    juce::File           scriptPath;
    juce::File           modelDir;
    CompletionCallback   completionCallback;

    double               lastProgressTime = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparationEngine)
};
