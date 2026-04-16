---
name: "PRD-0020: Stem Separation"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0003
  - PRD-0019
---

# 1. PRD-0020: Stem Separation

## 1.1. Problem

Professional DJs increasingly rely on stem separation to isolate vocals, drums, bass, and melodic elements during live performances — enabling creative transitions (e.g., blending the vocals of an incoming track over the instrumental of the outgoing track), live mashups, and a cappella breakdowns that are impossible with a mixed stereo file. Pioneer's CDJ-3000 and rekordbox, Traktor Pro 4, and Serato DJ Pro all ship stem separation as a headline feature. Without it, Sonik cannot compete at the professional tier.

The technical challenge is severe. The BS-RoFormer model that delivers state-of-the-art separation quality (SDR 12.9755 on the MUSDB18 benchmark) requires 10-120 seconds of GPU/CPU inference per track and produces four full-length audio buffers that quadruple the memory and disk footprint of the original file. This workload must never touch the audio thread, must not stall the UI, must not interrupt playback on any deck, and must produce stems that can be muted, soloed, and mixed in real time with zero latency once separation is complete. The system must manage a persistent stem cache on disk so that re-loading a previously separated track is instantaneous, and it must handle failure modes gracefully — a cancelled separation, a missing model file, or insufficient disk space — without corrupting deck state or crashing the application.

## 1.2. Objective

The system provides a stem separation feature for each deck that:
- Separates a loaded track into four stems — vocals, drums, bass, and other (melodic instruments, synths, pads) — using the BS-RoFormer model via the ML Inference Runtime (PRD-0019).
- Processes the full track offline on a background thread using the segmented overlap-add pipeline: the decoded PCM buffer (PRD-0003) is split into overlapping chunks, each chunk is inferred through the ONNX model, and the output chunks are overlap-added to reconstruct four continuous stem buffers at the original track length and sample rate.
- Writes the separated stem buffers to a persistent disk cache as lossless audio files, keyed by the source file's content hash, so that subsequent loads of the same track skip inference entirely and read stems from cache.
- Loads cached or freshly separated stem buffers into memory and delivers them to the audio engine (PRD-0002) as four parallel audio sources per deck, replacing the single mixed buffer during stem playback mode.
- Provides per-stem mute and solo controls that translate to atomic gain multipliers (0.0 or 1.0) read by the audio thread, enabling instant, click-free stem toggling during playback.
- Reports separation progress (0-100%) to the deck's ValueTree state (PRD-0001) for UI display, and supports cooperative cancellation at segment boundaries via the inference runtime's cancellation token.
- Falls back to the original mixed audio transparently if separation fails, is cancelled, or if stems are not yet available, ensuring the DJ never loses audio output.

## 1.3. User Flow

1. The user loads a track onto Deck A (PRD-0003 pipeline completes, the deck is in Stopped state with a decoded PCM buffer in memory). The deck state property `stems/status` is set to `"unavailable"`. The stem control panel displays four stem buttons (Vocals, Drums, Bass, Other) in a disabled/dimmed state. A "Separate" button is visible.
2. The system checks the stem cache directory for pre-computed stems matching the track's content hash. If cached stems exist and pass integrity validation (file count, sample count, sample rate match), the stem status transitions directly to `"cached"` and the stem buttons become enabled without any inference. The user flow continues at step 8.
3. The user presses the "Separate" button (or a keyboard shortcut). The deck state transitions `stems/status` from `"unavailable"` to `"separating"`. The "Separate" button transforms into a progress bar with a cancel button.
4. The stem separation consumer requests the ML Inference Runtime (PRD-0019) to load the BS-RoFormer model if it is not already resident in memory. If this is the first separation since launch, model loading takes 2-8 seconds; the progress bar displays "Loading model..." during this phase. If the model is already loaded from a previous separation on any deck, this step is skipped.
5. The separation pipeline begins: the decoded PCM buffer (stereo float, device sample rate from PRD-0003, resampled to 44.1 kHz if the device rate differs) is divided into overlapping segments. Each segment is submitted to the inference runtime as an input tensor. The runtime processes segments sequentially on its inference thread. After each segment completes, the consumer updates `stems/inferenceProgress` (0.0-1.0) in the deck's ValueTree. The UI progress bar advances.
6. The user can press the cancel button at any time during separation. The consumer signals cancellation via the `CancellationToken` (PRD-0019). The current segment's inference completes (maximum latency of one segment), then all remaining segments are skipped. Partially separated stems are discarded. The deck state transitions `stems/status` back to `"unavailable"`. The original mixed audio continues playing uninterrupted.
7. All segments complete. The consumer performs overlap-add reconstruction on the output segments to produce four continuous stem buffers (vocals, drums, bass, other), each matching the original track's sample count. The stem buffers are written to the disk cache as 32-bit float WAV files. The deck state transitions `stems/status` to `"ready"`.
8. The stem buttons become fully interactive. By default, all four stems are unmuted (playing the full mix). The user taps the "Vocals" button to mute vocals — the button dims and the vocal stem's gain atomic drops to 0.0. The audio engine immediately stops mixing the vocal stem into the deck's output. The user hears the instrumental version in real time.
9. The user double-taps (or long-presses) the "Drums" button to solo drums. All other stems mute (gain 0.0), only drums plays (gain 1.0). The button for Drums shows a solo indicator. The user hears isolated drums.
10. The user taps "Drums" again to exit solo mode. All stems return to their previous mute/unmute state before solo was activated.
11. The user adjusts the pitch fader or activates time stretching (PRD-0010, PRD-0011) while stems are active. The same pitch/time-stretch parameters apply identically to all four stem buffers, maintaining phase coherence. The stems remain in sync.
12. The user ejects the track from Deck A. Stem buffers are released from memory. The cached stem files remain on disk for future loads. The deck's stem state resets to defaults.
13. The user loads the same track onto Deck B later in the session. The cache lookup finds the previously separated stems. Within milliseconds, all four stem buffers are loaded into memory from the cache files. The stem buttons are immediately interactive — no inference required.
14. The user loads a different track onto Deck A and presses "Separate" while a separation is already in progress on Deck B. Both separations proceed without conflict — Deck B's separation continues while Deck A's request is queued by the inference runtime's single-thread pool (PRD-0019). Deck A's progress bar shows "Queued..." until Deck B's inference completes, then Deck A's separation begins. Both decks maintain uninterrupted playback throughout.
15. The user presses "Separate" but the BS-RoFormer model file is missing from disk. The inference runtime returns `ModelLoadError::FileNotFound`. The deck state transitions `stems/status` to `"error"` with `stems/errorMessage` set to `"Stem separation model not found. Please reinstall."`. The error message appears on the deck UI. The original mixed audio is unaffected.
16. The stem cache directory runs out of disk space while writing separated stems. The write operation fails, partially written files are cleaned up, and the stems remain available in memory for the current session. The deck state reflects `stems/cacheStatus` as `"write_failed"`. A non-blocking warning appears. Playback continues normally.
17. The user closes Sonik. In-progress separations are cancelled. Stem buffers are freed. The stem cache on disk persists across sessions.

## 1.4. Acceptance Criteria

### 1.4.1. BS-RoFormer Segmentation and Overlap-Add Pipeline

- [ ] The separation pipeline accepts the decoded PCM buffer from PRD-0003 as input: stereo interleaved float audio.
- [ ] If the audio device sample rate (from PRD-0002) is not 44.1 kHz, the pipeline resamples the decoded buffer to 44.1 kHz before segmentation, using the same high-quality interpolator as PRD-0003. Stem outputs are resampled back to the device sample rate after reconstruction.
- [ ] The pipeline segments the input audio into overlapping chunks of 352,800 samples (8 seconds at 44.1 kHz), consistent with BS-RoFormer's expected receptive field for the `ep_317_sdr_12.9755` checkpoint.
- [ ] Segments overlap by 25% (88,200 samples). The hop size between consecutive segment start positions is 264,600 samples (75% of the segment length).
- [ ] The final segment is zero-padded to the full segment length if the remaining audio is shorter than 352,800 samples.
- [ ] Each segment is formatted as an input tensor of shape `[1, 2, 352800]` (batch=1, channels=2, samples=352800) and submitted to the inference runtime's `runInference()` API.
- [ ] The model outputs a tensor of shape `[1, 4, 2, 352800]` (batch=1, stems=4, channels=2, samples=352800) for each segment. The four stems are indexed as: 0=vocals, 1=drums, 2=bass, 3=other.
- [ ] Overlap-add reconstruction uses a Hann window applied to the overlapping regions of adjacent segments. For each stem independently: the output of segment N is multiplied by the left half of a Hann window in its trailing overlap zone, and segment N+1 is multiplied by the right half of the Hann window in its leading overlap zone. The windowed regions are summed to produce a seamless crossfade.
- [ ] The first segment's leading edge and the last segment's trailing edge are not windowed (they have no adjacent segment to crossfade with).
- [ ] After overlap-add, each stem buffer is trimmed to exactly the original track's sample count (removing any zero-padding from the final segment).
- [ ] The four reconstructed stem buffers are validated: each has exactly the same sample count as the original input, and the sum of all four stems approximates the original mix within a tolerance of -60 dBFS RMS error.

### 1.4.2. Stem Cache System

- [ ] Separated stems are written to the cache directory at `<AppDataDir>/Sonik/StemCache/<contentHash>/` where `contentHash` is the same SHA-256 hash used by PRD-0001 to key track-specific data.
- [ ] Each stem is written as a separate 32-bit float WAV file: `vocals.wav`, `drums.wav`, `bass.wav`, `other.wav`.
- [ ] A metadata sidecar file `manifest.json` is written alongside the stem files, containing: source file content hash, source file path (informational only), source sample count, source sample rate, stem count, separation model name and checkpoint identifier, Sonik version, and a per-file SHA-256 integrity hash for each stem WAV.
- [ ] On track load (PRD-0003 completion), the stem separation system checks for a cache directory matching the track's content hash. If the directory exists and `manifest.json` passes validation (all four stem files exist, file sizes are non-zero, per-file integrity hashes match, sample count matches the loaded track), the stems are loaded from cache and `stems/status` transitions directly to `"cached"`.
- [ ] If any cache validation check fails (missing file, size mismatch, hash mismatch, sample count mismatch), the entire cache directory for that hash is deleted and `stems/status` remains `"unavailable"`. The user must re-trigger separation.
- [ ] Cache files are written sequentially (one stem at a time) on a background I/O thread separate from the inference thread and the audio thread. Write failures (disk full, permission denied) are caught, partially written files are deleted, and the error is reported to the deck state without crashing.
- [ ] The stem cache has a configurable maximum size with a default of 50 GB. When the cache exceeds the maximum size, the oldest entries (by last-access timestamp stored in each `manifest.json`) are evicted in LRU order until the cache is within budget. Eviction runs on a background thread during application startup and after each new cache write.
- [ ] Cache eviction never deletes stems for tracks currently loaded on any deck. Loaded tracks' cache entries are pinned until the track is ejected.
- [ ] The total cache size and entry count are published to the application state tree at `stemCache/totalSizeMB` and `stemCache/entryCount`.

### 1.4.3. Stem Playback Engine

- [ ] When stems are available (`stems/status` is `"ready"` or `"cached"`), the audio engine reads from four separate stem buffers instead of the single mixed PCM buffer for that deck.
- [ ] Each stem buffer is a contiguous stereo float array identical in format to the decoded PCM buffer from PRD-0003. Stem buffers are delivered to the audio engine via atomic pointer swap — zero allocation on the audio thread.
- [ ] The audio engine maintains four `std::atomic<float>` gain values per deck for stem control: `stemGain[0]` through `stemGain[3]`, corresponding to vocals, drums, bass, and other. Default value is 1.0 (unmuted).
- [ ] In `processBlock`, for each sample frame, the engine reads each stem buffer at the current playhead position, multiplies by the corresponding `stemGain` atomic, and sums the four weighted stems into the deck's output bus. This replaces the single-buffer read path used when stems are not active.
- [ ] The transition from mixed-buffer playback to stem-buffer playback (and vice versa) applies a 64-sample cosine crossfade to avoid clicks. The crossfade is triggered by an atomic flag (`stemPlaybackActive`) set by the message thread when stems become available or are deactivated.
- [ ] All four stem buffers share the same playhead position, transport state, pitch, and time-stretch parameters as the original track. No independent stem transport exists.
- [ ] If the user deactivates stem mode (toggling back to the original mix), the engine reverts to reading the single decoded PCM buffer. Stem buffers remain in memory and can be re-activated instantly.
- [ ] Stem playback adds no measurable latency beyond the existing audio engine latency (PRD-0002). The four-buffer read and weighted sum must complete within the same `processBlock` deadline as the single-buffer path.

### 1.4.4. Mute and Solo State Machine

- [ ] Each stem has two independent boolean states: `muted` and `soloed`, stored as properties in the deck's ValueTree under `stems/stem_0/muted`, `stems/stem_0/soloed` (through `stem_3`).
- [ ] The mute/solo state machine follows standard mixing console logic:
  - Toggling mute on a stem sets its `muted` flag to true. Its `stemGain` atomic is set to 0.0.
  - Toggling mute off sets `muted` to false. Its `stemGain` atomic is restored to 1.0 (unless another stem is in solo).
  - Soloing a stem sets its `soloed` flag to true. All other stems that are not soloed have their `stemGain` set to 0.0 regardless of their `muted` state. The soloed stem's `stemGain` is set to 1.0.
  - Multiple stems can be soloed simultaneously. When multiple stems are soloed, only the soloed stems have gain 1.0; all non-soloed stems have gain 0.0.
  - Unsoloing the last soloed stem exits solo mode entirely. All stems return to their individual `muted`/`unmuted` gain values.
- [ ] Gain changes from mute/solo toggling are applied by writing to the `std::atomic<float>` gain values from the message thread. The audio thread reads these values each callback — no locking, no allocation.
- [ ] To prevent clicks on mute/solo transitions, the audio engine applies a per-stem 64-sample linear fade between the previous and new gain values whenever a `stemGain` atomic changes. The fade is detected by comparing the current gain value to the previously applied value at the start of each `processBlock`.
- [ ] Mute and solo states are track-specific and reset to defaults (all unmuted, none soloed) on track load or eject, consistent with PRD-0001 state reset rules.

### 1.4.5. Separation Trigger and Progress

- [ ] Stem separation is triggered explicitly by the user pressing the "Separate" button in the deck UI. Separation does not start automatically on track load.
- [ ] The "Separate" button is enabled only when a track is loaded (`stems/status` is `"unavailable"`) and the deck is in Stopped or Playing state.
- [ ] Once separation begins, the "Separate" button is replaced by a progress bar (0-100%) and a cancel button.
- [ ] Separation progress is computed by the stem separation consumer as `completedSegments / totalSegments` and published to `stems/inferenceProgress` (float 0.0-1.0) in the deck's ValueTree. The UI reads this value via the Listener pattern.
- [ ] If the model is not yet loaded, the progress bar shows an indeterminate state with the label "Loading model..." until the inference runtime reports `ModelState::Ready`, then switches to determinate segment progress.
- [ ] Separation can be cancelled at any time via the cancel button. Cancellation is cooperative: the current segment completes, then remaining segments are skipped. Partial results are discarded. The deck state transitions `stems/status` back to `"unavailable"`.
- [ ] If the user loads a new track onto the deck while separation is in progress, the in-progress separation is cancelled automatically before the new track's load pipeline begins.
- [ ] If the user ejects the track while separation is in progress, the separation is cancelled and all stem resources are freed.

### 1.4.6. Error Handling and Fallback

- [ ] If the inference runtime returns any `ModelLoadError` (file not found, corrupted, unsupported format), `stems/status` transitions to `"error"` and `stems/errorMessage` is populated with a user-readable string. The deck UI displays the error inline in the stem control area. The original mixed audio is unaffected.
- [ ] If inference fails mid-separation (`InferenceError::OutOfMemory`, `InferenceError::RuntimeError`), all partially computed segments are discarded, `stems/status` transitions to `"error"`, and the error message is displayed. The user can retry by pressing "Separate" again.
- [ ] If cache loading fails (corrupt stem files on disk), the invalid cache is deleted and `stems/status` remains `"unavailable"`. The user can re-trigger separation. No error is shown for cache misses — only for cache corruption.
- [ ] At no point does a stem separation failure affect playback of the original mixed audio. The mixed buffer (PRD-0003) is always available as a fallback. If stems are active and a stem buffer becomes invalid (memory pressure, bug), the engine falls back to the mixed buffer with a 64-sample crossfade and logs a diagnostic warning.
- [ ] Errors are non-modal: no blocking dialogs. Error messages appear inline in the deck's stem control area and auto-dismiss after 10 seconds or on user interaction.

### 1.4.7. Memory Management

- [ ] Each stem buffer for a typical 5-minute stereo 44.1 kHz track occupies approximately 52 MB (5 * 60 * 44100 * 2 channels * 4 bytes). Four stems total approximately 210 MB per deck.
- [ ] Stem buffers are allocated on the background I/O thread during cache read or after overlap-add reconstruction. Buffers are managed by `std::unique_ptr` and ownership is transferred to the audio engine via atomic pointer swap.
- [ ] When a track is ejected, stem buffers are freed on the message thread after the audio engine has swapped the stem pointers to `nullptr`. A memory fence ensures the audio thread has stopped reading the old buffers before deallocation.
- [ ] The maximum simultaneous stem memory footprint is bounded by the deck count: 4 decks * 210 MB = 840 MB worst case (four 5-minute tracks, all separated). This is documented as a system requirement.
- [ ] Stem buffers are never allocated or freed on the audio thread.

### 1.4.8. State Tree Integration

- [ ] The following properties are managed per deck under the `stems` subtree of each deck's ValueTree node:
  - `status` (String): one of `"unavailable"`, `"separating"`, `"ready"`, `"cached"`, `"error"`.
  - `inferenceProgress` (float): 0.0-1.0 during separation, 0.0 otherwise.
  - `errorMessage` (String): human-readable error text, empty when no error.
  - `cacheStatus` (String): `"none"`, `"hit"`, `"miss"`, `"write_failed"`.
  - `stem_0/muted` through `stem_3/muted` (bool): per-stem mute state.
  - `stem_0/soloed` through `stem_3/soloed` (bool): per-stem solo state.
  - `stem_0/label` through `stem_3/label` (String): `"Vocals"`, `"Drums"`, `"Bass"`, `"Other"`.
- [ ] All ValueTree writes occur on the message thread via `juce::MessageManager::callAsync`.
- [ ] Stem state is classified as track-specific per PRD-0001: all properties reset to defaults on track load or eject.

### 1.4.9. Code Organization

- [ ] All stem separation code resides under `Source/Features/Stems/`.
- [ ] The feature slice contains the following components:
  - `StemSeparationPipeline` — orchestrates segmentation, inference calls, overlap-add reconstruction, and cache I/O. Runs on background threads.
  - `StemCache` — manages the disk cache directory, manifest validation, read/write operations, and LRU eviction.
  - `StemPlaybackEngine` — integrates with the audio engine (PRD-0002) to provide the four-buffer read path with per-stem gain atomics.
  - `StemControlComponent` — UI organism containing the four stem mute/solo buttons, the "Separate" button, and the progress bar.
- [ ] `StemSeparationPipeline` depends on the `InferenceRuntime` (PRD-0019) via constructor injection. It does not include ONNX Runtime headers.
- [ ] `StemPlaybackEngine` is instantiated per deck and registered with the audio engine as an alternative audio source. It exposes the four `std::atomic<float>` gain values and the stem buffer pointers.
- [ ] `StemControlComponent` reads the deck's ValueTree stem subtree via Listeners and dispatches mute/solo/separate commands through a command interface — it does not directly access the inference runtime or audio engine.

## 1.5. Grey Areas

1. **BS-RoFormer segment size and overlap ratio.** The `ep_317_sdr_12.9755` checkpoint was trained with a specific segment length and overlap configuration. The values specified in this PRD (352,800 samples / 8 seconds, 25% overlap) are based on common BS-RoFormer training configurations and published reference implementations. During implementation, the exact values must be validated against the exported ONNX model's input shape metadata (the first dimension after batch and channels). If the model's expected input length differs, the pipeline's segment size must be adjusted to match. The overlap ratio (25%) is a conservative default that balances reconstruction quality against inference count — lower overlap (e.g., 12.5%) would halve inference time but may introduce audible seams at segment boundaries. Profiling with real music material during implementation will determine the optimal ratio. The segment size and overlap are parameterized in the pipeline, not hardcoded, to accommodate model changes.

2. **Stem count and assignment.** The checkpoint name `model_bs_roformer_ep_317_sdr_12.9755` does not encode the stem count in its filename. BS-RoFormer models exist in both 4-stem (vocals, drums, bass, other) and 2-stem (vocals, instrumental) variants. This PRD assumes 4 stems based on the Epic's specification and the model's expected output tensor shape `[1, 4, 2, N]`. During implementation, the actual output shape must be verified after ONNX model loading via `getOutputTensorInfo()` (PRD-0019). If the model outputs 2 stems, the pipeline adapts by mapping stem 0 to "Vocals" and stem 1 to "Instrumental", and the UI displays two buttons instead of four. The stem count is derived from model metadata, not hardcoded.

3. **Playback architecture: per-stem buffers vs. pre-mixed buffer.** Two architectures were considered: (A) the audio engine reads four separate stem buffers and mixes them per sample, or (B) a background thread pre-mixes the stems based on current mute/solo state into a single buffer that the audio engine reads. Option A is chosen because it provides truly instantaneous mute/solo response (within one audio callback, ~3 ms at 128 samples / 44.1 kHz) with no background thread coordination. The CPU cost of reading four buffers instead of one is negligible — four multiply-accumulate operations per sample per deck vs. one read. Option B would introduce latency equal to the pre-mix buffer size and add thread synchronization complexity. The only downside of Option A is 4x memory usage per deck, which is acceptable given modern systems (840 MB worst case across 4 decks with 5-minute tracks).

4. **When to trigger separation: automatic vs. manual.** Automatic separation on track load was considered but rejected for several reasons: (a) inference is expensive and most tracks loaded during a set may not need stems, wasting CPU/GPU and power; (b) auto-separation would compete with the user-initiated separation on another deck, doubling queue times; (c) the DJ should have explicit control over when their system's resources are consumed. A future Preferences PRD may add an "auto-separate on load" toggle for DJs who prefer proactive separation, but the default is manual trigger. Cache hits make re-separation unnecessary for previously processed tracks, mitigating the manual trigger's friction.

5. **Cache storage size and eviction policy.** Four 32-bit float WAV stems for a 5-minute track consume approximately 420 MB on disk (4 stems * 5 min * 60 s * 44100 Hz * 2 ch * 4 bytes). A DJ's library of 500 separated tracks would require ~200 GB. The 50 GB default cache limit means approximately 60 tracks can be cached before eviction begins. LRU eviction prioritizes recently played tracks. The cache limit is configurable in a future Preferences PRD. An alternative considered was lossy compression (FLAC for stems), which would reduce cache size by 40-60%, but adds decode latency on cache read and risks introducing artifacts into already-processed audio. Lossless 32-bit float WAV is chosen for fidelity; disk space is cheap relative to audio quality for professional use.

6. **Handling device sample rate mismatch.** The BS-RoFormer model expects 44.1 kHz input. If the audio device runs at 48 kHz (common for professional audio interfaces), the pipeline must resample down to 44.1 kHz before inference and resample back to 48 kHz after. This double resampling introduces a small quality loss. An alternative is to train or fine-tune the model at 48 kHz, but this is outside the scope of this PRD. The resampling step is isolated in the pipeline and can be bypassed if a 48 kHz model becomes available in the future. Cache files are always stored at 44.1 kHz (the model's native rate) and resampled to the device rate on load, avoiding duplicate cache entries for different device configurations.

7. **Stem separation while track is playing.** Separation can proceed while the track is playing. The pipeline reads from the decoded PCM buffer, which is already fully in memory (PRD-0003 performs full-file decode). There is no contention with the audio thread because the audio thread reads the same buffer via a separate pointer and the buffer is immutable once decoded. The stems become available after separation completes, at which point the engine can switch to stem playback mode mid-song with the 64-sample crossfade. If the track reaches the end before separation finishes, the partial separation is still completed and cached for the next play.

8. **Phase coherence across stems.** The overlap-add reconstruction must preserve sample-accurate alignment across all four stems. If the stems drift by even a single sample, the re-summed mix will exhibit phase cancellation artifacts (comb filtering). The pipeline uses a single shared segment grid for all four stems — the same start/end sample indices and the same Hann window coefficients. Stems are never processed with independent segment boundaries. Phase coherence is validated by the acceptance criterion that requires the sum of all stems to approximate the original mix within -60 dBFS.

9. **Graceful degradation during low-memory conditions.** Loading four stem buffers (~210 MB per deck) on a memory-constrained system (e.g., 8 GB RAM laptop with 4 decks active) could trigger OS memory pressure. The stem system does not implement its own memory pressure detection in this PRD. If allocation fails (`std::bad_alloc`), the pipeline catches the exception, frees any partially allocated buffers, transitions `stems/status` to `"error"` with an appropriate message, and falls back to the original mixed audio. A future system-health PRD may add proactive memory monitoring that disables stem separation when available RAM drops below a threshold.