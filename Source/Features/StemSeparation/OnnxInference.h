#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

/// Encapsulates all ONNX Runtime interaction behind a PIMPL firewall.
/// No ONNX headers are exposed — only this .cpp includes them.
/// Must be created and used exclusively off the audio thread.
class OnnxInference
{
public:
    struct SessionResult
    {
        bool        success = false;
        std::string errorMessage;
    };

    OnnxInference();
    ~OnnxInference();

    OnnxInference (const OnnxInference&) = delete;
    OnnxInference& operator= (const OnnxInference&) = delete;

    /// Creates a configured Ort::Session for the given model file.
    /// Enables CoreML EP when available, falls back to CPU.
    /// Must be called off audio thread and off message thread.
    SessionResult createSession (const std::string& modelPath);

    /// Returns true if a valid session was created via createSession().
    bool hasValidSession() const;

    /// Result of a single inference run.
    struct RunResult
    {
        bool                 success = false;
        std::string          errorMessage;
        std::vector<float>   outputData;
        std::vector<int64_t> outputShape;
    };

    /// Run inference on the model.
    /// @param inputData      Flat input tensor data.
    /// @param inputShape     Shape of the input tensor (e.g. {1, 2, bins, frames}).
    /// @param inputName      Name of the model's input node.
    /// @param outputName     Name of the model's output node.
    RunResult run (const std::vector<float>& inputData,
                   const std::vector<int64_t>& inputShape,
                   const std::string& inputName,
                   const std::string& outputName);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
