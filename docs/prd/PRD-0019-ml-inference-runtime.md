---
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0003
---

# 1. PRD-0019: ML Inference Runtime

## 1.1. Problem

Sonik's stem separation feature (future PRD-0020) requires running a large neural network (BS-RoFormer, ~200-500 MB) to isolate vocals, drums, bass, and other instruments from a mixed audio signal. This inference workload is computationally intense — a single 5-minute track can take 10-60 seconds depending on hardware — and must never touch the audio thread. Without a dedicated, well-engineered ML inference runtime layer, every feature that wants to use a neural network must independently solve model loading, session lifecycle, tensor memory management, execution provider selection, threading, progress reporting, cancellation, and error handling. The result would be duplicated, fragile code that is impossible to test in isolation, difficult to extend to future models (e.g., vocal pitch analysis, beat detection via ML), and prone to catastrophic failures like audio-thread stalls from an accidental blocking call or leaked GPU memory from a crashed inference session. Professional DJ software cannot tolerate these failures during a live performance. The inference runtime must be a purpose-built infrastructure component that any consumer (starting with stem separation) can call through a clean, async API without understanding the internals of ONNX Runtime, GPU driver quirks, or cross-platform execution provider differences.

## 1.2. Objective

The system provides a generic ML inference runtime that:
- Wraps the ONNX Runtime C++ API behind a Sonik-owned abstraction, isolating all ONNX Runtime types, headers, and lifecycle management from the rest of the codebase.
- Manages the global `OrtEnv` as a single application-lifetime instance, and creates per-model `OrtSession` objects with configurable execution providers.
- Detects available hardware acceleration at startup — CoreML on macOS and DirectML on Windows — and selects the fastest available execution provider, falling back to the CPU provider transparently when hardware acceleration is unavailable or fails to initialize.
- Loads an ONNX model file from disk into an `OrtSession`, reporting progress during model loading and exposing input/output tensor metadata (names, shapes, element types) for consumer validation.
- Accepts inference requests asynchronously, executing them on a dedicated inference thread pool (separate from the UI thread, audio thread, and the file-decoding thread pool from PRD-0003), with configurable ONNX Runtime intra-op thread count to prevent starving the audio thread of CPU cores.
- Provides a typed tensor I/O API that allows consumers to allocate input tensors, fill them with data, submit them for inference, and retrieve output tensors — all using contiguous float buffers with explicit shape declarations, no ONNX Runtime types leaking to the consumer.
- Reports inference progress as a normalized percentage (0-100%) via an observable callback, enabling the UI to display a progress bar for long-running multi-segment inference workloads.
- Supports cooperative cancellation of in-progress inference via a cancellation token, allowing the user to eject a track or close the application without waiting for inference to complete.
- Handles all ONNX Runtime errors (model load failure, tensor shape mismatch, execution provider crash, out-of-memory) by catching exceptions at the runtime boundary and surfacing structured error information to the consumer — never propagating ONNX exceptions into the wider application.
- Manages model memory lifecycle: loads the model into memory on first request, keeps it resident for subsequent requests, and provides an explicit unload API for memory pressure scenarios.
- Publishes runtime capability information (available execution providers, selected provider, estimated VRAM/RAM for loaded models) to the application state tree for diagnostic display.

## 1.3. User Flow

1. The user launches Sonik. The ML inference runtime initializes the global ONNX Runtime environment (`OrtEnv`) on a background thread during application startup. The runtime probes the system for available execution providers: it attempts to initialize CoreML (macOS) or DirectML (Windows), records which providers are available, and selects the preferred provider. If no hardware acceleration is detected, the CPU provider is selected. The runtime publishes the available and selected provider names to the application state tree. No model is loaded yet; the runtime is idle.
2. The user loads a track onto Deck A (PRD-0003 decoding pipeline). The track decodes successfully. The user triggers stem separation (future PRD-0020 interaction). The stem separation consumer calls the inference runtime's `loadModel()` API, passing the path to the BS-RoFormer ONNX model file. The runtime begins loading the model on the inference thread.
3. The model file (~200-500 MB) loads into an `OrtSession`. During loading, the runtime publishes progress updates to the consumer's callback. Loading takes 2-8 seconds depending on hardware and execution provider. On completion, the runtime validates the model's input/output tensor metadata (names, shapes, element types) and makes it available to the consumer via `getInputTensorInfo()` and `getOutputTensorInfo()`.
4. The consumer (stem separation) validates that the model's expected input shape matches its audio chunk dimensions (stereo, N samples at 44.1 kHz). Validation passes. The consumer begins submitting inference requests.
5. The consumer allocates an input tensor via the runtime's `createInputTensor()` API, specifying the shape (e.g., `[1, 2, chunkSamples]` for batch-1 stereo audio). The runtime returns a `TensorHandle` wrapping a contiguous float buffer. The consumer fills the buffer with a chunk of decoded PCM audio. The consumer calls `runInference()` with the input tensor and a cancellation token.
6. The runtime dispatches the inference to the inference thread pool. The current execution provider (e.g., CoreML) processes the tensor. The runtime monitors segment-level progress and invokes the consumer's progress callback. The UI displays a progress bar on the deck.
7. Inference completes for this chunk. The runtime returns the output tensors (e.g., `[1, numStems, 2, chunkSamples]`) to the consumer via the completion callback. The consumer extracts the separated stem audio from the output tensor buffer.
8. The consumer repeats steps 5-7 for each overlapping segment of the track, accumulating separated stems. The runtime reports cumulative progress. The UI progress bar advances from 0% to 100%.
9. All segments are processed. The consumer signals completion. The model remains loaded in memory, ready for the next stem separation request on any deck.
10. The user triggers stem separation on Deck B. The runtime reuses the already-loaded model session. No model reload occurs. Inference begins immediately.
11. The user ejects Deck A while stem separation is in progress. The consumer invokes the cancellation token. The runtime cooperatively cancels the in-progress inference (the current ONNX Runtime `Run()` call completes, but the next segment is skipped). All allocated tensors for the cancelled request are freed. The consumer receives a cancellation status. No resource leak occurs.
12. The user closes Sonik. The runtime receives a shutdown signal. Any in-progress inference is cancelled via the cancellation token. The model session is destroyed, freeing model memory. The global `OrtEnv` is released. Shutdown completes cleanly with no leaked GPU or CPU memory.
13. The user launches Sonik on a machine with a GPU that does not support the required execution provider (e.g., an old Mac without CoreML ANE support, or a Windows machine without a DirectML-compatible GPU). The runtime detects the failure during provider initialization, logs a diagnostic message, and falls back to the CPU execution provider. The user experiences slower inference but identical results. The state tree reflects `executionProvider: "CPU"` so the UI can display a notice (e.g., "GPU acceleration unavailable — inference will be slower").
14. The user triggers stem separation, but the ONNX model file is missing from disk (deleted or corrupted). The runtime's `loadModel()` returns a structured error: `ModelLoadError::FileNotFound` (or `ModelLoadError::FileCorrupted`). The consumer surfaces the error to the UI: "Stem separation model not found. Please reinstall the model file." No crash, no unhandled exception.
15. The inference runtime encounters an out-of-memory condition while allocating tensors for a very long track on a memory-constrained system. The runtime catches the ONNX Runtime exception, frees any partially allocated tensors, and returns `InferenceError::OutOfMemory` to the consumer. The consumer reports the error to the user with an actionable message.

## 1.4. Acceptance Criteria

### 1.4.1. Environment and Session Lifecycle

- [ ] A single `OrtEnv` instance is created during application startup on a background thread and persists for the application's lifetime. It is destroyed during application shutdown after all sessions are released.
- [ ] `OrtEnv` creation uses `ORT_LOGGING_LEVEL_WARNING` in release builds and `ORT_LOGGING_LEVEL_VERBOSE` in debug builds.
- [ ] `OrtSession` instances are created per model via `loadModel(const juce::File& modelPath)`, which returns asynchronously with a result-or-error type.
- [ ] A loaded model session is kept resident in memory after the first `loadModel()` call. Subsequent calls with the same model path return the existing session without reloading.
- [ ] An explicit `unloadModel()` API destroys the session and frees all associated memory. Calling `unloadModel()` while inference is in progress first cancels all pending inferences, then destroys the session.
- [ ] The runtime tracks whether a model is loaded via a state enum (`ModelState::Unloaded`, `ModelState::Loading`, `ModelState::Ready`, `ModelState::Error`) published to the application state tree.

### 1.4.2. Execution Provider Detection and Selection

- [ ] On macOS, the runtime attempts to append the CoreML execution provider (`OrtSessionOptionsAppendExecutionProvider_CoreML`) with flags enabling CPU fallback for unsupported operators. If CoreML initialization fails, the runtime falls back to CPU.
- [ ] On Windows, the runtime attempts to append the DirectML execution provider. If DirectML initialization fails (no compatible GPU, missing DLL), the runtime falls back to CPU.
- [ ] On Linux (if ever supported), the runtime uses the CPU execution provider only.
- [ ] The CPU execution provider is always available as the final fallback and requires no special initialization.
- [ ] The selected execution provider name (`"CoreML"`, `"DirectML"`, or `"CPU"`) is published to the application state tree at the property path `mlRuntime/executionProvider`.
- [ ] A list of all detected providers is published to `mlRuntime/availableProviders` as a comma-separated string.
- [ ] Execution provider selection occurs once during `OrtSession` creation and cannot change for the lifetime of that session. Switching providers requires unloading and reloading the model.

### 1.4.3. Tensor I/O API

- [ ] The runtime exposes a `TensorHandle` type that wraps an `Ort::Value` internally but exposes only a `float*` data pointer and a shape vector (`std::vector<int64_t>`) to consumers. No ONNX Runtime types appear in public headers.
- [ ] `createInputTensor(const std::string& name, std::span<const int64_t> shape)` allocates a contiguous float tensor on CPU memory and returns a `TensorHandle`. The consumer writes PCM data directly into the returned buffer pointer.
- [ ] Output tensors are returned as `std::vector<TensorHandle>` from the inference completion callback, one per model output node. The consumer reads stem audio directly from the float buffer pointer.
- [ ] `getInputTensorInfo()` returns a vector of `{name, shape, elementType}` structs describing the model's expected inputs, as read from the ONNX model metadata after loading.
- [ ] `getOutputTensorInfo()` returns the same structure for the model's outputs.
- [ ] Tensor shape validation is performed before inference: if the consumer provides a tensor whose shape does not match the model's expected input shape (accounting for dynamic dimensions), the runtime returns `InferenceError::ShapeMismatch` without invoking ONNX Runtime `Run()`.

### 1.4.4. Inference Execution

- [ ] `runInference()` accepts one or more input `TensorHandle` objects, a cancellation token, and a completion callback. It dispatches the ONNX Runtime `Run()` call to the inference thread pool asynchronously.
- [ ] The inference thread pool is a dedicated pool of 1 thread (serialized inference to prevent GPU contention). It is separate from the UI thread, audio thread, and the file-decoding thread pool (PRD-0003).
- [ ] ONNX Runtime's intra-op thread count is configured to `std::max(1, totalCores / 2 - 1)` to leave CPU headroom for the audio thread, UI thread, and OS processes. This value is configurable via a setter before model loading.
- [ ] ONNX Runtime's inter-op thread count is set to 1 (single inference at a time) to prevent concurrent sessions from competing for resources.
- [ ] Inference never executes on the audio thread. This invariant is enforced by an assertion in debug builds that checks the calling thread is not the audio callback thread.
- [ ] Inference never executes on the UI/message thread. This invariant is asserted in debug builds.
- [ ] A single inference call's wall-clock time is measured and published to the consumer's completion callback for performance diagnostics.

### 1.4.5. Progress Reporting

- [ ] The runtime accepts a progress callback (`std::function<void(float progress)>`) as part of the inference request.
- [ ] For multi-segment inference workloads, the consumer is responsible for calling the runtime once per segment and computing overall progress (e.g., segment 3 of 20 = 15%). The runtime invokes the callback with per-segment granularity: 0.0 at segment start, 1.0 at segment completion.
- [ ] Progress callbacks are invoked on the inference thread. The consumer is responsible for marshalling updates to the UI thread (e.g., via `juce::MessageManager::callAsync`).
- [ ] Progress updates are published to the deck's ValueTree at `stems/inferenceProgress` as a float (0.0-1.0) by the consumer, not by the runtime directly. The runtime provides the raw callback; the consumer maps it to state.

### 1.4.6. Cancellation

- [ ] The runtime provides a `CancellationToken` class that wraps a `std::atomic<bool>`. The consumer creates a token before starting a multi-segment inference workload and passes it to each `runInference()` call.
- [ ] Before each ONNX Runtime `Run()` invocation, the runtime checks the cancellation token. If cancelled, the runtime skips the `Run()` call, frees the input tensors, and returns `InferenceResult::Cancelled` to the completion callback.
- [ ] An in-progress `Run()` call cannot be interrupted mid-execution (ONNX Runtime does not support mid-inference cancellation). The cancellation takes effect between segments. The maximum cancellation latency is the duration of one segment's inference.
- [ ] On cancellation, all allocated tensors for the cancelled request are freed. No GPU or CPU memory is leaked.
- [ ] Multiple cancellation tokens can coexist (one per deck) to allow independent cancellation of concurrent stem separation requests from different decks.

### 1.4.7. Error Handling

- [ ] All ONNX Runtime exceptions (`Ort::Exception`) are caught at the runtime boundary inside `loadModel()` and `runInference()`. No ONNX Runtime exception propagates beyond the runtime's public API.
- [ ] Errors are returned as a structured `InferenceError` enum with associated human-readable messages:
  - `ModelLoadError::FileNotFound` — model file does not exist at the specified path.
  - `ModelLoadError::FileCorrupted` — model file exists but fails ONNX validation.
  - `ModelLoadError::UnsupportedFormat` — file is not a valid ONNX model.
  - `ModelLoadError::ExecutionProviderFailed` — the selected EP failed to initialize for this model; the runtime retries with CPU and reports the fallback.
  - `InferenceError::ShapeMismatch` — input tensor shape does not match model expectations.
  - `InferenceError::OutOfMemory` — tensor allocation or inference failed due to insufficient memory.
  - `InferenceError::RuntimeError` — catch-all for unexpected ONNX Runtime errors.
  - `InferenceResult::Cancelled` — inference was cancelled via the cancellation token.
- [ ] When `ExecutionProviderFailed` occurs during model load, the runtime automatically retries with the CPU execution provider before returning an error to the consumer. If the CPU fallback also fails, the full error is returned.
- [ ] Error messages include the ONNX Runtime error string for diagnostic purposes (logged at debug level, not shown to the user).

### 1.4.8. Model File Management

- [ ] The ONNX model file is expected at a well-known path relative to the application bundle: `<AppDataDir>/models/<modelFileName>.onnx` where `AppDataDir` is `juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)` + `/Sonik/`.
- [ ] The runtime does not download, install, or update model files. Model distribution is handled by the application installer or a future model management feature. The runtime only loads from disk.
- [ ] `loadModel()` validates that the file exists, is readable, and has a non-zero size before attempting to create the `OrtSession`. Validation failures return immediately with a specific `ModelLoadError` without touching ONNX Runtime.
- [ ] The model file path is configurable via the runtime's constructor or a setter, allowing future support for user-supplied models or alternative model versions.

### 1.4.9. Memory and Resource Management

- [ ] The loaded `OrtSession` with the BS-RoFormer model consumes approximately 200-500 MB of RAM (CPU) or VRAM (GPU). This memory is allocated during `loadModel()` and freed during `unloadModel()` or application shutdown.
- [ ] Tensor memory for input and output buffers is allocated from the ONNX Runtime default allocator (CPU) for CPU inference, or from the execution provider's allocator for GPU inference. Tensors are freed when the `TensorHandle` is destroyed (RAII).
- [ ] The runtime publishes the approximate memory footprint of the loaded model to the state tree at `mlRuntime/modelMemoryMB` as an integer, read from `OrtSession`'s metadata or estimated from file size.
- [ ] No tensor allocation occurs on the audio thread. This is structurally guaranteed because the inference API is only callable from background threads.
- [ ] On application shutdown, the runtime destruction sequence is: (1) cancel all pending inferences, (2) wait for the inference thread pool to drain (with a 5-second timeout), (3) destroy all `OrtSession` instances, (4) destroy the `OrtEnv`.

### 1.4.10. State Tree Integration

- [ ] The runtime publishes the following properties to the application state tree under the `mlRuntime` subtree:
  - `executionProvider` (String): the currently active execution provider name.
  - `availableProviders` (String): comma-separated list of detected providers.
  - `modelState` (String): one of `"unloaded"`, `"loading"`, `"ready"`, `"error"`.
  - `modelName` (String): the filename of the loaded model (empty if unloaded).
  - `modelMemoryMB` (int): estimated memory usage of the loaded model.
  - `lastError` (String): the last error message (empty if no error).
- [ ] All state tree writes occur on the message thread via `juce::MessageManager::callAsync`. The runtime's internal threads never write to the state tree directly.

### 1.4.11. Code Organization and Dependencies

- [ ] All ML inference runtime code resides under `Source/Features/Stems/InferenceRuntime/`.
- [ ] The public API surface consists of a single class (`InferenceRuntime`) and supporting types (`TensorHandle`, `TensorInfo`, `CancellationToken`, `InferenceError`, `ModelLoadError`). All ONNX Runtime headers are included only in `.cpp` files, never in public `.h` headers.
- [ ] ONNX Runtime is linked as a pre-built shared library via CMake's `find_package(onnxruntime)` or a manual `target_link_libraries` to the ONNX Runtime dynamic library. The ONNX Runtime shared library (`.dylib` on macOS, `.dll` on Windows) is bundled with the application.
- [ ] The `InferenceRuntime` class receives no global state. Dependencies (state tree reference, model file path, thread pool configuration) are passed via the constructor.
- [ ] The runtime has no dependency on deck-specific or stem-separation-specific concepts. It is a generic inference API that could serve any ONNX model.

## 1.5. Grey Areas

1. **Model kept resident vs loaded on demand.** The BS-RoFormer model occupies 200-500 MB of memory. Keeping it resident after first load provides instant re-inference for additional decks but permanently consumes significant memory. Loading on demand frees memory between separation requests but adds a 2-8 second model load delay each time. Resolution: the model is kept resident after first load because professional DJs frequently separate stems on multiple tracks in rapid succession during a set. The `unloadModel()` API exists for future memory-pressure scenarios or a user-facing "free memory" action, but is not called automatically. A future Preferences PRD may add an auto-unload timer (e.g., unload after 10 minutes of inactivity).

2. **Intra-op thread count tuning.** ONNX Runtime's intra-op parallelism directly competes with the audio thread for CPU cores. Too many threads causes audio dropouts; too few makes inference unacceptably slow. Resolution: default to `max(1, totalCores / 2 - 1)` as a conservative starting point. This leaves at least half the cores plus one free for the audio thread, UI thread, OS, and other Sonik background tasks. The value is configurable so it can be tuned per-platform or exposed in a future advanced settings panel. Profiling on target hardware (M1/M2 Mac, mid-range Windows laptop) during implementation will validate this heuristic.

3. **GPU inference data transfer overhead.** CoreML and DirectML require tensor data to be transferred to GPU memory before inference and results transferred back. For large chunks of audio, this transfer overhead may negate the GPU speed advantage on systems with slow memory buses. Resolution: defer to implementation-time profiling. The runtime abstracts execution provider selection, so the consumer is unaware of where inference runs. If profiling reveals that CPU is faster than GPU for the BS-RoFormer model on certain hardware, the runtime can be configured to prefer CPU on those platforms without changing the consumer API.

4. **ONNX model format conversion from PyTorch checkpoint.** The BS-RoFormer model is distributed as a PyTorch checkpoint (`.ckpt` file). ONNX Runtime requires `.onnx` format. The conversion pipeline (PyTorch export, ONNX optimization, quantization) is a build-time concern and not part of this runtime PRD. Resolution: the conversion is handled offline by the development team. The runtime expects a pre-converted, pre-optimized `.onnx` file at the well-known path. Documentation for the conversion pipeline (including dynamic axis handling for variable-length audio chunks) will be maintained in the project's build/tooling documentation.

5. **Concurrent inference across multiple decks.** If the user triggers stem separation on Deck A and Deck B simultaneously, should the runtime run both inferences in parallel or serialize them? Resolution: serialize via a single-thread inference pool. GPU execution providers generally do not benefit from concurrent sessions (they share the same hardware), and parallel CPU inference would double the core contention. Requests are queued FIFO. The consumer can display "Queued..." status for the second deck based on the model's busy state. A future optimization could interleave segments from different decks for better perceived latency, but that is out of scope for this PRD.

6. **Graceful degradation when model file is missing at first launch.** The ONNX model is large (200-500 MB) and may not be bundled with the initial application download to reduce installer size. Resolution: the runtime does not handle model distribution — it only loads from disk. If `loadModel()` cannot find the file, it returns `ModelLoadError::FileNotFound`. The consumer (PRD-0020) surfaces this as a user-facing message with instructions. A future model management PRD may add in-app download with progress UI, integrity verification (SHA-256 checksum), and retry logic. This PRD intentionally avoids coupling the runtime to network I/O.