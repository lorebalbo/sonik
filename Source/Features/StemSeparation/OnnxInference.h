#pragma once

#include <memory>
#include <string>

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

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
