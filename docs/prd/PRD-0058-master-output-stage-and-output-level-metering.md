---
status: Implemented
epic: EPIC-0007
depends-on:
  - PRD-0002
  - PRD-0052
  - PRD-0053
  - PRD-0054
  - PRD-0055
  - PRD-0056
  - PRD-0057
---

# 1. PRD-0058: Master Output Stage and Output Level Metering

## 1.1. Problem

By the time the crossfader (PRD-0057) feeds its stereo bus into `MasterStage`, the mixer has channel gain, EQ, filter, fader, and A/B/crossfader curve all working — but the DJ has no master output level control and no visual confirmation of signal level anywhere in the chain. PRD-0002's hard-clip safety net still runs on the output bus, but it is a last-resort limiter, not a level meter: when it engages the DJ has no warning, no peak history, no clip indicator. A professional mixer must expose (a) a master gain knob with smooth, click-free DSP, and (b) per-channel post-fader stereo metering plus master post-master-gain stereo metering with peak, RMS, peak-hold, and a latched clip indicator. Without these, the DJ cannot manage gain staging, cannot see whether channels are headed toward clip, and cannot trust the mixer for a live set.

The metering subsystem is also the first place the audio thread must publish values back to the UI at a high refresh rate (60 Hz × 4 channels × 5 scalars + master = ~28 atomic writes per UI frame). PRD-0052 §1.5.4 explicitly carved out a `MixerMeterSnapshot` block of atomics specifically for this; this PRD is the first consumer/producer of that snapshot.

## 1.2. Objective

The system provides a complete master output stage and a full metering subsystem such that:

- `MasterStage` reads `mixer.master.gain` from the PRD-0052 atomic snapshot and applies it as a per-sample smoothed multiplier to the crossfader bus, before the PRD-0002 hard-clip safety net.
- The master gain range is -inf dB to +12 dB, with the same dB → linear mapping as channel gain (PRD-0054).
- Continuous master gain changes are smoothed with a 7 ms ramp; no zipper noise; no click on snap-to-value.
- Per-channel post-fader stereo metering computes peak (sample-accurate) and RMS (300 ms window) for left and right, and a clip latch that engages on any sample with `|x| >= 1.0` post-fader. Master metering does the same for the master output post-master-gain.
- All metering values are written to the `MixerMeterSnapshot` lock-free atomic block (PRD-0052) by the audio thread using `std::atomic::store(memory_order_relaxed)`. The UI polls the snapshot from a `juce::Timer` at 60 Hz; the UI work belongs to PRD-0059 (meter atom).
- Peak-hold: each peak meter sample latches the highest peak seen and decays it to the current peak over 1.5 s with a linear decay (see §1.5.4).
- Clip latch: once engaged, stays true for 3.0 s after the last clip sample, then auto-clears. Can be cleared manually by clicking the meter (UI behaviour, owned by PRD-0059); the underlying state is a single atomic bool with a sample-counted auto-clear in the audio thread.
- All audio-thread paths obey `AGENTS.md` (no allocations, no locks, no I/O; cross-thread only via `std::atomic`).

## 1.3. Developer / Integration Flow

1. `MasterStage::prepare(double sampleRate, int blockSize)` constructs a `juce::SmoothedValue<float>` for master gain (linear) with 7 ms ramp length, and pre-allocates the RMS rolling-window state for the master meter (a single ring buffer per stereo channel sized for 300 ms of samples at the configured sample rate). The same prepare hook initialises the channel meter state inside each `ChannelStripProcessor` (PRD-0053 already invokes `prepare` on every stage), allocating 4 channels × 2 ring buffers for RMS, and per-channel + master peak-hold state.
2. `MasterStage::process(crossfaderBus, outputBus)` runs after `CrossfaderStage` (PRD-0057). It loads `mixer.master.gain` from the atomic, calls `setTargetValue` on its smoother, then iterates samples: per sample, advance the smoother, multiply input by smoother output, write to `outputBus`. After the master-gain multiplication and before PRD-0002's hard-clip safety net, the master metering machinery samples every sample to update peak / RMS / clip latch state for the master bus.
3. `ChannelStripProcessor` is amended (by this PRD) to invoke the per-channel metering update on its post-fader output, immediately before handing the buffer to `ABBus::accumulate` (PRD-0057). The metering update is a separate inline function (`updateChannelMeter`) that reads each sample, updates running peak, advances the RMS ring, updates peak-hold, and latches clip. It writes results to the `MixerMeterSnapshot` slots for that channel.
4. Peak meter: for each sample, `peak = max(peak * peakDecayPerSample, |sample|)` where `peakDecayPerSample = exp(-1 / (sampleRate * 0.3))` (300 ms decay constant). Published to `levelPeakL` / `levelPeakR` once per block (the last value computed).
5. Peak-hold: a second value `peakHold` is updated as `peakHold = max(peakHold - peakHoldDecayPerSample, peak)`, where `peakHoldDecayPerSample = 1.0 / (sampleRate * 1.5)` (linear decay over 1.5 s; see §1.5.4). Published to a separate slot `levelPeakHoldL` / `levelPeakHoldR` added by this PRD to `MixerMeterSnapshot`.
6. RMS meter: a 300 ms ring buffer of squared samples accumulates with a running sum; `rms = sqrt(sumOfSquares / windowLengthSamples)`. Published once per block.
7. Clip latch: per sample, `clip = clip || (|sample| >= 1.0)`. A sample-counter `samplesSinceLastClip` advances every block; when `clip == true` and `samplesSinceLastClip > 3.0 * sampleRate`, `clip` is auto-reset to false. A manual clear from the UI writes `false` to the atomic; the audio thread's auto-clear logic respects whichever change is observed first (no race because the manual clear is a single atomic write).
8. UI meter components (PRD-0059) poll `MixerMeterSnapshot` at 60 Hz via `juce::Timer::startTimerHz(60)` and render. The audio thread updates the snapshot once per block; at 256-sample blocks @ 44.1 kHz the audio thread produces meter updates at ~172 Hz, well above the 60 Hz UI refresh. The UI samples a snapshot; no synchronisation beyond the atomic load is needed (each meter scalar is independently atomic; UI tolerates per-scalar tearing since no two scalars need to agree to within a single frame).
9. PRD-0002's hard-clip safety net continues to run on the `outputBus` after `MasterStage::process` completes and after master metering finishes. Metering observes the pre-hard-clip signal so the master clip indicator shows when the source signal would have clipped, not just when the safety net engaged.

## 1.4. Acceptance Criteria

- [ ] `MasterStage` reads `mixer.master.gain` from the PRD-0052 atomic and applies it as a per-sample smoothed linear multiplier with 7 ms ramp length. Range -inf dB … +12 dB; `dB <= -60` clamped to 0.0 linear.
- [ ] No zipper or click occurs at the master output when master gain steps by 24 dB instantaneously.
- [ ] `MasterStage::process` runs after `CrossfaderStage::process` (PRD-0057) and before the PRD-0002 hard-clip safety net.
- [ ] Per-channel metering computes, for left and right independently: instantaneous peak with 300 ms exponential decay, peak-hold with 1.5 s linear decay, RMS over a 300 ms rectangular window, and a clip latch that engages on any sample with `|sample| >= 1.0` and auto-clears 3.0 s after the last clip sample.
- [ ] Per-channel metering is sampled at the post-fader, pre-A/B-bus signal of `ChannelStripProcessor` (i.e. the channel's contribution to the crossfader, ignoring A/B assignment).
- [ ] Master metering computes the same five scalars (peak L/R, peakHold L/R, rms L/R, clip) on the post-master-gain, pre-hard-clip signal of `MasterStage`.
- [ ] Meter slots `levelPeakL`, `levelPeakR`, `levelPeakHoldL`, `levelPeakHoldR`, `levelRmsL`, `levelRmsR`, and `clip` are added to the `MixerMeterSnapshot` block (PRD-0052) for each channel and for the master, addressable by the dotted identifiers documented in PRD-0052 (`mixer.channel.{A,B,C,D}.levelPeakHoldL`, etc.). This PRD amends PRD-0052 to add `levelPeakHoldL` and `levelPeakHoldR` slots.
- [ ] Meter values are written by the audio thread exclusively via `std::atomic::store(memory_order_relaxed)` and read by UI components exclusively via `std::atomic::load(memory_order_relaxed)`.
- [ ] A manual clip-clear write (`false`) to a clip atomic from the message thread is honoured: the auto-clear sample counter is reset by the next audio-thread read so that the latch does not immediately re-fire from stale sample state. Auto-clear logic itself is single-thread (audio), so no race exists between the two clear paths beyond the well-defined atomic visibility.
- [ ] UI polling rate is 60 Hz (PRD-0059 owns the timer); the audio thread updates the snapshot once per `processBlock` call. The metering subsystem does not enforce any specific UI polling rate beyond exposing the atomics.
- [ ] All metering and master-gain audio-thread code paths perform no memory allocation, take no locks, perform no I/O. RMS ring buffers and peak-hold state are pre-allocated in `prepare` and never resized on the audio thread.
- [ ] At least one unit test in `Tests/` (e.g. `MasterStageMeteringTests.cpp`) verifies: (a) master-gain smoothing produces no click on a 24 dB step, (b) peak meter tracks a 1 kHz sine within 0.5 dB, (c) RMS meter tracks the same sine's RMS within 0.5 dB, (d) clip latches on a sample at 1.0 and auto-clears after exactly 3.0 s of silence, (e) peak-hold decays from 1.0 to 0.0 over exactly 1.5 s, (f) channel and master metering are independent (a clip on channel A does not latch master clip unless the summed signal also clips).
- [ ] No UI atom or organism is introduced by this PRD; the meter atom belongs to PRD-0059.

## 1.5. Grey Areas

### 1.5.1. RMS Window Length

Common choices range from 50 ms (fast, snappy, follows transients closely) through 300 ms (BS.1770 / loudness-standard "momentary" window, perceptually relevant for DJ work) to 3 s (long-term loudness). Too short and the RMS meter looks indistinguishable from the peak meter; too long and it lags transient changes the DJ is making in real time.

**Resolution:** 300 ms rectangular window. This matches the BS.1770 momentary loudness window, is what every reference DJ mixer's RMS meter does in practice, and is short enough that the meter responds within one beat at typical DJ tempos (120 BPM = 500 ms per beat). A rectangular window (rather than Hann or Gaussian) is the simplest, has the lowest CPU cost, and is what hardware meters implement. The implementation uses a running sum over a ring buffer of squared samples to avoid recomputing the full sum each sample.

### 1.5.2. Metering Tap Point: Pre-Fader, Post-Fader, or Both

A channel meter could tap pre-fader (shows the signal entering the strip), post-fader (shows what the channel contributes to the bus), or expose both. Pre-fader is useful for cueing levels before pulling the fader up; post-fader is what the DJ "hears." Hardware mixers vary.

**Resolution:** Post-fader only for this PRD. The Mixer Epic does not include cue / headphone monitoring (explicitly out of scope per EPIC-0007 §1.2.2), so pre-fader metering's primary use case (cue-level matching) is also out of scope. A post-fader meter shows the DJ what they are actually contributing to the mix, which is the single most useful piece of information during a live set. A future cue/headphone Epic can add a pre-fader meter as a sibling slot without breaking this PRD's contract.

### 1.5.3. Master Meter Tap Point: Pre- or Post-Hard-Clip

The master meter could tap the master signal before PRD-0002's hard-clip safety net (shows the actual signal envelope, including would-be-clipped peaks) or after (shows what reaches the device). Pre-clip is more informative; post-clip masks problems.

**Resolution:** Pre-hard-clip. The master meter taps the output of `MasterStage::process` before the hard-clip safety net runs. This means the clip latch fires on samples that would have clipped without the safety net, which is the correct signal to the DJ: "your mix is hitting +0 dBFS, pull the master back." Post-clip would silently hide every clip the safety net catches, which is the opposite of the meter's job. The cost is that the meter "lies" relative to what reaches the audio device by exactly the amount the hard-clip is reshaping — but the hard-clip is a safety net the DJ should never be relying on, so the meter showing "you're clipping" when the safety net is engaged is the desired behaviour.

### 1.5.4. Peak-Hold Decay Profile and Time

Three reasonable profiles: instantaneous hold then snap-down after a fixed time (hardware-classic, but visually choppy); exponential decay (smooth, but never quite reaches zero); linear decay (smooth, deterministic, reaches zero in known time). Decay time of 1.0 s is too fast for the DJ to see, 3.0 s feels stale.

**Resolution:** Linear decay over 1.5 s. The peak-hold value decreases by `1.0 / (sampleRate * 1.5)` per sample (in linear amplitude space) and is clamped to never fall below the current instantaneous peak (so a new peak resets the hold instantly upward). 1.5 s is long enough that a transient peak remains visible across roughly two beats at typical DJ tempos and short enough that the meter doesn't feel "stuck." Linear decay is preferred over exponential because it reaches zero in a known finite time, which avoids the visual "long tail" of an exponential that never quite leaves the meter. The state is one `std::atomic<float>` per channel per stereo side, written each block.

### 1.5.5. Clip Latch Duration and Manual Clear

The Epic notes "clip indicators that latch" without a duration. Hardware typically latches for 1–3 s; some software latches forever until clicked. Forever is annoying when a single transient clip leaves the indicator on for an entire set; too short and the DJ misses the warning.

**Resolution:** Auto-clear 3.0 s after the last clip-sample event, AND clickable to clear manually. The audio thread maintains a `samplesSinceLastClip` counter per channel and per master. On any sample with `|x| >= 1.0`, the counter resets to zero and the clip atomic is set true. Every block, if `clip == true` and `samplesSinceLastClip > 3.0 * sampleRate`, the audio thread writes false to the clip atomic. Manual clear from the UI (PRD-0059 owns the click handler) writes false directly. Both clear paths converge on a single atomic bool with no race: the audio thread is the sole reader of the counter, and the boolean's value resolves to whichever atomic write is observed last, which is the correct semantics ("clip happened recently → red; user acknowledged or time elapsed → off"). The 3.0 s value is large enough to give the DJ time to notice in a busy set, small enough to clear itself before becoming an annoyance.

### 1.5.6. Adding `levelPeakHold` Slots to PRD-0052

PRD-0052 declared `levelPeakL`, `levelPeakR`, `levelRmsL`, `levelRmsR`, and `clip` per channel and per master, but did not declare separate peak-hold slots. The naive option is to compute peak-hold in the UI from `levelPeak` history, but that requires the UI to maintain audio-rate-synchronous state, which is impossible at 60 Hz polling.

**Resolution:** Amend PRD-0052 to add `levelPeakHoldL` and `levelPeakHoldR` slots per channel and per master in `MixerMeterSnapshot`. This is a purely additive change (no removed slots, no renamed identifiers, no schema migration). The dotted identifier paths follow PRD-0052's pattern: `mixer.channel.{A,B,C,D}.levelPeakHoldL`, `mixer.master.levelPeakHoldR`, etc. UI components opt in by reading the additional slots; nothing in PRD-0052 or any earlier consumer breaks.
