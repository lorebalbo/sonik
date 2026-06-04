---
status: Implemented
epic: EPIC-0007
depends-on:
  - PRD-0052
  - PRD-0053
  - PRD-0054
---

# 1. PRD-0055: 3-Band EQ DSP and Per-Band Kill

## 1.1. Problem

EPIC-0007 §1.2.1 places a 3-band EQ stage with per-band kills between the channel gain (PRD-0054) and the per-channel filter (PRD-0056). The state schema (PRD-0052) already exposes `mixer.channel.{A,B,C,D}.eq.{high,mid,low}` (dB, range -inf … +6, full CCW = true cut) and `mixer.channel.{A,B,C,D}.eq.kill{High,Mid,Low}` (bool, latched cut), and the channel strip skeleton from PRD-0053 reserves the pass-through slot they will occupy. No DSP yet exists. Without a real EQ, every later UI / MIDI / metering PRD that depends on a working EQ is blocked, and PRD-0056's filter is left as the only frequency-shaping tool — which contradicts every reference DJ mixer (Pioneer DJM, Allen & Heath Xone, Behringer DDM4000) and removes the most-used DJ technique (bass swap on transitions).

A naive shelf-and-peak topology with naive parameter writes will produce two failure modes a DJ will immediately reject: (a) zipper / step noise when sweeping a band knob during a transition, and (b) clicks when latching a band kill from 0 dB to -inf and back. Both must be suppressed by per-sample smoothing, and the kill must not interfere with the underlying knob value the user set.

## 1.2. Objective

The system provides, for every mixer channel, a real 3-band EQ DSP stage inside `ChannelStripProcessor` such that:

- Each of the three bands reads its corresponding `eq.{high,mid,low}` value from the PRD-0052 atomic snapshot and applies a biquad filter to the channel's stereo signal.
- The three filter shapes are: low shelf centred at ~250 Hz (low band), peak / parametric centred at ~1 kHz with Q ≈ 0.7 (mid band), high shelf centred at ~2.5 kHz (high band). Crossover regions are smooth and produce no audible peak at unity (all knobs at 0 dB).
- Each band's effective gain range is -inf dB to +6 dB: full CCW (dB ≤ -60) cuts the band to silence ("true kill"); full CW boosts by +6 dB.
- Per-band kill latches (`eq.killHigh|Mid|Low`) ramp the band to -inf dB (true cut) over a 5–10 ms smoothed transition when latched, and ramp back to the user's last knob value when released, without overwriting the knob value.
- All parameter changes (knob sweep, kill latch, MIDI input via PRD-0044) are smoothed per-sample with a 7 ms time constant for knob sweeps and a fixed 8 ms ramp for kill transitions (see §1.5.4).
- Filter coefficients are recomputed only when a band's effective gain changes by more than `kCoefficientUpdateThresholdDb = 0.05` dB, and the recomputation publishes the new coefficients atomically (single-writer, single-reader pattern on the audio thread, see §1.5.5).
- The EQ stage sits between the channel gain (PRD-0054) and the filter (PRD-0056), preserving the EPIC-0007 §1.2.1 signal flow.
- All audio-thread code paths obey `CLAUDE.md`: no allocations, no locks, no I/O.
- No new user-facing parameter beyond the six already declared in PRD-0052 (three band gains, three kill bools) is introduced.

## 1.3. Developer / Integration Flow

1. `ChannelStripProcessor::prepare(double sampleRate, int blockSize)` constructs three biquad filter slots per channel (one low shelf, one peak, one high shelf) using `juce::dsp::IIR::Filter<float>` (or equivalent). It also constructs three `juce::SmoothedValue<float>` instances per channel — one per band — operating in linear-gain space, with ramp length `round(sampleRate * 0.007)` samples.
2. `ChannelStripProcessor` subscribes (message thread) to the six per-channel EQ ValueTree properties. On change, the handler converts dB → linear amplitude (with `dB <= -60` clamped to 0.0) and writes the effective target into a `std::atomic<float>` per band. The kill bool's effective target overrides the knob's target while latched: when `killX = true`, the band's target becomes 0.0 (true cut) regardless of the knob value; when released, the target reverts to the knob's last-known linear value. The kill override is computed in the message-thread handler so the audio thread sees a single effective target.
3. Inside `processBlock`, the processor loads the three atomic targets for the channel with `memory_order_relaxed` and calls `setTargetValue` on each band's smoother. Smoother state lives in the processor and persists across blocks.
4. The processor advances each smoother once per block. If the new smoothed effective gain differs from the last value used to compute the band's coefficients by more than `kCoefficientUpdateThresholdDb`, it recomputes the band's biquad coefficients via the standard RBJ cookbook formulas (using the fixed centre frequency and Q of that band) and stores the new coefficients in the band's filter slot. The coefficient update is performed in place (no allocation).
5. The processor iterates samples in order. For each sample on each channel, the three bands are applied in series in the order `low → mid → high` (or any fixed order; series order does not affect linear-magnitude response, see §1.5.3). The smoothers advance per-sample so the effective filter coefficients glide across the block; coefficients are only recomputed at the block boundary, while the per-sample multiplier (the smoother output applied as a post-biquad scalar) glides continuously (see §1.5.5).
6. The EQ stage is invoked between the channel gain stage (PRD-0054) and the filter stage (PRD-0056). EPIC-0007 §1.2.1 ordering: `gain → EQ → kills → filter → fader`. The "kills" in the Epic ordering are not a separate stage; they are folded into the same band as a target-override, so the implementation has three biquads and not six.
7. The MIDI inbound router (PRD-0044) writes the same ValueTree properties, so every input path inherits the same smoothing. No bypass.

## 1.4. Acceptance Criteria

- [ ] `ChannelStripProcessor` owns three biquad filter slots per channel: one low-shelf centred at 250 Hz (low band), one peak filter centred at 1 kHz with Q = 0.7 (mid band), one high-shelf centred at 2.5 kHz (high band).
- [ ] Each band's gain in dB is read from `mixer.channel.{A,B,C,D}.eq.{high,mid,low}` (PRD-0052) and converted to linear via `linear = pow(10, dB / 20.0)` with `dB <= -60` treated as 0.0 (true cut).
- [ ] Each band's effective target is overridden to 0.0 (true cut) while its corresponding `eq.killHigh|Mid|Low` is true; when released, the target reverts to the underlying knob's current dB-derived linear value. The knob value itself is never written by the kill machinery.
- [ ] Continuous knob changes are smoothed by a per-sample `juce::SmoothedValue` with a 7 ms ramp length (matching PRD-0054).
- [ ] Kill latch and release transitions are smoothed over a fixed 8 ms ramp (see §1.5.4). No click is audible at the channel strip output when toggling any kill at any sample-accurate moment.
- [ ] Biquad coefficients are recomputed only when the band's effective dB target moves by more than `kCoefficientUpdateThresholdDb = 0.05` dB since the last recomputation. The recomputation is performed in place inside the existing filter slot; no `new`, `malloc`, or container resizing occurs on the audio thread.
- [ ] At all knobs = 0 dB and all kills = false, the EQ stage produces output bit-equivalent (within 1e-5 per sample) to its input across a 20 Hz – 20 kHz pink-noise input.
- [ ] At any band = -inf (full CCW), the band attenuates a sine at the band's centre frequency by ≥ 60 dB, demonstrating true cut.
- [ ] At any band = +6 dB, the band boosts a sine at the band's centre frequency by 6 dB ± 0.2 dB.
- [ ] The three bands are applied in series in a fixed order; the order is documented in the implementation and constant across channels and across builds.
- [ ] The EQ stage is invoked between the channel gain stage (PRD-0054) and the filter stage (PRD-0056). EQ / kill / filter stages occupy the pass-through slots reserved by PRD-0053 in that exact order.
- [ ] All audio-thread code paths in the EQ stage (smoother advancement, atomic loads, coefficient recomputation, per-sample filtering) perform no memory allocation (no `new`, `delete`, `malloc`, `std::vector::push_back`, `std::string`), take no locks, perform no file / network / logging I/O, and communicate with non-audio threads exclusively via `std::atomic`.
- [ ] At least one unit test in `Tests/` (e.g. `ChannelStripEqTests.cpp`) verifies: (a) unity passthrough at all-zero settings, (b) -60 dB attenuation at each band's centre frequency under true cut, (c) +6 dB boost at each band's centre frequency at the upper limit, (d) no click when latching any kill mid-block, (e) no zipper noise on a 0 → -24 dB knob sweep over 100 ms.
- [ ] No new parameter is added to `MixerState` or the ValueTree schema; the EQ stage consumes only the six per-channel properties already declared by PRD-0052.

## 1.5. Grey Areas

### 1.5.1. Band Centre Frequencies and Crossover Topology

DJ mixers vary widely: Pioneer DJM-A9 uses 70 Hz / 1 kHz / 13 kHz isolator-style crossovers; Allen & Heath Xone:96 uses 100 Hz / 1.4 kHz / 10 kHz shelving; Behringer DDM4000 uses ~250 Hz / 1 kHz / 2.5 kHz. The wider the crossover separation (Pioneer-style), the more "isolator" the EQ feels — kills cut entire frequency bands cleanly. The closer the centres (DDM4000-style), the more "tonal" the EQ feels — useful for sculpting individual elements.

**Resolution:** Adopt the DDM4000-style spacing (low shelf 250 Hz, mid peak 1 kHz Q=0.7, high shelf 2.5 kHz). The Mixer Epic's stated muscle-memory target hardware includes the DDM4000, and the bundled MIDI profile is the DDM4000 (PRD-0043). Mismatched frequencies between hardware EQ and software EQ would cause the DJ's expectations of a DDM4000 hardware-mapped EQ to diverge from what they hear, which defeats the muscle-memory goal. The compact spacing also gives a more "tonal" EQ that works well for sculpting individual tracks rather than only doing brutal kill swaps; brutal swaps are still available via the per-band kill button. Frequencies and Qs are exposed as `constexpr float` in the EQ header so a future Epic could promote them to per-mixer-profile values without re-architecting the DSP.

### 1.5.2. Shelf vs Peak for High and Low Bands

Shelving filters (low shelf, high shelf) and peak (parametric) filters can both serve as the outer bands of a 3-band EQ. Shelving more accurately mimics analogue tone-control hardware and is the dominant choice in DJ mixers; peak filters give more surgical control but ring more under fast modulation.

**Resolution:** Low shelf for the low band, peak for the mid band, high shelf for the high band — the canonical DJ-mixer topology. Shelves have a flat response above (low shelf) or below (high shelf) the crossover, which is what DJs expect when they "cut all the bass" — every frequency below 250 Hz drops by the same amount. A peak in the mid band gives finer surgical control over the most musically important spectral region without affecting the spectral extremes. This is what every DJ mixer in the cited reference set does.

### 1.5.3. Series Order of the Three Bands

Three biquads in series produce the same linear-magnitude response regardless of order, but numerical noise floor and small phase nonlinearity differ. The order also affects which band sees the largest internal signal magnitude (relevant for fixed-point implementations, less so for float32).

**Resolution:** Apply in the order `low → mid → high`. This matches the natural "bass → mids → treble" cognitive ordering and is the order most DJ-mixer firmware uses. For float32 audio at the levels the channel strip handles (post-gain, pre-fader, so typically in the [-2, +2] range under normal use), numerical noise floor differences between orders are well below the channel strip's audible noise floor. The order is fixed in code as `constexpr` and documented inline.

### 1.5.4. Kill Smoothing Time and Profile

A kill that engages instantly (single-sample step) clicks loudly. A kill that engages over too long a window (>20 ms) feels mushy and breaks DJ expectations of a "kill" being a precise transient gesture. Profile choice (linear ramp, exponential, raised-cosine) also matters.

**Resolution:** Lock the kill ramp to 8 ms with a linear gain profile. 8 ms sits inside the Epic-stated 5–10 ms range, is short enough to feel like a true kill (the gesture's tactile feedback in DJ practice is "instant"), and is long enough that no audible click occurs at any signal level. A linear gain ramp is chosen over exponential because the destination value is `0.0` (true silence): exponential ramps approach silence asymptotically and would either require an arbitrary cutoff threshold or never quite reach zero. A raised-cosine profile would add CPU and is not perceptually distinguishable from linear at this duration. The ramp is implemented as a separate `juce::SmoothedValue<float>` per band initialised to a linear ramp; on kill latch / release, the smoother's target is set to 0.0 or to the knob's current linear value respectively. The 7 ms continuous-knob smoother and the 8 ms kill ramp run independently per band and are combined multiplicatively (final per-sample band gain = continuous-knob smoother output × kill smoother output, both in linear). This composition is unambiguous and produces no surprising interaction when a kill is released during a knob sweep.

### 1.5.5. Coefficient Update Strategy: Per-Block, Per-Sample, or Thresholded

Biquad coefficients depend on the band's effective gain. Options: (a) recompute every sample (CPU-heavy, perfect tracking), (b) recompute once per block from the start-of-block target (cheap, audible stair-step on long ramps), (c) recompute when the effective gain moves by more than a threshold since the last recomputation (cheap, smooth perception).

**Resolution:** Adopt strategy (c) with `kCoefficientUpdateThresholdDb = 0.05 dB`. At each block boundary, after the smoother advances, the EQ stage checks whether the new smoothed dB value has moved by more than 0.05 dB since the last coefficient recomputation; if so, it recomputes. 0.05 dB is below the audible difference threshold (typically ~0.5 dB at this band layout), and the per-sample smoother continues to advance the underlying gain target so transients are still smoothed. To avoid the coefficient-update step itself producing an audible click when coefficients change (since biquad memory carries state that becomes "incorrect" relative to the new coefficients), the implementation does not separately crossfade old and new coefficients: at the small per-update gain delta (≤ 0.05 dB), the resulting transient is below the noise floor for the audio Sonik produces. If a future profile required larger per-update steps (e.g. very fast snap-to-target), a coefficient crossfade would be added; this is out of scope here. The single-writer, single-reader pattern is trivially safe because both the writer and reader (the EQ stage) run on the audio thread.

### 1.5.6. Interaction Between Kill Knob Value and Underlying Knob

When the user latches kill, the knob value the user previously dialled in is still meaningful — they want to return to it when the kill is released. The naive implementation (write 0 to the knob field on latch, restore from a saved value on release) writes to the ValueTree from the audio path and creates ordering issues with MIDI feedback (PRD-0047) and the UI listener.

**Resolution:** The kill machinery never writes the knob's ValueTree field. Latch and release are pure UI / message-thread state transitions that flip the `killX` boolean and let the message-thread handler compute a single effective linear target (`kill ? 0.0 : knobLinear`) and push it into the band's atomic. The audio thread sees only the effective target, the smoother glides to it, and the knob's ValueTree value remains exactly as the user set it. MIDI feedback (PRD-0047) reads the knob's ValueTree value for the LED ring (so the controller continues to display the user's set value) and reads the killX boolean independently for the kill button LED. UI updates likewise read the two values independently. This contract guarantees that toggling kill is a non-destructive operation on the knob's set value.
