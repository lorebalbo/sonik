#include "OnnxInference.h"

// --- ONNX Runtime headers are ONLY included in this translation unit ---
#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>

#include <iostream>

struct OnnxInference::Impl
{
    Ort::Env                       env;
    std::unique_ptr<Ort::Session>  session;

    Impl()
        : env (ORT_LOGGING_LEVEL_WARNING, "Sonik")
    {
    }
};

OnnxInference::OnnxInference()
    : pImpl (std::make_unique<Impl>())
{
}

OnnxInference::~OnnxInference() = default;

OnnxInference::SessionResult OnnxInference::createSession (const std::string& modelPath)
{
    SessionResult result;

    try
    {
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads (1);
        sessionOptions.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef __APPLE__
        // Attempt CoreML EP for GPU acceleration on Apple Silicon.
        // Flag 0 = use Neural Engine / GPU when available.
        try
        {
            uint32_t coremlFlags = COREML_FLAG_USE_NONE;
            Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_CoreML (
                sessionOptions, coremlFlags));
        }
        catch (const Ort::Exception& coremlEx)
        {
            // CoreML unavailable (e.g. Intel Mac) — fall back to CPU.
            std::cerr << "[Sonik] CoreML EP unavailable, falling back to CPU: "
                      << coremlEx.what() << std::endl;
        }
#endif

        pImpl->session = std::make_unique<Ort::Session> (
            pImpl->env, modelPath.c_str(), sessionOptions);

        result.success = true;
    }
    catch (const Ort::Exception& e)
    {
        pImpl->session.reset();
        result.success      = false;
        result.errorMessage  = e.what();
    }
    catch (const std::exception& e)
    {
        pImpl->session.reset();
        result.success      = false;
        result.errorMessage  = e.what();
    }

    return result;
}

bool OnnxInference::hasValidSession() const
{
    return pImpl->session != nullptr;
}

OnnxInference::RunResult OnnxInference::run (const std::vector<float>& inputData,
                                              const std::vector<int64_t>& inputShape,
                                              const std::string& inputName,
                                              const std::string& outputName)
{
    RunResult result;

    if (pImpl->session == nullptr)
    {
        result.success      = false;
        result.errorMessage  = "No valid ONNX session";
        return result;
    }

    try
    {
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu (
            OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

        // Create input tensor
        auto inputTensor = Ort::Value::CreateTensor<float> (
            memInfo,
            const_cast<float*> (inputData.data()),
            inputData.size(),
            inputShape.data(),
            inputShape.size());

        // Run inference
        const char* inputNames[]  = { inputName.c_str() };
        const char* outputNames[] = { outputName.c_str() };

        auto outputTensors = pImpl->session->Run (
            Ort::RunOptions { nullptr },
            inputNames, &inputTensor, 1,
            outputNames, 1);

        // Extract output
        auto& outputTensor = outputTensors.front();
        auto typeInfo = outputTensor.GetTensorTypeAndShapeInfo();

        result.outputShape = typeInfo.GetShape();

        size_t totalElements = 1;
        for (auto dim : result.outputShape)
            totalElements *= static_cast<size_t> (dim);

        const float* outputPtr = outputTensor.GetTensorData<float>();
        result.outputData.assign (outputPtr, outputPtr + totalElements);
        result.success = true;
    }
    catch (const Ort::Exception& e)
    {
        result.success      = false;
        result.errorMessage  = std::string ("ONNX inference error: ") + e.what();
    }
    catch (const std::exception& e)
    {
        result.success      = false;
        result.errorMessage  = std::string ("Inference error: ") + e.what();
    }

    return result;
}
