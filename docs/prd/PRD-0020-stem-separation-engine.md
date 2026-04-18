---
status: Not Implemented
epic: EPIC-0002
depends-on:
  - PRD-0019
  - PRD-0003
---

# 1. PRD-0020: Stem Separation Engine

## 1.1. Problem

A DJ performing live may want to play only the vocals (acapella) or only the instrumental of a track — for creative transitions, mashups, or isolating elements during a mix. Today, this requires pre-prepared stem files created externally in tools like iZotope RX or command-line Demucs, which is a time-consuming offline workflow that breaks the creative flow.

The user needs Sonik to separate any loaded track into individual stems directly within the application. The separation must happen in the background without interrupting current playback, must report progress so the user knows how long to wait, and must cache results so a track is never re-processed unnecessarily. The output must be high-quality audio files that the playback engine (PRD-0021) can load and mix in real-time.

Without this engine, the stem separation buttons and toggles in the UI are inert — there is no backend capable of producing the stem data.

## 1.2. Objective

The system provides a stem separation engine that:
- Accepts a decoded audio buffer (from a loaded track) and produces 4 separate stem audio buffers: vocals, drums, bass, and other.
- Runs the `htdemucs` ONNX model via the inference session provided by PRD-0019, executing entirely on a dedicated background thread.
- Reports granular progress (0-100%) and estimated time remaining to the Stems ValueTree node, updated at least once per second during processing.
- Supports cooperative cancellation: the user can abort a running separation at any time, and partial output is cleaned up.
- Exports the 4 stem outputs as 32-bit float WAV files to a persistent cache directory, keyed by the track's content hash and the model version.
- Checks the cache before starting separation: if valid cached stems exist for the track, delivers them immediately without re-processing.
- Persists cache metadata in SQLite (`stems_data` table) for fast lookup and LRU eviction.
- Handles errors gracefully: model failures, disk write failures, and out-of-memory conditions are reported to the Stems node without crashing.

## 1.3. User Flow

1. The user has loaded a track onto Deck A. The track is fully decoded and its `AudioBufferHolder` is available in memory. The Stems node status is `"none"`.
2. **Automatic cache check on track load**: immediately after the track is decoded and metadata is available, the `StemSeparationManager` checks the SQLite `stems_data` table using the track's content hash. **Cache hit**: the manager verifies that all 4 WAV files exist on disk, sets the Stems node status to `"loading_cached"`, and dispatches a background job to load the cached WAVs into memory. Once loaded, the status transitions to `"ready"`. The user sees the stem toggles become enabled automatically — no button click required. Skip to step 8.
3. **Cache miss**: the Stems node status remains `"none"`. The user must click the "Separate Stems" button (PRD-0023 UI) to begin separation.
4. The manager sets the Stems node status to `"separating"` and progress to `0.0`. If the dedicated separation thread is already processing a job for another deck, the status is set to `"queued"` instead and the job waits until the thread becomes available. The UI shows the appropriate state (progress indicator or "queued" indicator).
5. The separation engine runs on its dedicated background thread. It performs preprocessing (resampling to the model's expected sample rate if needed, STFT), feeds chunks to the ONNX model, collects output, and performs postprocessing (iSTFT, resampling back to the device sample rate if needed). Progress updates are posted to the message thread via `callAsync`, updating the Stems node `progress` property (0.0 to 1.0) approximately every second.
6. Once inference completes, the engine writes 4 WAV files (`vocals.wav`, `drums.wav`, `bass.wav`, `other.wav`) to `~/Library/Caches/Sonik/Stems/<content_hash>/`. It then inserts a record into the `stems_data` SQLite table with the content hash, model version, stem file paths, timestamp, and total file size.
7. The engine posts the result to the message thread. The manager transitions the Stems node status to `"ready"` and delivers the 4 `AudioBufferHolder::Ptr` stem buffers to the audio engine (PRD-0021).
8. The stem toggles (VOC, INST) are now enabled. The user can mute/unmute stems in real-time.
9. **Cancellation**: at any point during step 5, the user clicks "Cancel" (or loads a different track). The separation job checks `shouldExit()` at each major stage boundary and after each inference chunk. If cancellation is detected, the job aborts, cleans up any partial WAV files, and the Stems node status transitions to `"none"`. No partial or corrupt data is left on disk.
10. **Error**: if inference fails (model error, OOM, disk full), the job posts an error message via `callAsync`, setting Stems node status to `"error"` and a descriptive `stemError` property. The deck continues playing the original track normally.
11. **Track unload**: if the user ejects the track while separation is in progress, the separation job is cancelled (same as step 9). If stems were already loaded, the stem buffers are released.

## 1.4. Acceptance Criteria

- [ ] The stem separation engine accepts an `AudioBufferHolder::Ptr` (decoded PCM) and a content hash, and produces 4 stem `AudioBufferHolder::Ptr` outputs (vocals, drums, bass, other).
- [ ] Separation runs on a dedicated `juce::ThreadPool { 1 }` — not shared with the `AudioFileLoader` pool or the beatgrid/key analyzers.
- [ ] The separation job is a `juce::ThreadPoolJob` subclass following the established codebase pattern (pool takes ownership, cooperative `shouldExit()` cancellation).
- [ ] Progress is reported to the Stems ValueTree node via `juce::MessageManager::callAsync()`, updating `IDs::progress` (float 0.0 to 1.0) at least once per second during processing.
- [ ] The Stems node `status` property transitions through: `"none"` → `"queued"` → `"separating"` → `"ready"` (on success with queue), or `"none"` → `"separating"` → `"ready"` (on success, no queue), or `"none"` → `"separating"` → `"error"` (on failure), or `"none"` → `"loading_cached"` → `"ready"` (on cache hit). The `"queued"` state is used when another deck's separation is already in progress.
- [ ] The automatic cache check runs on the message thread during track load (after decode completes). If a cache hit is found, stems are loaded automatically without user action. If no cache exists, the Stems node remains at `"none"` and the user must manually trigger separation.
- [ ] On cache hit: the `stems_data` SQLite table is queried by content hash; if a matching record exists and all 4 WAV files are present on disk, the engine skips inference and loads the cached files.
- [ ] On cache miss: after successful separation, 4 WAV files (32-bit float, matching the device sample rate) are written to `~/Library/Caches/Sonik/Stems/<content_hash>/`.
- [ ] A record is inserted into `stems_data` with columns: `content_hash TEXT PRIMARY KEY`, `model_version TEXT`, `vocal_path TEXT`, `drums_path TEXT`, `bass_path TEXT`, `other_path TEXT`, `created_at INTEGER`, `file_size_bytes INTEGER`.
- [ ] Cache eviction: when total cache size exceeds a configurable limit (default 10 GB), the oldest entries by `created_at` are deleted (both DB records and disk files) until the cache is under the limit. Eviction must skip entries whose `content_hash` matches any currently-loaded deck's track to prevent deleting stems that are actively in use.
- [ ] Cancellation: calling `cancelSeparation(deckId)` signals the running job to exit. The job cleans up any partially written WAV files before finishing. Stems node status returns to `"none"`.
- [ ] Cancellation is cooperative: `shouldExit()` is checked after each STFT chunk, after each inference batch, and after each iSTFT chunk — at minimum every ~500 ms of wall-clock time.
- [ ] Error handling: if ONNX `Run()` throws or returns an error, the exception is caught within the job, the Stems node status is set to `"error"`, and `stemError` is set to a human-readable description. The application does not crash.
- [ ] Error handling: if WAV file writing fails (disk full, permissions), the error is reported the same way and partial files are deleted.
- [ ] If a separation is already in progress for a deck and a new separation is requested (e.g., track changed), the previous job is cancelled before starting the new one.
- [ ] The STFT parameters (window size, hop size, FFT size, window function) match the `htdemucs` model's training configuration exactly to avoid quality degradation.
- [ ] If the source audio sample rate differs from the model's expected rate (44100 Hz), the engine resamples before inference and resamples the output back to the device rate after separation.
- [ ] The `StemSeparationManager` is constructed with explicit dependency injection: `TrackDatabase&`, `OnnxInference&` (from PRD-0019), and a reference to the Stems ValueTree node.
- [ ] Track-level cache check happens on the message thread (fast DB query). Only the actual separation and file I/O run on the background thread.
- [ ] After separation, stem buffers delivered to the audio engine are `AudioBufferHolder::Ptr` (ref-counted), matching the existing buffer delivery pattern.
- [ ] The `stems_data` table is created in `TrackDatabase::createTables()` following the existing `INSERT OR REPLACE` upsert pattern.
- [ ] All new code resides under `Source/Features/StemSeparation/` following Feature-Sliced Design.
- [ ] Loading cached WAV files from disk uses the existing `juce::AudioFormatManager` and `WavAudioFormat` reader — no custom file parsing.
- [ ] The separation engine does NOT modify the audio engine's playback state — it only produces buffers and updates the ValueTree. Playback integration is deferred to PRD-0021.
- [ ] On application startup, a cleanup routine scans `~/Library/Caches/Sonik/Stems/` for subdirectories whose `content_hash` has no matching row in the `stems_data` table and deletes them, preventing orphan files from accumulating after crashes or interrupted writes.
- [ ] Tracks shorter than 5 seconds are rejected for separation: the manager sets Stems node status to `"none"` and does not enqueue a job. PRD-0023 is responsible for disabling the STEMS button with a tooltip for short tracks.
- [ ] The DB record is inserted with a `"pending"` status before WAV files are written, and updated to `"complete"` after all 4 files are successfully written. On startup cleanup, `"pending"` entries are treated as incomplete and their associated files are deleted.