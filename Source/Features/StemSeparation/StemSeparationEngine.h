#pragma once

#include "StemData.h"
#include "StemCache.h"
#include "SpectralProcessor.h"
#include "OnnxInference.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include "../Deck/DeckIdentifiers.h"
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>

/// Background ThreadPoolJob that performs stem separation on a single track.
///
/// Pipeline: resample → STFT → ONNX inference → iSTFT → resample back → cache.
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
    /// @param inference     Reference to the OnnxInference session (owned by ModelManager).
    /// @param cache         Reference to the stem cache manager.
    /// @param stemsNode     ValueTree Stems node for this deck (for progress updates).
    /// @param deviceRate    The device sample rate.
    /// @param callback      Called on completion (success or failure).
    StemSeparationEngine (const juce::String& deckId,
                           const juce::String& contentHash,
                           AudioBufferHolder::Ptr sourceBuffer,
                           OnnxInference& inference,
                           StemCache& cache,
                           juce::ValueTree stemsNode,
                           double deviceRate,
                           CompletionCallback callback);

    ~StemSeparationEngine() override = default;

    JobStatus runJob() override;

private:
    // Pipeline stages
    bool resampleToModelRate (juce::AudioBuffer<float>& output);
    bool performSTFT (const juce::AudioBuffer<float>& input,
                       std::vector<float>& specL, std::vector<float>& specR,
                       int& nFrames);
    bool performInference (const std::vector<float>& specL,
                            const std::vector<float>& specR,
                            int nFrames,
                            std::vector<std::vector<float>>& stemSpecsL,
                            std::vector<std::vector<float>>& stemSpecsR);
    bool performISTFT (const std::vector<std::vector<float>>& stemSpecs,
                        int nFrames, int originalLength,
                        std::array<std::vector<float>, StemData::NumStems>& stemSignals);
    bool assembleStemBuffers (const std::array<std::vector<float>, StemData::NumStems>& stemSignalsL,
                               const std::array<std::vector<float>, StemData::NumStems>& stemSignalsR,
                               int originalLength,
                               StemData& output);

    void reportProgress (float prog);
    void reportError (const juce::String& message);

    static constexpr double kModelSampleRate = 44100.0;

    juce::String         deckId;
    juce::String         contentHash;
    AudioBufferHolder::Ptr sourceBuffer;
    OnnxInference&       onnxInference;
    StemCache&           stemCache;
    juce::ValueTree      stemsNode;
    double               deviceSampleRate;
    CompletionCallback   completionCallback;

    SpectralProcessor    spectralProcessor;
    double               lastProgressTime = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparationEngine)
};
