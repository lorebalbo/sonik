#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <string>

/// Manages the stem separation model lifecycle.
///
/// Responsibilities:
///  - Ensures the model directory exists
///    (`~/Library/Application Support/Sonik/Models/`)
///  - Validates that the BS-RoFormer .ckpt model file is present
///  - Discovers a Python 3 interpreter with audio_separator installed
///  - Writes the separation helper script to the model directory
///  - Updates the Stems ValueTree node with status/error information
class ModelManager final : private juce::Thread
{
public:
    /// @param stateTree  The root SonikState ValueTree.
    explicit ModelManager (juce::ValueTree stateTree);
    ~ModelManager() override;

    ModelManager (const ModelManager&) = delete;
    ModelManager& operator= (const ModelManager&) = delete;

    /// Returns the model version identifier.
    static constexpr const char* getModelVersion() { return "bs_roformer_v1"; }

    /// Returns the expected model filename.
    static constexpr const char* getModelFilename() { return "model_bs_roformer_ep_317_sdr_12.9755.ckpt"; }

    /// Returns the model directory path.
    static juce::File getModelDirectory();

    /// Returns true if model + Python are validated and ready.
    bool isModelReady() const;

    /// Returns the discovered Python 3 interpreter path.
    juce::String getPythonPath() const;

    /// Returns the path to the separation helper script.
    juce::File getScriptPath() const;

private:
    void run() override;  // juce::Thread — performs off-thread initialisation

    juce::String findPythonWithSeparator();
    void ensureSeparationScript();

    void updateStemsStatus (const juce::String& newStatus,
                            const juce::String& error = {});

    juce::ValueTree           rootState;
    std::atomic<bool>         modelReady { false };
    juce::String              discoveredPythonPath;
    juce::File                scriptPath;
};
