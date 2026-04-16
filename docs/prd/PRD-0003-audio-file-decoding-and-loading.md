---
status: Implemented
epic: EPIC-0001
---

# 1. PRD-0003: Audio File Decoding and Loading

## 1.1. Problem

A professional DJ's entire workflow begins with getting audio into a deck. Before any mixing, beat-matching, cueing, or effects processing can happen, the software must open an audio file, decode its compressed or lossless data into raw PCM samples, and deliver those samples to the audio engine in a format it can play back instantly. If this process is slow, the DJ loses momentum between tracks during a live set. If it is unreliable, the DJ cannot trust the software with unfamiliar files from guest USB sticks or downloaded packs. If it blocks the UI or audio thread, the currently-playing track drops out — catastrophic during a performance. Every format quirk (variable bitrate MP3, hi-res 24-bit FLAC at 192 kHz, mono field recordings, files with malformed metadata) must be absorbed silently by the decoding layer so the DJ experiences a single, consistent interaction: drop a file, hear it ready to play.

## 1.2. Objective

The system provides a background audio file decoding and loading pipeline that:
- Accepts audio files via drag-and-drop (from the OS file system or the future Library) or programmatic load request, supporting MP3, FLAC, WAV, and AIFF formats at bit depths up to 32-bit float and sample rates up to 192 kHz.
- Decodes the entire file into a memory-resident PCM buffer on a background thread, completing in under 2 seconds for a typical 8-minute 320 kbps MP3 and under 5 seconds for a 10-minute 24-bit/96 kHz FLAC file.
- Resamples decoded audio to match the current audio device sample rate using high-quality interpolation, so the audio engine receives buffers it can play without further conversion.
- Converts all audio to stereo interleaved float format regardless of source channel count (mono summed to dual-mono, multi-channel downmixed to stereo).
- Delivers the decoded buffer to the audio engine's deck source slot via an atomic pointer swap — zero allocation, zero locking on the audio thread.
- Extracts file metadata (title, artist, album, duration, sample rate, bit depth, channel count, album art) from ID3v2, Vorbis comment, and AIFF chunk tags and publishes it to the deck's ValueTree state.
- Validates files before decoding begins, rejecting corrupted, DRM-protected, zero-length, or unsupported files with a specific, user-readable error message.
- Publishes loading progress (0 to 100%) to the deck state tree so the UI can display a progress indicator for large or hi-res files.
- Supports concurrent loading on up to 4 decks simultaneously without mutual interference, UI freezes, or audio dropouts on already-playing decks.

## 1.3. User Flow

1. The user has Deck A in the Empty state. They drag an audio file from Finder (macOS) or Explorer (Windows) onto Deck A's drop zone. The UI highlights the drop zone with a visual affordance (border glow or color change) confirming a valid drop target.
2. The user releases the file. The deck state transitions to a Loading sub-state: the UI displays the file name and a progress bar. The background decoding thread begins work.
3. The system validates the file: checks that the file exists, is readable, has a supported extension, and can be opened by JUCE's AudioFormatManager. If validation fails, the deck returns to Empty and a transient error message appears on the deck (e.g., "Unsupported file format" or "File is corrupted").
4. The system reads metadata from the file's tags (ID3v2 for MP3, Vorbis comments for FLAC, RIFF/AIFF chunks for WAV/AIFF). Title, artist, album, duration, sample rate, bit depth, and channel count are published to the deck state. Album art (if present) is extracted and cached for the UI to display.
5. The background thread decodes the full file into a contiguous float PCM buffer. Progress updates (0-100%) are published to the deck state at regular intervals. The UI reflects progress via the progress bar.
6. If the source sample rate differs from the device sample rate, the decoded buffer is resampled using high-quality interpolation. If the source is mono, it is duplicated to stereo. If the source is multi-channel (>2), it is downmixed to stereo.
7. Decoding completes. The background thread atomically swaps the new buffer pointer into the audio engine's deck source slot. The deck state transitions from Loading to Stopped. The playhead is at position 0. The progress bar disappears, replaced by the full track info display and waveform.
8. The user presses Play. Audio plays immediately from the memory-resident buffer — no further disk access is needed.
9. The user drags a different file onto Deck A while Deck B is playing. The system loads the new file on a background thread for Deck A. Deck B's playback continues without interruption. On Deck A, the previous buffer is released after the atomic pointer swap to the new buffer completes.
10. The user drags a corrupted MP3 onto Deck C. The system detects the error during decoding (truncated frames, invalid headers), aborts the load, transitions Deck C back to Empty, and displays an error: "File could not be decoded. It may be corrupted."
11. The user drags a DRM-protected file onto Deck A. The system rejects the file at the validation step with the message: "This file appears to be DRM-protected and cannot be loaded."
12. The user drags a very large file (~700 MB WAV) onto Deck D. A confirmation dialog appears: "This file is very large (X GB). Loading it will use significant memory. Continue?" If confirmed, the progress bar advances over several seconds. During this time, other decks remain fully operational.
13. The user loads a 24-bit/96 kHz FLAC file while the audio device is running at 44.1 kHz. The file decodes and is then resampled to 44.1 kHz. The deck metadata shows the original file's sample rate and bit depth for reference, while the audio buffer delivered to the engine is at the device sample rate.

## 1.4. Acceptance Criteria

- [ ] The system supports decoding MP3, FLAC, WAV, and AIFF files via JUCE `AudioFormatManager` with the appropriate format readers registered.
- [ ] Files with bit depths of 16, 24, and 32-bit (integer and float) decode correctly to 32-bit float PCM.
- [ ] Files with sample rates of 44.1, 48, 88.2, 96, 176.4, and 192 kHz decode correctly.
- [ ] The entire decoded file is held in a single contiguous memory buffer (full-file decode, not disk-streamed).
- [ ] All decoding and resampling work executes on a dedicated background thread — never on the UI thread or audio thread.
- [ ] The UI remains fully responsive (no frame drops below 60 fps) during file loading.
- [ ] Playback on other decks produces zero dropouts or audible artifacts during a concurrent file load.
- [ ] Decoded audio is resampled to the current audio device sample rate using JUCE's `LagrangeInterpolator` or equivalent high-quality interpolator when the source sample rate differs from the device rate.
- [ ] Mono files are converted to dual-mono (identical left and right channels).
- [ ] Multi-channel files (>2 channels) are downmixed to stereo using a simplified ITU-R BS.775 downmix.
- [ ] The decoded buffer is delivered to the audio engine's deck source slot via an atomic pointer swap — zero allocations, zero locks, zero I/O on the audio thread.
- [ ] Drag-and-drop from the OS file system (Finder/Explorer) onto a deck's drop zone triggers the load pipeline.
- [ ] Drag-and-drop from the future Library panel onto a deck's drop zone triggers the same load pipeline (interface parity via a single `LoadRequest` abstraction).
- [ ] The deck state transitions from Empty to Loading on drop, from Loading to Stopped on success, and from Loading to Empty on failure.
- [ ] A loading progress value (0-100%) is published to the deck's ValueTree and updated at least 10 times per second during decoding. The UI only renders the progress bar if decoding takes longer than 200 ms.
- [ ] Metadata fields (title, artist, album, duration in seconds, source sample rate, source bit depth, channel count) are extracted and published to the deck state before decoding completes.
- [ ] Album art is extracted from embedded tags (ID3v2 APIC, Vorbis METADATA_BLOCK_PICTURE, AIFF ID3 chunk), resized to 256x256 thumbnail, and cached in an LRU memory cache (max 200 entries, keyed by content hash).
- [ ] If no album art is embedded, the deck state reflects a null/empty art field and the UI shows a default placeholder.
- [ ] Invalid, corrupted, zero-length, or unreadable files are rejected with a specific error code and human-readable message published to the deck state.
- [ ] Unsupported formats (AAC, OGG without FLAC, WMA, DRM-protected files) are rejected at validation with a clear error message.
- [ ] Loading a new track into a deck that already has a track loaded cancels any in-progress decode for that deck, releases the previous buffer, and begins the new load.
- [ ] Ejecting a deck or removing a deck while a decode is in progress cancels the decode and frees all associated resources.
- [ ] A thread pool with a maximum of 2 concurrent decode threads is used. Additional load requests are queued; queued decks show an "In queue..." indicator.
- [ ] The previous audio buffer is freed only after the atomic pointer swap completes — no dangling pointer risk on the audio thread.
- [ ] A file size soft limit of 2 GB is enforced; files exceeding this trigger a confirmation dialog before loading.
- [ ] After a successful full decode, the system does not maintain an open file handle to the source file.
- [ ] All file decoding and loading code resides under `Source/Features/AudioEngine/`.