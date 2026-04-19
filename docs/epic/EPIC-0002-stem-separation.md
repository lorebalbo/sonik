---
name: EPIC-0002: Stem Separation
status: Open
---

# EPIC-0002: High-Quality Offline Stem Separation for DJ Decks

## 1. Goal and Vision

Deliver a professional-grade, on-device stem separation system that allows DJs to isolate Vocals and Instrumentals from any loaded track directly within a Sonik deck. The system targets maximum audio quality over speed, processing tracks offline using a state-of-the-art ML model (Meta's Hybrid Transformer Demucs / `htdemucs`) accelerated by Apple's CoreML/GPU execution provider via ONNX Runtime.

The end-user experience: click "Separate Stems" on a deck, watch a progress indicator while the track continues playing normally, and once processing completes, toggle "VOC" and "INST" buttons to mute/unmute stems in real-time with zero clicks or artifacts. Pre-computed stems are cached persistently so a track only needs to be separated once.

Internally, the architecture supports N stems (the model outputs 4: vocals, drums, bass, other) to enable a future upgrade to 4-stem control. The MVP UI exposes 2 toggles, with "INST" being the sum of drums + bass + other.

### 1.1. Industry Context

| Software | Stems | Approach |
|---|---|---|
| Traktor Pro 4 | 4 | On-device, offline pre-analysis |
| Serato DJ Pro | 4 | On-device, near-real-time |
| djay Pro (Algoriddim) | 4 | Apple Neural Engine (CoreML) |
| VirtualDJ | 4+ | On-device |
| Rekordbox 7 | 3 | Cloud + offline |

Sonik's approach prioritizes quality (offline `htdemucs`) over real-time speed, matching Traktor's philosophy. The N-stem internal architecture ensures competitive parity when 4-stem UI is shipped in a future iteration.

## 2. Scope & Boundaries

### 2.1. In Scope

User-facing features:

- "Separate Stems" button on each deck (disabled when no track is loaded or separation is already in progress)
- Progress indicator showing ML processing status (percentage, estimated time remaining)
- Cancellation support (user can abort a running separation)
- "VOC" toggle button to mute/unmute the vocals stem
- "INST" toggle button to mute/unmute the instrumental stem (drums + bass + other, summed)
- Instant stem enable when cached stems exist for the loaded track (no re-processing)
- Click-free mute/unmute transitions (64-sample crossfade)
- Full compatibility with all existing deck features: transport, loops, cue points, hot cues, beat jump, seeking, slip mode, and time stretching

Foundational systems (non-user-facing):

- ONNX Runtime C++ integration with CoreML Execution Provider for GPU-accelerated inference
- `htdemucs` ONNX model management (bundled or downloaded, versioned)
- STFT/iSTFT spectral processing pipeline for model input/output
- Background separation thread (dedicated, not shared with audio file loader)
- Stem WAV file export (high-quality, 32-bit float, matching source sample rate)
- Stem file cache (`~/Library/Caches/Sonik/Stems/`) keyed by content hash + model version
- SQLite `stems_data` table for cache metadata and lookup
- N-stem audio playback architecture in `DeckAudioSource` and `processBlock`
- Per-stem Rubberband time stretcher instances when key-lock is active with stems
- Atomic multi-buffer swap for thread-safe stem activation

### 2.2. Out of Scope

- Real-time (live) stem separation — offline only
- 4-stem UI (4 individual toggles for vocals/drums/bass/other) — future Epic iteration
- Stem-specific EQ or effects processing — requires Mixer/Effects Epics
- Stem export to disk (user-facing) — potential future feature
- Custom model training or model selection UI
- Cloud/server-based separation
- Stem separation for tracks not loaded into a deck

## 3. Implicit & Foundational Technical Requirements

### 3.1. Audio Thread Safety (Inherited)

All existing audio thread constraints apply without exception. Stem mixing in `processBlock` must be zero-allocation, lock-free, and complete within the buffer deadline. Per-stem gain changes (mute/unmute) use 64-sample crossfade ramps identical to the existing transport fade convention.

### 3.2. ONNX Runtime Integration

- ONNX Runtime C++ library linked via CMake (FetchContent or pre-built release)
- CoreML Execution Provider enabled for Apple Silicon GPU acceleration
- Session creation and `Run()` calls execute exclusively on a dedicated background thread
- Model weights (~80-300 MB) managed as bundled app resources or cached downloads
- Model version tracked in DB for cache invalidation on model upgrades

### 3.3. Spectral Processing

- Windowed STFT (Short-Time Fourier Transform) converts decoded PCM to complex spectrograms for model input
- Inverse STFT with overlap-add reconstructs time-domain PCM from model output masks
- Uses `juce::dsp::FFT` or FFTW (already linked via Essentia) for FFT computation
- Window function, hop size, and FFT size must match `htdemucs` training configuration exactly

### 3.4. Multi-Buffer Playback Architecture

- `DeckAudioSource` extended to hold an array of N stem buffer pointer pairs (`channelL[N]`, `channelR[N]`)
- Atomic stem activation flag prevents the audio thread from seeing partially-swapped buffers
- When stems are inactive, playback reads from the original single buffer (zero overhead)
- When stems are active, `processBlock` reads from all N stem buffers, applies per-stem gain, and sums
- The original buffer can be released once stems are verified (sum of stems = original)

### 3.5. Memory Management

- A 5-minute stereo track at 44.1 kHz ≈ 52 MB per buffer
- 4 stems per track ≈ 208 MB per deck (worst case)
- Original buffer released after stem activation to reduce to ~208 MB
- Stem buffers for inactive/unloaded decks are released immediately
- Temporary WAV files on disk serve as backing store; stems can be reloaded from cache on demand

### 3.6. Stem Cache & Persistence

- Cache directory: `~/Library/Caches/Sonik/Stems/<content_hash>/`
- Files: `vocals.wav`, `drums.wav`, `bass.wav`, `other.wav` (32-bit float WAV)
- SQLite table `stems_data`: `content_hash TEXT PRIMARY KEY, model_version TEXT, stem_paths TEXT, created_at INTEGER, file_size_bytes INTEGER`
- On track load: check cache → if hit, enable stem toggles immediately (load WAVs in background)
- Cache eviction: LRU by `created_at`, configurable max cache size (default 10 GB)

### 3.7. Time Stretching with Stems

- When key-lock is enabled and stems are active, one `RubberBandStretcher` instance is created per active stem
- Stretchers are created lazily on the message thread and published via atomic pointer (matching PRD-0011 pattern)
- All stretchers share the same speed ratio and pitch settings
- CPU budget: N stretchers = N× CPU cost; monitor via existing CPU load metric and warn user if >80%

### 3.8. Threading Model Summary

| Thread | Responsibilities |
|---|---|
| **Audio thread** | Read stem buffers, apply per-stem gain, sum, output. Zero allocation/locking. |
| **Message thread** | UI updates, ValueTree mutations, stem toggle state changes, atomic pointer swaps. |
| **Separation thread** | ONNX session creation, STFT, inference `Run()`, iSTFT, WAV export. Long-lived, cancellable. |
| **File loader thread pool** | Audio file decoding (existing). Not used for separation. |
| **Stem loader thread** | Load cached WAV stems from disk into memory buffers (on cache hit or post-separation). |

### 3.9. File Structure (Feature-Sliced Design)

```text
Source/Features/
├── StemSeparation/
│   ├── StemSeparationEngine.h/cpp    # ML inference pipeline, STFT, background thread
│   ├── StemSeparationManager.h/cpp   # Orchestrator: cache lookup, job lifecycle, state
│   ├── StemCache.h/cpp               # File cache + DB persistence
│   ├── OnnxInference.h/cpp           # ONNX Runtime session wrapper
│   ├── SpectralProcessor.h/cpp       # STFT / iSTFT utilities
│   ├── StemData.h                    # Stem buffer holder, stem metadata structs
│   └── UI/
│       ├── StemSeparateButton.h/cpp  # "Separate Stems" button + progress
│       └── StemToggleComponent.h/cpp # VOC / INST toggle buttons
```

## 4. PRD Roadmap

- [x] PRD-0019: ONNX Runtime Integration & Model Management
- [x] PRD-0020: Stem Separation Engine
- [x] PRD-0021: Stem-Aware Audio Playback
- [ ] PRD-0022: Stem-Aware Time Stretching
- [ ] PRD-0023: Stem Separation UI