---
status: Implemented
epic: EPIC-0001
---

# 1. PRD-0004: Transport System

## 1.1. Problem

After loading and decoding a track (PRD-0003) and initializing the audio engine (PRD-0002), the DJ still has no way to actually hear, control, or navigate the loaded audio. The audio engine's `processBlock` callback runs every cycle but has no mechanism to determine which samples to read from the decoded buffer, how to advance through the track over time, or how to respond to user commands like play, pause, or seek. Without a transport system, every downstream feature — waveform scrolling, beat-syncing, cueing, looping — has no reference point in time.

A professional DJ performing live needs three guarantees from transport controls. First, instantaneous response: pressing Play must produce audio within a single buffer cycle (under 3 ms at 128 samples / 44.1 kHz), and pressing Pause must silence output just as quickly. Second, zero artifacts: every state transition (play, pause, stop, seek, cue return) must produce no audible click, pop, or discontinuity. Third, sample-accurate positioning: the playhead must track the exact sample being output, with no drift or rounding error over a 10-minute track, so that the waveform display, the elapsed/remaining time readout, and the beat-phase indicator all agree with what the DJ hears.

## 1.2. Objective

The system provides a sample-accurate, real-time transport engine for each deck that:
- Advances the playhead through the decoded audio buffer at the correct rate inside `processBlock`, with zero allocations, zero locks, and zero I/O on the audio thread.
- Responds to Play, Pause, Stop, Seek, and CUE commands issued from the UI thread via lock-free communication, with the command taking effect on the next `processBlock` cycle.
- Applies a short crossfade ramp (64 samples) on every state transition that starts or stops audio output to eliminate clicks and discontinuities.
- Implements CDJ-standard CUE button behavior: set temporary cue point, return to cue, and hold-to-preview with play-through on Play press during preview.
- Reads a `speedMultiplier` value (default 1.0) from the deck state atomically, applying it to the playhead advancement rate with sub-sample precision.
- Publishes the current playhead position (in samples) to the UI thread via `std::atomic<int64_t>`, updated every `processBlock` cycle.
- Handles end-of-track by transitioning the deck to Stopped state with a fade-out ramp, leaving the playhead at the final sample position.
- Handles Stop by transitioning to Stopped state with a fade-out ramp and resetting the playhead to position 0.

## 1.3. User Flow

1. The user has Deck A in the Stopped state with a track loaded. The playhead is at sample position 0. The UI displays elapsed time 0:00 and remaining time equal to the track's full duration.
2. The user presses Play. On the next `processBlock` cycle, the transport transitions to Playing state and begins outputting audio from sample position 0 with a 64-sample fade-in ramp. The playhead advances by `bufferSize * speedMultiplier` samples each cycle.
3. The user watches the elapsed time and waveform position update smoothly. The UI reads the atomic playhead position at its display refresh rate (~60 Hz) and updates the elapsed/remaining time and waveform scroll.
4. The user presses Pause. The transport applies a 64-sample fade-out ramp, then holds the playhead at the position where the ramp completed. The deck transitions to Paused.
5. The user presses Play again. Playback resumes from the paused position with a 64-sample fade-in.
6. The user presses Stop. The transport applies a 64-sample fade-out, transitions to Stopped, and resets the playhead to sample position 0. The UI jumps elapsed time back to 0:00.
7. The user seeks to a specific position via needle-drop or waveform click. If Playing, the transport fades out at the old position, jumps to the target, and fades in at the new position. If Paused or Stopped, the playhead jumps instantly with no fade.
8. The user presses CUE while Paused and the playhead is not at the temp cue position. The transport sets the temp cue point to the current playhead position.
9. The user presses Play and then presses CUE during playback. The transport fades out, jumps to the temp cue point, and transitions to Paused.
10. The user holds CUE while Paused at the temp cue position. The transport begins cue preview playback from the temp cue point.
11. The user releases CUE without pressing Play. The transport fades out, returns the playhead to the temp cue position, and pauses.
12. The user holds CUE (cue preview begins), then presses Play while still holding CUE. Playback continues uninterrupted — the cue preview clears and normal Playing state resumes.
13. The track reaches the end of the decoded buffer. The transport applies a fade-out ramp, transitions to Stopped, and holds the playhead at the final sample position.
14. The pitch fader (future PRD) updates `speedMultiplier` to 1.06. The transport advances the playhead by `bufferSize * 1.06` samples per cycle with sub-sample precision via a double accumulator.

## 1.4. Acceptance Criteria

- [ ] Play, Pause, Stop, and Seek commands are delivered from the UI thread to the audio thread via a lock-free mechanism with zero allocations and zero locks on the audio thread.
- [ ] A Play command results in audio output beginning within a single `processBlock` cycle after the command is written.
- [ ] A Pause command halts playhead advancement and audio output within a single `processBlock` cycle, with the playhead holding at the fade-out completion position.
- [ ] A Stop command halts audio output within a single `processBlock` cycle and resets the playhead to sample position 0.
- [ ] A Seek command moves the playhead to the exact target sample position within a single `processBlock` cycle, clamped to [0, totalSamples - 1].
- [ ] Every transition that starts audio output applies a linear fade-in ramp of 64 samples.
- [ ] Every transition that stops audio output applies a linear fade-out ramp of 64 samples.
- [ ] Seeking during playback fades out at the old position and fades in at the new position with no audible click.
- [ ] Seeking during Paused or Stopped state moves the playhead instantly with no fade.
- [ ] CUE sets a temp cue point at the current playhead position when pressed in Paused state (and playhead is not already at the temp cue).
- [ ] CUE returns to the temp cue point and pauses when pressed during Playing state.
- [ ] Holding CUE at the temp cue position (Paused state) plays audio from the temp cue point (cue preview mode).
- [ ] Releasing CUE during cue preview returns the playhead to the temp cue point and pauses.
- [ ] Pressing Play while holding CUE during cue preview continues playback without returning to the cue point on CUE release.
- [ ] The temp cue point defaults to sample position 0 on track load and is not persisted in the database.
- [ ] The playhead position is published via `std::atomic<int64_t>` (unit: samples), updated at the end of every `processBlock` cycle.
- [ ] The UI derives elapsed time as `playheadPosition / sampleRate` and remaining time as `(totalSamples - playheadPosition) / sampleRate`.
- [ ] A `speedMultiplier` is read from `std::atomic<float>` (default 1.0) and applied to playhead advancement rate.
- [ ] Sub-sample playhead position is tracked using a `double` accumulator to prevent drift. Integer sample index is derived by truncation for buffer reads.
- [ ] When `speedMultiplier` != 1.0, the transport performs linear interpolation between adjacent samples to produce clean output.
- [ ] At end-of-track, the transport fades out, transitions to Stopped, and holds the playhead at the final sample position (not reset to 0).
- [ ] If fewer than 64 samples remain before end-of-track, the fade-out ramp equals the remaining sample count.
- [ ] If the deck buffer pointer is null (Empty state), `processBlock` outputs silence with no processing.
- [ ] All transport state is per-deck. Multiple decks operate independently with zero cross-talk.
- [ ] The transport performs zero memory allocations, zero mutex locks, and zero I/O on the audio thread.
- [ ] Reverse playback is not implemented in this PRD. `speedMultiplier` is clamped to non-negative values.
- [ ] Sync/master tempo integration is not implemented in this PRD. The `speedMultiplier` interface is established for future use.
- [ ] All transport code resides under `Source/Features/AudioEngine/`.