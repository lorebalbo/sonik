---
status: Not Implemented
epic: EPIC-0007
depends-on:
  - PRD-0052
  - PRD-0053
  - PRD-0054
---

# 1. PRD-0057: Crossfader and A/B Channel Assignment

## 1.1. Problem

Once each mixer channel has its own gain stage and fader (PRD-0054), the DJ still has no way to perform the single most defining gesture of a mixer: blending two channels against each other with a crossfader. Without a crossfader, every transition has to be done by riding two channel faders simultaneously, which is ergonomically awkward, makes scratching impossible, and prevents the DJ from using either hand for cues, EQ, or filter work. The channel-strip skeleton produced by PRD-0053 already exposes an `ABBus` summing stage and a `CrossfaderStage` block in the signal flow, but both are currently pass-throughs: every channel contributes equally to a single mono-summed bus regardless of its A/B assignment, and the `mixer.crossfader` value declared by PRD-0052 has no DSP effect.

A working crossfader is also the first place in the mixer where curve choice becomes audible. A DJ scratching expects a hard-cut "sharp" curve where any motion away from the edge instantly opens the opposite channel; a DJ doing long blends expects an equal-power "smooth" curve that keeps total perceived loudness constant through the middle of the travel. Sonik must support both, selectable at runtime, and switching between them must not click. Finally, every continuous crossfader motion must be smoothed: directly multiplying a per-sample signal by a stepwise-changing gain produces audible zipper noise on sustained content, which is unacceptable for a professional tool.

## 1.2. Objective

The system provides a working crossfader DSP stage and per-channel A/B bus routing that:
- Routes each channel's post-fader output to bus A, bus B, both, or neither based on the channel's `assignA` and `assignB` flags declared by PRD-0052.
- Applies a click-free `(gainA, gainB)` pair to the two buses based on the current `mixer.crossfader` position and the selected `mixer.crossfader.curve`.
- Supports at least two curves: `"sharp"` for scratching (hard cut near the edges) and `"smooth"` for mixing (equal-power blend).
- Smooths crossfader position changes over a fixed window so that no audible click or zipper is produced when the crossfader moves abruptly.
- Sums the gained A and B buses into a single stereo bus that feeds the master stage (PRD-0058).
- Runs entirely on the audio thread with no allocations, locks, or I/O, and communicates with the message thread exclusively through `std::atomic` reads.

This PRD amends PRD-0052 by adding the `mixer.crossfader.curve` field to the ValueTree schema. This PRD does not implement master gain, metering, or any UI; those are owned by PRD-0058 and the UI PRDs in the Epic.

## 1.3. Developer / Integration Flow

1. The audio engine completes the per-channel signal chain (gain → EQ → filter → channel fader) for every active channel, leaving each channel's post-fader stereo buffer ready for routing. This is the contract handed off by PRD-0054.
2. For each channel, the `ChannelStripProcessor` reads two `std::atomic<bool>` flags from the audio-thread state mirror of the channel's ValueTree node: `assignA` and `assignB` (defined by PRD-0052).
3. The `ABBus` stage accumulates two stereo buses for the current block. For every channel `i`, sample-by-sample: `busA += assignA_i ? channelOut_i : 0` and `busB += assignB_i ? channelOut_i : 0`. Both flags false means the channel contributes to neither bus and is therefore inaudible at the master output (see §1.5.3). Both flags true means the channel contributes equally to both buses (dual assign / "thru"); see §1.5.4 for the headroom implication.
4. The `CrossfaderStage` reads `mixer.crossfader` (a `std::atomic<float>` in `[0, 1]`, declared by PRD-0052) and `mixer.crossfader.curve` (a `std::atomic<int>` enumeration encoded from the schema field added by this PRD; the message thread translates the ValueTree string into the enum before the audio thread reads it).
5. The raw crossfader position is fed into a per-sample one-pole smoother whose time constant produces a 7 ms exponential rise to 90% (see §1.5.6). The smoothed position is then passed to the curve function selected by the current enum value to produce `(gainA, gainB)`. The curve is evaluated once per sample, not once per block, so that smoother output is faithfully tracked even during fast scratching motions.
6. The crossfader stage writes `output = gainA * busA + gainB * busB` into the stereo bus consumed by the master stage (PRD-0058).
7. The message thread updates `mixer.crossfader` and `mixer.crossfader.curve` from UI / MIDI input. ValueTree listeners (PRD-0052 contract) propagate changes into the audio-thread atomics. No audio-thread code touches the ValueTree directly.
8. On startup, the mixer initialises `mixer.crossfader = 0.5` and `mixer.crossfader.curve = "smooth"` (see §1.5.5).

## 1.4. Acceptance Criteria

- [ ] The `ABBus` stage accumulates the post-fader output of every channel into stereo bus A when `assignA` is true for that channel, and into stereo bus B when `assignB` is true for that channel.
- [ ] A channel with `assignA = false` and `assignB = false` contributes zero to both buses (the channel is silent at the crossfader output).
- [ ] A channel with `assignA = true` and `assignB = true` contributes its full post-fader signal to both buses without normalisation (dual-assign, hardware-style behaviour).
- [ ] The `CrossfaderStage` reads `mixer.crossfader` (range `[0, 1]`) and `mixer.crossfader.curve` (enum: `sharp`, `smooth`) from `std::atomic` mirrors of the ValueTree state.
- [ ] The `smooth` curve produces `gainA = cos(p * π / 2)` and `gainB = sin(p * π / 2)`, where `p` is the smoothed crossfader position in `[0, 1]`. This is an equal-power blend: `gainA^2 + gainB^2 = 1` for all `p`.
- [ ] The `sharp` curve produces a piecewise-linear cut centred at `p = 0.5` with a transition half-width of `w = 0.02` (full transition width 4% of the crossfader travel):
  - For `p ∈ [0, 0.5 − w]`: `gainA = 1`, `gainB = 0`.
  - For `p ∈ (0.5 − w, 0.5 + w)`: `gainA = ((0.5 + w) − p) / (2w)`, `gainB = (p − (0.5 − w)) / (2w)`.
  - For `p ∈ [0.5 + w, 1]`: `gainA = 0`, `gainB = 1`.
- [ ] The crossfader stage writes `gainA * busA + gainB * busB` into the stereo bus consumed by the master stage (PRD-0058).
- [ ] Raw position changes on `mixer.crossfader` are smoothed by a one-pole filter whose coefficient produces a 7 ms time-to-90% rise at the current device sample rate. The smoother is reinitialised when the device sample rate changes.
- [ ] Switching `mixer.crossfader.curve` between `sharp` and `smooth` while audio is playing produces no click, pop, or discontinuity in the output. The smoothed position is not reset on curve change; only the curve function applied to it changes.
- [ ] The `mixer.crossfader.curve` field is added to the ValueTree schema declared by PRD-0052 with valid string values `"sharp"` and `"smooth"`, default `"smooth"`. Any other string is rejected by the message-thread validator and the previous valid value is retained.
- [ ] On application startup, `mixer.crossfader = 0.5` and `mixer.crossfader.curve = "smooth"`.
- [ ] All audio-thread code paths in the `ABBus` stage and `CrossfaderStage` perform zero memory allocations (no `new`, `delete`, `malloc`, `std::string`, `std::vector::push_back`), take zero locks, and perform zero I/O (file, network, logging).
- [ ] All cross-thread communication for crossfader position, curve selection, and channel A/B flags uses `std::atomic` exclusively; the audio thread never reads from the ValueTree directly.
- [ ] No UI work, no master-gain logic, no metering, and no MIDI binding registration is introduced by this PRD. The crossfader value and curve are exercised through the existing binding targets `mixer.crossfader` and `mixer.crossfader.curve` (the latter newly registered by PRD-0052 amendment).

## 1.5. Grey Areas

### 1.5.1. Location and Enum of `mixer.crossfader.curve`

The crossfader curve must be stored somewhere observable. The `mixer.crossfader` field declared by PRD-0052 is a single float; the curve does not belong on it. A sibling field is the natural choice, but PRD-0052 is already drafted and authoritative for the schema.

**Resolution:** Add `mixer.crossfader.curve` as a sibling field at the same hierarchy level as `mixer.crossfader` (i.e., directly under the `mixer` branch of the ValueTree), with a string type and a closed enum of `"sharp"` and `"smooth"`. The default is `"smooth"`. The message thread translates the string into a small enum (`enum class CrossfaderCurve : int { Smooth = 0, Sharp = 1 }`) and writes it into a `std::atomic<int>` that the audio thread reads. This PRD explicitly amends PRD-0052: the field is added to the PRD-0052 schema definition and to the binding-target registry. Adding a third value to the enum in a future PRD is a non-breaking change because unknown strings fall back to the last valid value.

### 1.5.2. Per-Session vs. Per-Deck-Count vs. App-Global Curve

The crossfader curve could be scoped per session (saved with each mix), per deck-count layout (2-deck vs. 4-deck), or globally for the whole application.

**Resolution:** App-global. Sonik exposes a single mixer instance and a single crossfader. Scoping the curve per session would force the DJ to reconfigure their preferred curve every time they load a new set, which contradicts the "muscle-memory compatibility" goal stated in EPIC-0007 §1.1. Scoping per deck-count is meaningless because the crossfader is a single global control regardless of how many decks are visible. The curve therefore lives at the top level of the `mixer` branch (alongside `mixer.crossfader` and `mixer.master.gain`) and is implicitly app-global because the mixer itself is.

### 1.5.3. Behaviour When Both `assignA` and `assignB` Are False

A channel with neither A nor B assigned could either (a) be muted from the crossfader output entirely, or (b) bypass the crossfader and feed the master at full gain ("thru"). DJ hardware varies; some mixers (early DDM4000 firmwares) implement option (b), while modern Pioneer/Allen & Heath mixers implement option (a).

**Resolution:** Option (a) — muted. A channel with both assign buttons off contributes zero to bus A, zero to bus B, and therefore zero to the crossfader output. This matches the dominant modern hardware convention and matches DJ expectation: turning both assign LEDs off is the gesture used to "take a channel out of the crossfader path entirely," and the only sensible interpretation of "out of the crossfader path" when the crossfader path is the only path to the master is silence. A future "thru" routing that bypasses the crossfader would be a new, separately named feature; conflating it with "both off" would be ergonomically confusing.

### 1.5.4. Dual-Assign (Both True) Normalisation

When a channel is assigned to both A and B and the crossfader is centred on a smooth curve, the channel's effective gain is `gainA + gainB = cos(π/4) + sin(π/4) ≈ 1.414`. Normalising by dividing by `sqrt(2)` (or by `gainA + gainB`) would keep the dual-assigned channel at unity at all crossfader positions, at the cost of diverging from hardware behaviour.

**Resolution:** Do not normalise. The un-normalised sum matches Pioneer DJM and Allen & Heath Xone hardware: a dual-assigned channel is loudest when the crossfader is centred, which is the exact reason a DJ would dual-assign in the first place (to use the crossfader as a momentary "boost / cut" rather than a blend). The headroom implication is that a single dual-assigned channel can drive the crossfader output up to +3 dB relative to the channel's post-fader level on the smooth curve. PRD-0058's master stage and the hard-clip safety net inherited from PRD-0002 are responsible for the final headroom budget; the crossfader stage does not own gain compensation.

### 1.5.5. Default Position and Curve at Startup

The crossfader could initialise at either end (full A or full B), in the middle, or at the last saved position.

**Resolution:** Initialise at `mixer.crossfader = 0.5` (centred) with `mixer.crossfader.curve = "smooth"`. Centring on startup is the only position that respects symmetry between decks: opening Sonik should not arbitrarily favour Deck A or Deck B. `"smooth"` is the safe default because it is the curve most DJs use for general mixing; selecting `"sharp"` by default would surprise a user who moves the crossfader expecting a gradual blend and instead gets a hard cut. Persistence of the last-saved position is deferred to a future session-restore PRD.

### 1.5.6. Smoothing Time Constant

The Epic specifies "click-free smoothing on crossfader position changes (smoothing time ~5–10 ms)." A value at the low end of the range preserves the responsiveness needed for scratching; a value at the high end maximises click suppression on coarse mouse drags.

**Resolution:** Lock the one-pole smoother to a 7 ms time-to-90% rise. This sits in the middle of the Epic's stated range, is well above any reasonable per-buffer audible-click threshold (~3 ms at 44.1 kHz buffers of 128 samples), and is short enough that a fast scratching motion (typical hand travel ~50–100 ms across the fader) is not perceptibly lagged. The smoother is implemented in the audio thread as a single-state one-pole IIR with coefficient `α = 1 − exp(−ln(10) / (0.007 * sampleRate))`, recomputed only when the device sample rate changes (message thread → atomic publish → audio thread reload at the start of the affected block).

### 1.5.7. Exclusion of the `transition` Curve

EPIC-0007 §1.3.6 enumerates "sharp, smooth, transition-style" curves but explicitly qualifies the requirement as "at minimum sharp and smooth." A `transition` curve (both channels at unity through most of the travel, with cuts only at the very edges) is a recognised third option in many hardware mixers but is not required by the Epic.

**Resolution:** Out of scope for this PRD. The implementation defines the curve enum as exactly `{ Smooth, Sharp }`, and the ValueTree validator rejects any other string. Adding `"transition"` later is a non-breaking change: it requires (a) extending the enum, (b) implementing the new curve function, and (c) widening the validator's accepted string set. No existing acceptance criterion of this PRD prevents that future extension, and no schema migration is required because the field type (`string`) does not change.
