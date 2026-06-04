---
status: Implemented
epic: EPIC-0007
depends-on:
  - PRD-0052
  - PRD-0053
  - PRD-0055
---

# 1. PRD-0056: Per-Channel Filter Knob (HPF / LPF Sweep)

## 1.1. Problem

Every professional DJ mixer (Pioneer DJM, Allen & Heath Xone, Behringer DDM4000) exposes a per-channel filter knob with a center-detent that sweeps a high-pass filter clockwise and a low-pass filter counter-clockwise. DJs rely on this control during transitions, build-ups, and drops to remove or restore frequency content in real time without touching the EQ. Sonik's mixer Epic (EPIC-0007) declares this control as part of the in-scope channel strip and reserves the binding target `mixer.channel.{A,B,C,D}.filter` (PRD-0052), but the DSP that turns that parameter into actual filtering is not yet written. PRD-0053 has carved out a `ChannelStripProcessor` skeleton with a placeholder slot between the EQ stage and the channel fader; that slot currently passes audio through unchanged. Until a real filter stage is implemented, the filter knob exists in state and in routing but has no audible effect on the signal, which makes every other mixer PRD that depends on filter behaviour (UI feedback in PRD-0059 / PRD-0060, MIDI feedback validation in PRD-0061) untestable.

A naive implementation also creates two failure modes a DJ will immediately hear: (1) clicks and zipper noise as the user sweeps the knob across the detent or rapidly turns it during a transition, and (2) wasted CPU recomputing filter coefficients on every sample when the knob is parked at 12 o'clock and the filter is supposed to be doing nothing at all. Both must be ruled out before downstream PRDs can integrate against the channel strip.

## 1.2. Objective

The system provides, for every mixer channel, a real per-channel filter DSP stage inside `ChannelStripProcessor` such that:

- The bipolar parameter `mixer.channel.{A,B,C,D}.filter` (range `[-1, +1]`, default `0.0`) defined by PRD-0052 drives one state-variable filter per channel that operates as a high-pass when the parameter is positive, as a low-pass when negative, and is fully bypassed inside a center detent.
- The cutoff sweep covers approximately 20 Hz to 20 kHz on each side of the detent, mapped exponentially so the audible "musical" response is roughly linear in perceived pitch.
- The filter introduces no clicks when the user enters or leaves the detent, no zipper noise during fast cutoff sweeps, and no audible artefacts when the parameter crosses the detent from HPF to LPF or vice versa.
- When the knob is parked inside the detent the filter stage is short-circuited: no per-sample coefficient computation, no allocations, no extra branches on the hot path beyond a single bypass test.
- The filter stage sits between the EQ stage (PRD-0055) and the channel fader (PRD-0054), matching the signal flow declared in EPIC-0007 §1.2.1.
- Every audio-thread code path obeys `CLAUDE.md`: no allocations, no locks, no I/O, cross-thread parameter delivery through `std::atomic` only.
- No new user-facing parameter beyond the single existing filter parameter is introduced. In particular, no resonance parameter, no separate HPF/LPF toggle, no cutoff parameter, no engage/bypass parameter.
- The implementation exposes a small, deterministic interface (cutoff in Hz, mode, bypass) that the UI PRDs (PRD-0059 knob atom, PRD-0060 channel strip organism) can bind without re-deriving filter math.

## 1.3. Developer / Integration Flow

1. `MixerState` (PRD-0052) publishes the per-channel filter value as a `std::atomic<float>` in the audio-thread snapshot consumed by `ChannelStripProcessor`. The value is already clamped to `[-1, +1]` by the state layer and the detent-snap is enforced at write time (see §1.5.6); the audio thread treats it as authoritative and does no re-clamping beyond a defensive `juce::jlimit`.
2. `ChannelStripProcessor::prepare(double sampleRate, int blockSize)` (the skeleton method introduced by PRD-0053) constructs and resets one `juce::dsp::StateVariableTPTFilter<float>` per channel, sets the fixed Butterworth Q (`0.7071`), and pre-allocates the per-sample parameter smoother state. No allocations happen after `prepare`.
3. On each block, `ChannelStripProcessor::processBlock` reads the channel's filter parameter once from the atomic into a local `float filterParam`. It also reads the previous block's "engaged side" (`+1`, `-1`, or `0`) from a member variable on the processor (no atomic needed; only the audio thread writes it).
4. The processor classifies the target state of the filter for this block from `filterParam`:
   - If `|filterParam| < kDetentEpsilon`: target is bypass.
   - Else if `filterParam >  kDetentEpsilon`: target is HPF, with normalised side position `t = (filterParam - kDetentEpsilon) / (1 - kDetentEpsilon)`.
   - Else: target is LPF, with normalised side position `t = (-filterParam - kDetentEpsilon) / (1 - kDetentEpsilon)`.
5. The processor maps `t ∈ (0, 1]` to a target cutoff in Hz using the exponential map declared in §1.5 (HPF and LPF use the same magnitudes, mirrored). Both sides span 20 Hz → 20 kHz.
6. The processor handles transitions between sides:
   - Same side as previous block, not bypassed: the per-sample cutoff smoother is fed the new target Hz; no SVF state reset; the wet/dry mix remains at 1.0.
   - Previous block was bypassed, this block is engaged: the SVF mode is set to the current side, the SVF integrator state is reset to zero, the cutoff smoother is snapped to the new target, and the wet/dry mix begins a ramp from 0 toward 1 over `kEngageRampSamples`.
   - Previous block was engaged, this block is bypassed: the wet/dry mix ramps from its current value toward 0 over `kEngageRampSamples`. Once it reaches zero, the SVF is short-circuited (see step 8).
   - Side changed (HPF → LPF or LPF → HPF) without passing through a bypass block (a fast crossing): the SVF mode is switched at the side-change sample, the SVF integrator state is reset to zero, the cutoff smoother is snapped to the new target Hz, and the wet/dry mix is re-ramped from 0 → 1 over `kEngageRampSamples`. See §1.5.5.
7. For each sample in the block, when the filter is engaged:
   - The per-sample cutoff smoother advances one step toward target (one-pole exponential, time constant `kCutoffSmoothingTauMs`, performed in log-Hz domain; see §1.5.3).
   - The smoothed log-Hz value is converted to Hz with a single `std::exp` and pushed into the SVF via `setCutoffFrequency`.
   - The SVF processes the L and R sample.
   - The wet/dry ramp advances one step linearly.
   - Output = `wet * filteredSample + (1 - wet) * inputSample`.
8. When `filterParam` is inside the detent and the wet/dry ramp has fully reached zero, `processBlock` skips the SVF processing branch entirely: no `setCutoffFrequency`, no `processSample`, no smoothing math. The bypass branch is a single compare-and-jump on the hot path, and the input buffer is left untouched in place.
9. The filter stage is invoked from `ChannelStripProcessor::processBlock` after the EQ stage (PRD-0055) writes its output into the channel scratch buffer and before the channel fader stage (PRD-0054) consumes it. This matches EPIC-0007 §1.2.1: `gain → EQ → filter → fader`.
10. The UI thread, MIDI input (PRD-0044), and any future automation source write the filter parameter through the same state path declared by PRD-0052. The detent snap (§1.5.6) is enforced inside the state setter, so the audio thread never observes a value with `0 < |filter| < kDetentEpsilon`.
11. No new file, no new class, and no new feature slice are introduced by this PRD. All code lives in the existing `Source/Features/Mixer/Routing/ChannelStripProcessor.{h,cpp}` (created by PRD-0053) and the existing `Source/Features/Mixer/Dsp/` directory if a small free-function `mapFilterParamToCutoffHz` helper is needed.

## 1.4. Acceptance Criteria

- [ ] `ChannelStripProcessor` owns one `juce::dsp::StateVariableTPTFilter<float>` per mixer channel, configured with type `LowPass` or `HighPass` depending on the current engaged side and with a fixed Q of `0.7071`.
- [ ] The per-channel filter stage is invoked between the EQ stage (PRD-0055) and the channel fader stage (PRD-0054), with no other DSP block interposed.
- [ ] When `|filter| < kDetentEpsilon` (with `kDetentEpsilon = 0.02f`), the audio output of the filter stage is bit-identical to its input for all engaged-to-bypass transitions that have completed (i.e. once the wet/dry ramp has reached zero).
- [ ] When `filter > kDetentEpsilon`, the stage operates as a high-pass filter; when `filter < -kDetentEpsilon`, it operates as a low-pass filter.
- [ ] At `filter = +1.0`, the HPF cutoff is within ±1% of 20 kHz; at `filter = -1.0`, the LPF cutoff is within ±1% of 20 Hz; at `filter = +kDetentEpsilon + small`, the HPF cutoff is within ±1% of 20 Hz; at `filter = -kDetentEpsilon - small`, the LPF cutoff is within ±1% of 20 kHz.
- [ ] The cutoff mapping uses the exponential formula declared in §1.5.4 (with `fMin = 20 Hz`, `fMax = 20 kHz`, `epsilon = 0.02`).
- [ ] Per-sample cutoff smoothing is implemented as a one-pole exponential filter in the log-Hz domain with a time constant of `kCutoffSmoothingTauMs = 10.0f` ms (see §1.5.3).
- [ ] Crossing into or out of the detent triggers a linear wet/dry ramp of length `kEngageRampSamples` (equivalent to 5 ms at the current sample rate) and never produces a click.
- [ ] Crossing the detent directly (HPF ↔ LPF) without stopping inside the detent triggers (a) an SVF integrator state reset to zero, (b) a snap of the cutoff smoother to the new target Hz, and (c) a fresh 0 → 1 wet/dry ramp of length `kEngageRampSamples` on the new side (see §1.5.5).
- [ ] When parked in the detent and the wet/dry ramp has reached zero, the audio-thread cost of the filter stage is a single bypass check — no `setCutoffFrequency` call, no `processSample` call, no smoothing math.
- [ ] The audio-thread code path inside `ChannelStripProcessor::processBlock` performs no memory allocation (no `new`, `delete`, `malloc`, `std::vector::push_back`, `std::string`).
- [ ] The audio-thread code path takes no locks (`std::mutex`, `std::lock_guard`, `juce::CriticalSection`, etc.) and performs no I/O (file, network, logging, `DBG`, `std::cout`).
- [ ] Cross-thread parameter delivery uses only `std::atomic` reads of `mixer.channel.{A,B,C,D}.filter` (already in place via PRD-0052) — no shared mutable non-atomic state is read.
- [ ] No resonance parameter, cutoff parameter, mode-toggle parameter, or bypass parameter is added to `MixerState`, the `ValueTree` schema, or the MIDI binding registry. The only mixer-side filter parameter remains `mixer.channel.{A,B,C,D}.filter`.
- [ ] The detent snap is enforced inside the state setter for `mixer.channel.{A,B,C,D}.filter` so that any write — from UI, MIDI input (PRD-0044), or future automation — that lands inside the detent stores exactly `0.0f`. The audio thread never observes a value satisfying `0 < |filter| < kDetentEpsilon` (see §1.5.6).
- [ ] A unit test under `Tests/` (e.g. `ChannelStripFilterTests.cpp`) drives `ChannelStripProcessor` with a known signal (sine sweep or white noise) and asserts: (a) bypass equivalence at `filter = 0`, (b) HPF attenuation > 30 dB at 20 Hz when `filter = +1.0`, (c) LPF attenuation > 30 dB at 20 kHz when `filter = -1.0` (at supported sample rates), (d) no NaN / Inf samples on a fast detent-crossing sweep, (e) no inter-block sample discontinuity > 0.5 LSB at 16-bit equivalent when sweeping at typical UI rates.

## 1.5. Grey Areas

### 1.5.1. Fixed Resonance Q

The Epic states "resonance is fixed by this Epic" (§1.3.6) but does not pick a value. Candidates: Butterworth (`Q = 0.7071`, maximally flat magnitude, no resonant peak), mild musical resonance (`Q ≈ 1.0`, slight emphasis near cutoff that DJs sometimes prefer on filter sweeps), or strong DJM-style resonance (`Q ≈ 2.0+`, audible "wow" sweep).

**Resolution:** Lock `Q = 0.7071` (Butterworth). It guarantees a maximally flat passband and a monotonic transition that will not surprise the user with peaks during fast sweeps, will not interact badly with the EQ stage immediately upstream (PRD-0055), and matches the behaviour DJs assume when a mixer ships without an explicit resonance knob. A resonant "creative" filter is an FX-Epic concern, not a channel-strip concern. The value is exposed as a single `constexpr float kFilterQ = 0.7071f;` in the channel-strip processor header so that a future Epic can promote it to a parameter without re-architecting the DSP.

### 1.5.2. Detent Epsilon Value

The bipolar `[-1, +1]` parameter needs a "dead zone" around `0.0` where the filter is fully bypassed. Too narrow (e.g. `0.005`) and a DJ trying to park the knob at 12 o'clock will frequently land just outside the detent and hear an unintended faint filter. Too wide (e.g. `0.10`) and the audible filter sweep loses precision near the detent because the first 10% of knob travel does nothing.

**Resolution:** Lock `kDetentEpsilon = 0.02f`. This is 2% of half-travel, which on a typical 270-degree rotary corresponds to roughly ±2.7 degrees around 12 o'clock — wider than the angular resolution of a mouse-drag interaction, narrower than the angular resolution a DJ uses to "aim" at the detent. It is exposed as a single `constexpr float` in the channel-strip processor header. The detent is enforced as a snap at the state-write boundary (§1.5.6), not as a soft dead-zone in the DSP, so all DSP code can rely on the invariant `filter == 0.0f` OR `|filter| >= kDetentEpsilon`.

### 1.5.3. Coefficient Smoothing Strategy

A 20 Hz → 20 kHz exponential cutoff sweep performed naively (one-pole smoother on cutoff Hz) is asymmetric: smoothing in Hz space gives near-instant response near the top of the sweep (e.g. 19 kHz → 20 kHz) and audibly slow response near the bottom (e.g. 30 Hz → 40 Hz), because the same Hz delta is a tiny fraction of an octave up high and a huge fraction down low. Smoothing the normalised parameter `t` before the exponential map is sample-cost equivalent but exhibits the opposite distortion: equal time per octave but uneven response near the detent epsilon.

**Resolution:** Smooth the log-Hz target with a per-sample one-pole exponential filter at a fixed time constant `kCutoffSmoothingTauMs = 10.0f` ms. Concretely: the target cutoff in Hz is converted once per block to `targetLogHz = std::log(targetCutoffHz)`; the per-sample state `currentLogHz` advances `currentLogHz += alpha * (targetLogHz - currentLogHz)` with `alpha = 1.0f - std::exp(-1.0f / (sampleRate * kCutoffSmoothingTauMs * 1.0e-3f))`; the SVF cutoff is then `std::exp(currentLogHz)`. This gives a sweep response that is constant in octaves-per-second regardless of where in the range the sweep happens, which is what the exponential cutoff map (§1.5.4) is already promising the user. The 10 ms time constant is short enough that DJ-rate knob movement feels instantaneous and long enough to suppress zipper noise from UI input quantisation. `std::log` and `std::exp` are deterministic and have well-defined cost on every supported platform; they run on the audio thread, which is acceptable because the channel strip's existing EQ stage (PRD-0055) already performs comparable per-sample math.

### 1.5.4. Exponential Cutoff Mapping Formula

The user-facing promise is "20 Hz to 20 kHz, musically linear." Concretely we need a closed-form map from the parameter `filter ∈ [-1, +1]` to a target cutoff in Hz, with the bypass detent excluded.

**Resolution:** With `fMin = 20.0f`, `fMax = 20000.0f`, and `epsilon = kDetentEpsilon = 0.02f`, define the engaged-side normalised position `t ∈ (0, 1]`:

- HPF (`filter > epsilon`): `t = (filter - epsilon) / (1 - epsilon)`, cutoff `Hz = fMin * std::pow(fMax / fMin, t)`. At `t → 0+` the cutoff approaches `fMin = 20 Hz`; at `t = 1` it equals `fMax = 20 kHz`. The HPF starts barely audible (rolling off below ~20 Hz) and sweeps up to fully muting the signal.
- LPF (`filter < -epsilon`): `t = (-filter - epsilon) / (1 - epsilon)`, cutoff `Hz = fMax * std::pow(fMin / fMax, t)` (equivalently `fMax / std::pow(fMax / fMin, t)`). At `t → 0+` the cutoff approaches `fMax = 20 kHz`; at `t = 1` it equals `fMin = 20 Hz`. The LPF starts barely audible (rolling off above ~20 kHz) and sweeps down to fully muting the signal.

This is the standard "1000x ratio over one knob side" exponential and gives roughly 10 octaves per side, with constant octaves-per-radian — the perceptually linear sweep DJs expect. The same `std::pow(fMax / fMin, t)` factor is shared by HPF and LPF, so it is computed once per block (target cutoff is constant within a block; per-sample smoothing happens after the map, in log-Hz domain — see §1.5.3) and the cost is negligible.

The Nyquist clamp is applied to the final smoothed cutoff before it reaches the SVF: `cutoffHz = juce::jmin(cutoffHz, 0.45 * sampleRate)` to keep the TPT filter stable near the top of the HPF sweep at 44.1 kHz devices.

### 1.5.5. SVF Topology Choice

Three reasonable candidates: a classical Chamberlin SVF (cheap, well-understood, but unstable at high cutoff/Q combinations), a hand-rolled TPT (topology-preserving transform) SVF, or JUCE's `juce::dsp::StateVariableTPTFilter`.

**Resolution:** Use `juce::dsp::StateVariableTPTFilter<float>`. The TPT (zero-delay feedback) topology is unconditionally stable across the full 20 Hz → 20 kHz cutoff range at any supported sample rate and is well-behaved under fast cutoff modulation — exactly the conditions a DJ filter knob will impose. JUCE's implementation is in-tree (no new dependency), already used elsewhere in the codebase pattern, and supports `setCutoffFrequency`, `setResonance`, and `setType` (LowPass / HighPass / BandPass) directly. The Chamberlin SVF would save a handful of cycles per sample but its sample-rate-dependent stability limit (`fc < sampleRate / pi` approximately) is too close to our 20 kHz target at 44.1 kHz to be safe. Hand-rolling a TPT SVF buys nothing over the JUCE implementation and adds maintenance surface.

### 1.5.6. Behaviour When Cutoff Crosses the Detent Quickly

If the user (or a MIDI controller) sweeps the filter parameter from, say, `+0.5` to `-0.5` in a single UI tick, the filter must transition HPF → bypass → LPF without clicks and without holding stale integrator state from the HPF side that would ring inside the new LPF mode. Three reasonable strategies: (a) continuously interpolate filter coefficients across the crossing, (b) switch mode at the detent boundary and let the SVF state evolve naturally, (c) switch mode and explicitly reset the SVF integrator state at the crossing.

**Resolution:** Adopt strategy (c) with an explicit wet/dry ramp. At the first audio block in which the side changes (HPF → LPF or LPF → HPF) without an intervening fully-bypassed block, the channel strip:

1. Sets the SVF type to the new side (`LowPass` or `HighPass`).
2. Resets the SVF integrator state to zero (via `juce::dsp::StateVariableTPTFilter::reset()`), since the previous side's internal state has no physical meaning under the new mode and would otherwise emit a transient.
3. Snaps the cutoff smoother to the new target Hz (no exponential glide across the detent — a glide here is meaningless because the filter is conceptually "off" at the detent).
4. Starts a fresh wet/dry ramp from 0 toward 1 over `kEngageRampSamples` (5 ms at the current sample rate).

This guarantees no click even on instant detent crossings, costs one extra reset per crossing (rare), and keeps the bypass-detent invariant trivially true. Strategy (a) would require maintaining and crossfading two SVF instances per channel, doubling DSP cost for a control gesture that happens a few times per second at most; strategy (b) preserves state that is mathematically meaningless across the mode switch and can ring audibly.

### 1.5.7. Detent Snap on MIDI Writes

PRD-0044 routes MIDI controller input directly into the binding-target namespace. A 7-bit MIDI CC mapped onto `[-1, +1]` lands on exactly 128 discrete values, of which roughly two or three fall inside the `[-0.02, +0.02]` detent. The question: does the MIDI write path bypass the detent snap (so a controller's exact center position is preserved as-is and the DSP layer decides bypass), or does it enforce the snap the same way the UI does?

**Resolution:** The detent snap is enforced at the state setter, not at any individual writer. Any write to `mixer.channel.{A,B,C,D}.filter` from any source — UI knob drag, UI double-click reset, MIDI input (PRD-0044), or future automation — passes through the same setter, and that setter snaps any value satisfying `|value| < kDetentEpsilon` to exactly `0.0f`. This (a) keeps the bypass invariant `filter == 0.0f` OR `|filter| >= kDetentEpsilon` unconditionally true, (b) means the DSP layer can rely on a single integer-equality bypass check instead of an epsilon-test in the hot loop, and (c) ensures a DJ centring the knob via a MIDI controller gets the same audible "click into bypass" as a DJ centring it via the UI. The cost is that a small contiguous range of MIDI CC values (typically values 62, 63, 64, 65 on a 0–127 CC mapped via standard bipolar conversion) all snap to exact center, which is the correct behaviour for a detented knob.
