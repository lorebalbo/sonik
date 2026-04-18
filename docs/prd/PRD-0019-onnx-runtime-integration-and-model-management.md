---
status: Not Implemented
epic: EPIC-0002
depends-on: []
---

# 1. PRD-0019: ONNX Runtime Integration & Model Management

## 1.1. Problem

Sonik's stem separation feature requires running a deep neural network (Hybrid Transformer Demucs / `htdemucs`) to decompose audio tracks into individual stems. This model is designed for the ONNX format and requires a dedicated inference runtime to execute. Without ONNX Runtime integrated into the build system and linked correctly, no ML inference can occur — the entire stem separation Epic is blocked.

On macOS with Apple Silicon, the inference must leverage the GPU (via CoreML Execution Provider) to achieve acceptable processing times. Without GPU acceleration, a 5-minute track would take 30-60+ minutes on CPU alone — an unacceptable user experience. Additionally, the `htdemucs` model weights are 80-300 MB and cannot be compiled into the binary; they must be managed as external files with versioned discovery, validation, and caching.

This PRD is a foundational infrastructure layer: it delivers no user-visible functionality on its own, but every downstream PRD in EPIC-0002 depends on it.

## 1.2. Objective

The system provides:
- ONNX Runtime C++ library integrated into the CMake build system, compiling and linking successfully on macOS (Apple Silicon / arm64).
- The CoreML Execution Provider enabled and functional, allowing ONNX inference sessions to execute on Apple GPU hardware.
- A model management subsystem that locates, validates, and loads the `htdemucs` ONNX model file from a well-known application support directory.
- A thin C++ wrapper around the ONNX Runtime C API that creates inference sessions, configures execution providers, and exposes a clean interface for downstream consumers (PRD-0020).
- The model file version is tracked so that future model upgrades trigger cache invalidation of previously separated stems.

## 1.3. User Flow

This PRD delivers no direct user-facing flow. The following describes the developer/system-level flow:

1. The developer clones the repository and runs `cmake -B build`. CMake downloads or locates the ONNX Runtime pre-built release (arm64 macOS) and configures link targets. The configure step completes without errors.
2. The developer runs `cmake --build build --parallel`. The project compiles and links successfully with ONNX Runtime and CoreML framework dependencies.
3. On first application launch, the model management subsystem checks for the `htdemucs` ONNX model file in `~/Library/Application Support/Sonik/Models/`. If the model file is not found, the subsystem logs a clear diagnostic message and the stem separation feature reports "Model not available" via the ValueTree Stems node status.
4. The user (or an installer/setup script) places the `htdemucs.onnx` model file in the expected directory. On the next stem separation request (PRD-0020), the model manager locates the file, validates its size and version metadata, and provides a valid file path to the inference session factory.
5. The inference session factory creates an `Ort::Session` with the CoreML Execution Provider configured. If CoreML is unavailable (e.g., running on an Intel Mac), it falls back to the CPU Execution Provider and logs a warning.
6. If the model file is corrupt or incompatible (wrong ONNX opset, truncated download), the session creation fails gracefully with a descriptive error published to the Stems node status — no crash, no undefined behavior.

## 1.4. Acceptance Criteria

- [ ] ONNX Runtime (version 1.17+) is fetched at CMake configure time via CPM or as a pre-built release download, targeting macOS arm64.
- [ ] The project compiles and links cleanly with ONNX Runtime on macOS Apple Silicon (AppleClang 17+).
- [ ] CoreML Execution Provider is enabled in the ONNX session configuration; inference sessions created by the wrapper use GPU acceleration when available on Apple Silicon.
- [ ] If CoreML is unavailable at runtime (Intel Mac, or framework not found), the session falls back to the CPU Execution Provider without crashing; a warning is logged.
- [ ] A model directory path is defined as `~/Library/Application Support/Sonik/Models/` and the directory is created automatically on first access if it does not exist.
- [ ] The model manager locates the `htdemucs.onnx` file in the model directory and validates that the file exists, is readable, and has a non-zero file size before passing it to session creation.
- [ ] If the model file is missing, the model manager sets the Stems node `status` property to `"model_unavailable"` and does not attempt inference — no crash, no hang.
- [ ] If the model file is corrupt or session creation fails, the error is caught, a descriptive message is published to the Stems node `status` property as `"model_error"`, and the `stemError` property is set with a human-readable description. The application continues running normally.
- [ ] `"model_unavailable"` and `"model_error"` are distinct statuses: `"model_unavailable"` means the file is not found at the expected path; `"model_error"` means the file exists but is corrupt, incompatible, or session creation failed. Both are communicated to PRD-0023 for distinct UI treatment.
- [ ] The `OnnxInference` wrapper class exposes a method to create a configured `Ort::Session` given a model file path, returning either a valid session or an error — no exceptions escape to callers.
- [ ] The ONNX Runtime environment (`Ort::Env`) is created once per application lifetime (not per session, not per inference call) and shared across all stem separation jobs.
- [ ] A model version identifier (derived from the model filename or an embedded metadata convention, e.g., `htdemucs_v4`) is exposed by the model manager for use in stem cache keying (PRD-0020).
- [ ] The `OnnxInference` wrapper does NOT perform any inference in this PRD — it only creates and configures sessions. Actual `Run()` calls are deferred to PRD-0020.
- [ ] All ONNX Runtime session creation and model loading occurs off the audio thread and off the message thread (on a background thread or lazily on first separation request).
- [ ] No ONNX Runtime headers or symbols leak into audio engine headers — the dependency is encapsulated within `Source/Features/StemSeparation/`.
- [ ] The `CMakeLists.txt` changes do not break existing targets (Sonik app and SonikTests both compile and link).
- [ ] Apple framework dependencies required by CoreML EP (`CoreML.framework`, `Foundation.framework`) are linked conditionally on macOS only.
- [ ] All new code resides under `Source/Features/StemSeparation/` following Feature-Sliced Design.
- [ ] The ONNX Runtime license (MIT) is acknowledged in the project's dependency documentation or build output.