---
name: EPIC-0001: Deck / CDJ System
status: Open
---

# 1. EPIC-0001: Deck / CDJ System

## 1.1. Goal and Vision

Build a professional-grade, multi-deck playback system comparable to Pioneer CDJ-3000 and Traktor Pro's deck architecture. The system provides 1 to 4 independently controllable decks with full audio playback, real-time DSP processing, beat-aware features, and AI-powered stem separation. This is the foundational Epic of the Sonik application: every future feature area (Mixer, Effects, Library) depends on a fully functional deck.

The end-user experience targets professional DJs who expect sub-5ms latency, sample-accurate cueing, and a fluid UI that adapts seamlessly as decks are added or removed during a live session.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:
- Dynamic deck layout supporting 1 to 4 decks with adaptive grid (1 full-width, 2 side-by-side, 3 as 2+1, 4 as 2x2 grid)
- Add/remove deck controls with safety rules (no deletion while playing, minimum 1 deck)
- Drag-and-drop audio file loading (MP3, FLAC, WAV, AIFF, hi-res 24/32-bit up to 192kHz)
- Play, Pause, and transport controls
- Dual waveform display (full-track overview + scrolling detail view with zoom)
- Track info HUD (title, artist, BPM, key, elapsed/remaining time, album art)
- Pitch fader with selectable ranges (+-4%, +-8%, +-16%, +-50%)
- Per-deck gain/trim knob
- Time stretching with pitch-lock toggle (key lock on/off)
- Cue points and hot cues (8 pads, color-coded, persistent)
- Quantize mode (beat-snap for cues, loops, jumps)
- Looping system: manual in/out, auto-loop (1/2, 1, 2, 4, 8, 16, 32 beats), loop halve/double
- Beat jump (1/2, 1, 2, 4, 8, 16, 32 bars forward/backward)
- Needle drop / waveform click-to-seek
- Slip mode (silent timeline continuation during loops/scratch/reverse)
- Virtual jog wheel (scratch, pitch-bend nudge, track scrubbing)
- Stem separation: vocal/instrumental isolation using BS-RoFormer (`model_bs_roformer_ep_317_sdr_12.9755.ckpt`) with per-stem mute/solo buttons

Foundational systems (non-user-facing):
- Deck state management via `juce::ValueTree`
- Real-time audio engine core with per-deck DSP chain
- Audio file decoding, disk-streaming, and buffer management
- Sample-accurate transport/playhead system
- BPM detection and beatgrid analysis engine
- Musical key detection (Camelot/Open Key notation)
- Waveform peak/RMS analysis pipeline (background thread)
- ML inference runtime (ONNX Runtime) for stem separation model

### 1.2.2. Out of Scope

- Mixer (crossfader, channel EQ, channel faders) - separate Epic
- Audio effects (reverb, delay, flanger, etc.) - separate Epic
- Track library, search, playlists, and database - separate Epic
- MIDI controller mapping - separate Epic
- Recording / broadcast output - separate Epic
- Preferences / settings UI - separate Epic
- Networking / streaming input - separate Epic

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Audio Thread Safety

All code executing in `processBlock` must be strictly real-time safe:
- No memory allocation (`new`, `delete`, `malloc`, `std::vector::push_back`)
- No locks or blocking (`std::mutex`, `std::lock_guard`)
- No I/O (`std::cout`, file reads/writes, network)
- Cross-thread communication exclusively via `std::atomic` and lock-free structures (`juce::AbstractFifo`)

### 1.3.2. State Architecture

- `juce::ValueTree` as single source of truth for all deck state (count, playback status, position, cue points, loop state, beatgrid, key, loaded file metadata)
- Observer pattern via JUCE Listeners: UI reacts to state changes, never polls
- Dependency injection: no singletons, all dependencies passed via constructors

### 1.3.3. File Structure (Feature-Sliced Design)

Each PRD produces code organized under its feature slice:
- `Source/Features/Deck/` - Deck state, layout, lifecycle
- `Source/Features/AudioEngine/` - Core engine, decoding, transport
- `Source/Features/Waveform/` - Analysis pipeline, waveform UI components
- `Source/Features/BeatGrid/` - BPM detection, beatgrid data
- `Source/Features/Cue/` - Cue points, hot cues
- `Source/Features/Loop/` - Looping system
- `Source/Features/TimeStretch/` - Rubberband integration, pitch control
- `Source/Features/Stems/` - ML runtime, BS-RoFormer inference, stem separation

### 1.3.4. DSP & ML Dependencies

- **Rubberband** for time stretching (pitch-lock mode)
- **ONNX Runtime** for BS-RoFormer model inference
- **JUCE AudioFormatManager** for codec support (MP3, FLAC, WAV, AIFF)
- Pre-computed stem cache (separated audio written to disk after background processing)

## 1.4. PRD Roadmap

- [ ] PRD-0001: Deck State Management
- [ ] PRD-0002: Audio Engine Core
- [ ] PRD-0003: Audio File Decoding & Loading
- [ ] PRD-0004: Transport System (Play, Pause, Seek, Playhead)
- [ ] PRD-0005: Deck UI Shell & Dynamic Layout
- [ ] PRD-0006: Waveform Analysis & Display
- [ ] PRD-0007: Track Info Display
- [ ] PRD-0008: BPM & Beatgrid Analysis
- [ ] PRD-0009: Key Detection
- [ ] PRD-0010: Pitch Fader & Gain Control
- [ ] PRD-0011: Time Stretching
- [ ] PRD-0012: Cue Points & Hot Cues
- [ ] PRD-0013: Quantize Mode
- [ ] PRD-0014: Looping System
- [ ] PRD-0015: Beat Jump
- [ ] PRD-0016: Needle Drop & Waveform Seeking
- [ ] PRD-0017: Slip Mode
- [ ] PRD-0018: Jog Wheel
- [ ] PRD-0019: ML Inference Runtime
- [ ] PRD-0020: Stem Separation