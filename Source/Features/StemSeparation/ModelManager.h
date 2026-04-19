#pragma once

#include "OnnxInference.h"
#include <juce_data_structures/juce_data_structures.h>
#include <memory>
#include <string>

/// Manages the ONNX model lifecycle for stem separation.
///
/// Responsibilities:
///  - Ensures the model directory exists
///    (`~/Library/Application Support/Sonik/Models/`)
///  - Validates that htdemucs.onnx is present, readable, and non-empty
///  - Creates an OnnxInference session (off audio & message threads)
///  - Updates the Stems ValueTree node with status/error information
///
/// Owns the single OnnxInference instance (which in turn owns the
/// application-lifetime Ort::Env).
class ModelManager final : private juce::Thread
{
public:
    /// @param stateTree  The root SonikState ValueTree.
    explicit ModelManager (juce::ValueTree stateTree);
    ~ModelManager() override;

    ModelManager (const ModelManager&) = delete;
    ModelManager& operator= (const ModelManager&) = delete;

    /// Returns the model version identifier (e.g. "htdemucs_v4").
    static constexpr const char* getModelVersion() { return "htdemucs_v4"; }

    /// Returns the expected model filename.
    static constexpr const char* getModelFilename() { return "htdemucs.onnx"; }

    /// Returns the model directory path.
    static juce::File getModelDirectory();

    /// Returns true if a valid ONNX session is available.
    bool isModelReady() const;

private:
    void run() override;  // juce::Thread — performs off-thread initialisation

    void updateStemsStatus (const juce::String& newStatus,
                            const juce::String& error = {});

    OnnxInference             inference;
    juce::ValueTree           rootState;
    std::atomic<bool>         modelReady { false };
};
