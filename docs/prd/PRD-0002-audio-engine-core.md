---
status: Not Implemented
epic: EPIC-0001
---

# 1. PRD-0002: Audio Engine Core

## 1.1. Problem

A professional DJ performing live depends entirely on hearing audio output from loaded tracks with zero interruptions. The audio engine is the runtime heart of Sonik: it takes decoded audio buffers from each deck, applies per-deck gain staging, sums the results into a single stereo output, and delivers them to the system audio device within a hard real-time deadline. If this engine does not exist, every other feature — transport controls, waveforms, EQ, effects — is inert UI with no sound behind it. If this engine is poorly designed, the DJ hears clicks, pops, dropouts, or silence — any of which is indistinguishable from a crash during a live set. The engine must also provide real-time metering data to the UI so the DJ can visually monitor signal levels, and it must handle runtime changes (adding or removing a deck, switching audio devices, varying buffer sizes) without interrupting playback on active decks.

## 1.2. Objective

The system provides a real-time audio engine core that:
- Accepts audio buffers from 1 to 4 independently playing decks and sums them into a single stereo output bus, delivered to the configured system audio device.
- Maintains end-to-end output latency at or below 5 ms (measured from the audio device callback request to the completed buffer handoff) at 44.1 kHz / 128-sample buffer size.
- Performs all audio-thread work (buffer mixing, gain staging, metering computation) with zero memory allocations, zero locks, and zero I/O — strictly obeying the real-time audio thread contract.
- Reads per-deck gain values atomically from the deck state tree (PRD-0001) and applies them as linear gain multipliers before summing.
- Publishes per-deck pre-fader peak and RMS metering data to the UI thread via a lock-free mechanism, updated every audio callback cycle.
- Handles deck addition and removal at runtime without audible artifacts on other active decks.
- Operates correctly across all sample rates supported by the audio device (44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, 176.4 kHz, 192 kHz) and buffer sizes from 32 to 2048 samples.
- Reports the current audio device configuration (sample rate, buffer size, measured output latency) to the state tree so the UI and other subsystems can read it.

## 1.3. User Flow

1. The user launches Sonik. The audio engine initializes with the OS default audio output device, a sample rate of 44.1 kHz, and a buffer size of 128 samples. No audio plays (all decks are in the Empty state). The engine begins running its audio callback, outputting silence.
2. The user loads a track onto Deck A and presses Play. The audio engine's `processBlock` reads Deck A's audio buffer, applies the deck's gain value (default 0 dB / unity gain), writes the result to the stereo output bus, and the user hears audio through their speakers or headphones.
3. The user adjusts Deck A's gain knob. The gain value updates in the deck state tree; the audio engine reads the new atomic value on the next callback cycle and applies the updated gain immediately. The user hears the volume change with no latency perceptible beyond the buffer size.
4. The user loads and plays Deck B simultaneously. The audio engine now reads buffers from both Deck A and Deck B, applies per-deck gain, sums the two signals, and writes the mixed result to the output bus. The user hears both tracks playing together.
5. The user observes the per-deck level meters in the UI. Each meter displays real-time peak and RMS values derived from pre-fader signal data published by the audio engine via lock-free communication. Meters update smoothly and reflect the actual audio signal.
6. The user adds Deck C via the "Add Deck" button while Decks A and B are playing. The audio engine begins including Deck C's output (silence, since no track is loaded yet) without any audible glitch on Decks A or B.
7. The user removes Deck C (stopped, no track loaded). The audio engine stops reading from Deck C's slot. Playback on Decks A and B continues uninterrupted.
8. The user opens the audio device settings and changes the buffer size from 128 to 256 samples. The audio engine reconfigures the device, briefly interrupting output (expected JUCE behavior on device reconfiguration), and resumes playback at the new buffer size.
9. The user's external audio interface is unplugged during playback. The audio engine detects the device disconnection, halts processing, and publishes an error state to the state tree. The UI displays a notification: "Audio device disconnected. Please reconnect or select a different device." Playback state on all decks is preserved (paused, not reset).
10. The user reconnects the audio device (or selects a different one). The engine reinitializes, resumes the audio callback, and playback continues from the preserved positions.

## 1.4. Acceptance Criteria

- [ ] The audio engine initializes a JUCE `AudioDeviceManager` on application startup with the OS default output device, 44.1 kHz sample rate, and 128-sample buffer size.
- [ ] The `processBlock` callback executes with zero memory allocations, zero mutex locks, and zero I/O operations — verified by code review and runtime assertions in debug builds.
- [ ] The engine reads audio buffers from up to 4 deck source slots and sums them into a single stereo output bus per callback cycle.
- [ ] Per-deck gain is read from `std::atomic<float>` values and applied as a linear multiplier to each deck's buffer before summing.
- [ ] Pre-fader peak and RMS metering values are computed per deck per callback and published to the UI thread via `std::atomic<float>` — no lock, no allocation.
- [ ] Adding a deck at runtime (while other decks are playing) introduces no audible artifact on existing decks; the new deck outputs silence until a track plays.
- [ ] Removing a deck at runtime introduces no audible artifact on remaining decks.
- [ ] The engine operates correctly at sample rates of 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, and 192 kHz without code changes.
- [ ] The engine operates correctly with buffer sizes from 32 to 2048 samples without code changes.
- [ ] At 44.1 kHz / 128 samples, end-to-end latency is at or below 5 ms as reported by `AudioDeviceManager`.
- [ ] The current audio device name, sample rate, buffer size, and output latency in milliseconds are published to the state tree and readable by the UI.
- [ ] If the audio device is disconnected or becomes unavailable, the engine publishes an error state to the state tree, preserves all deck playback positions (decks move to Paused state), and attempts to reinitialize the device on a timer (every 2 seconds, up to 5 attempts).
- [ ] If a new audio device is selected, the engine reinitializes and resumes output without requiring an application restart.
- [ ] The audio engine is instantiated with explicit dependency injection (AudioDeviceManager, state tree references passed via constructor) — no singletons or global state.
- [ ] A lightweight CPU load monitor measures `processBlock` execution time and publishes the load percentage to the state tree via `std::atomic<float>`. A warning flag is published if load exceeds 90% for 3 consecutive callbacks.
- [ ] A hard clip safety net (`juce::jlimit(-1.0f, 1.0f, sample)`) is applied to the final output bus to prevent digital clipping.
- [ ] Deck audio source slots use a fixed-size `std::array` of atomic pointers (size 4), initialized to `nullptr`. Adding/removing a deck atomically swaps a pointer — zero allocation on the audio thread.
- [ ] Sample rate conversion of loaded files is NOT handled by this PRD — the engine assumes all deck buffers arrive at the device sample rate.
- [ ] Headphone/cue output (PFL) is NOT implemented in this PRD — the engine delivers a single stereo master bus only.
- [ ] Audio device selection UI is deferred to the Preferences Epic; for MVP the engine uses OS default device with hardcoded defaults.
- [ ] All audio engine code resides under `Source/Features/AudioEngine/`.