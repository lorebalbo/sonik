---
status: Not Implemented
epic: EPIC-0007
depends-on:
  - PRD-0002
  - PRD-0010
  - PRD-0052
  - PRD-0053
---

# 1. PRD-0054: Channel Gain Stage and Channel Fader

## 1.1. Problem

PRD-0053 introduces the `ChannelStripProcessor` skeleton and re-routes the audio engine so that each deck's signal now passes through a per-channel processing chain on its way to the A/B bus and the master sum. The skeleton is plumbed but inert: every channel strip currently passes audio through unchanged, ignoring both the channel gain knob (the trim) and the channel volume fader. As a result, the DJ has no functional level control on the new mixer at all. The deck-shell gain knob defined in PRD-0010 still drives an atomic that the engine reads in its pre-refactor location, which is now inconsistent with the new signal flow defined in EPIC-0007 §1.2.1 (`gain → EQ → kills → filter → fader`).

Two DSP stages must come online before any further mixer work is useful: the channel gain stage (trim, -inf … +12 dB) and the channel volume fader (linear amplitude, 0 … 1). Both are continuous controls that a DJ may move aggressively mid-mix, which means a naive per-block multiplier produces audible zipper noise. Without click-free parameter smoothing and without unification of the deck-shell gain knob with the mixer's channel gain knob into a single source of truth, the mixer cannot fulfil its role as the central mixing console and PRD-0010's contract effectively forks.

## 1.2. Objective

The system provides functional channel gain and channel fader DSP stages inside the `ChannelStripProcessor` such that:

- Each channel strip reads `mixer.channel.{A,B,C,D}.gain` (the trim, -inf dB … +12 dB, default 0 dB) from the state defined in PRD-0052 and applies it as a linear multiplier to its input audio.
- Each channel strip reads `mixer.channel.{A,B,C,D}.fader` (linear amplitude, 0.0 … 1.0, default 1.0) from the same state and applies it as the final stage of the channel chain, immediately before the A/B-bus sum performed by PRD-0053.
- Both stages apply per-sample parameter smoothing with a 7 ms time constant so that step changes (UI drag end, MIDI value jump, programmatic write) do not produce zipper noise or clicks.
- The DSP per-deck gain multiplier that PRD-0002 previously applied inside the engine's summing loop is removed from the engine sum and reapplied inside the channel strip's gain stage, completing the migration started by PRD-0053. PRD-0002's hard-clip safety net at the master output remains authoritative and is untouched.
- The deck-shell gain knob defined in PRD-0010 and the mixer's channel gain knob become the same control: one atomic float, one ValueTree key (`mixer.channel.{A,B,C,D}.gain`), one DSP stage. Removal of the deck-shell gain knob UI itself is explicitly out of scope and is handed off to PRD-0060.
- Neither stage introduces more than one sample of latency.
- All audio-thread paths involved comply with the immutable rules in `AGENTS.md`.

## 1.3. Developer / Integration Flow

1. `ChannelStripProcessor::prepareToPlay(sampleRate, blockSize)` allocates and initialises a `juce::SmoothedValue<float>` (or equivalent) for gain (in linear amplitude) and another for fader, both with their ramp length set to `round(sampleRate * 0.007)` samples (the 7 ms smoothing constant). Initial values are set to the current ValueTree values via `setCurrentAndTargetValue` so that no ramp is performed at startup.
2. `ChannelStripProcessor` subscribes (message thread) to `mixer.channel.{X}.gain` and `mixer.channel.{X}.fader` changes via the existing ValueTree listener wiring established by PRD-0052. On change, the message-thread handler converts dB to linear (gain only), clamps to the documented ranges, and writes the new target into a pair of `std::atomic<float>` snapshot values: `gainLinearTarget` and `faderTarget`.
3. Inside `processBlock`, the processor first loads the two atomics with `memory_order_relaxed` and calls `setTargetValue` on each `SmoothedValue`. The smoothers' internal state persists across blocks because they live in the channel strip object allocated at `prepareToPlay`.
4. The processor iterates samples in order. For each sample on each channel, the gain stage multiplies the input by `gainSmoother.getNextValue()`. The fader stage multiplies by `faderSmoother.getNextValue()`. Between the gain and fader stages, the EQ / kills / filter stages provided by sibling PRDs (PRD-0055, PRD-0056) will eventually sit; for this PRD they are still pass-through, so the chain is effectively `input → gain → (pass-through) → fader → output`. The ordering is `gain → EQ → kills → filter → fader` as fixed by EPIC-0007 §1.2.1.
5. The engine's pre-refactor per-deck gain multiplier (PRD-0002 §"per-deck gain applied before summing") is removed from the engine sum in this PRD. The engine now sums the channel-strip outputs directly without applying any gain, exactly as PRD-0053 prescribed; the only thing this PRD changes inside the engine is the deletion of the now-redundant gain multiplication, not any restructuring of the routing.
6. PRD-0010's deck-shell gain knob continues to function and continues to write to the same ValueTree field. Because the field is now consumed inside the channel strip rather than in the engine sum, the audible behaviour is identical to PRD-0010's contract. The deck-shell gain knob UI is not removed in this PRD; that removal is performed by PRD-0060, which composes the channel strip into the deck layout.
7. MIDI input writes coming through the binding registry (PRD-0044) reach the channel strip through the same ValueTree path, hit the same atomic-snapshot mechanism, and therefore inherit the same smoothing. No bypass path exists.

## 1.4. Acceptance Criteria

- [ ] `ChannelStripProcessor` exposes a functional gain stage that multiplies its input by a linear amplitude derived from `mixer.channel.{A,B,C,D}.gain` (dB), using `linear = pow(10, dB / 20.0)` with `dB <= -60` treated as -inf (linear 0.0). The mapping is identical to PRD-0010.
- [ ] `ChannelStripProcessor` exposes a functional fader stage that multiplies its (post-EQ/kills/filter) signal by a linear amplitude read directly from `mixer.channel.{A,B,C,D}.fader`, with no dB conversion. Range is `[0.0, 1.0]`; default is `1.0`.
- [ ] Gain dB values are clamped to `[-inf, +12.0]` before the dB→linear conversion. Fader values are clamped to `[0.0, 1.0]` on read.
- [ ] Both stages use per-sample smoothing with a 7 ms ramp length derived from `sampleRate` at `prepareToPlay`. The smoother state lives in the channel strip object and is preserved across `processBlock` calls.
- [ ] When the gain value changes by 24 dB instantaneously (e.g. UI snap from 0 dB to -24 dB), no click or step discontinuity is audible at the channel strip output. Equivalent test applies to fader changes from 1.0 to 0.0.
- [ ] When both gain and fader smoothers are ramping concurrently, the effective channel multiplier is the product of their two instantaneous values. No special coordination logic is required and none is introduced.
- [ ] The stages contribute no more than one sample of latency to the channel strip output relative to the input.
- [ ] The order of DSP stages inside `ChannelStripProcessor` is `gain → EQ → kills → filter → fader`. EQ / kills / filter are pass-through in this PRD but are wired in the correct position so sibling PRDs slot in without re-ordering.
- [ ] The audio engine no longer applies the legacy per-deck gain multiplier inside its summing loop. The summing loop performs only the A/B-bus accumulation and the master-gain / hard-clip stage already defined by PRD-0053 and PRD-0002.
- [ ] The deck-shell gain knob defined in PRD-0010 continues to function: dragging it changes the channel strip's gain stage output, and its ValueTree write path is unchanged. Its UI is not deleted in this PRD.
- [ ] Loading a new track on a deck does not reset `mixer.channel.{X}.gain` or `mixer.channel.{X}.fader`. Both values persist across track loads, matching PRD-0010 §1.4 (gain is deck-level state) and consistent with PRD-0001 (which resets pitch but not gain).
- [ ] All gain / fader audio-thread code paths (smoother evaluation, atomic loads, sample loop) perform no memory allocation (no `new`, `delete`, `malloc`, `std::vector::push_back`, `std::string`), take no locks, perform no file / network / logging I/O, and communicate with non-audio threads exclusively via `std::atomic` (and the ValueTree-fed snapshot atomics established by PRD-0052).
- [ ] MIDI input writes routed via the binding registry (PRD-0044) reach the channel strip through the same ValueTree → atomic → smoother chain. No bypass / instant-set path is exposed to MIDI or any other writer.
- [ ] At least one unit test in `Tests/` covers the dB→linear mapping (including the -60 dB → 0.0 floor and the +12 dB upper bound) and at least one covers fader-and-gain composition (their multiplicative product at known sample positions).

## 1.5. Grey Areas

### 1.5.1. Fader Taper: Linear Amplitude vs dB-Tapered vs Hybrid

A channel fader can be tapered three ways. Linear in amplitude (fader value `f` is itself the multiplier) gives `f = 0.5` ≈ -6 dB, which DJs migrating from analogue mixers find intuitive but compresses most musically useful adjustments (-12 to -3 dB) into the top third of fader travel. A dB-tapered fader spreads the useful range across the full travel but feels "wrong" near the bottom because crossing the -60 dB line audibly fades to silence over the last few percent rather than the last few millimetres. Hybrid CDJ-style tapers add an exponential curve near the bottom and a near-linear curve up top, which is ergonomically excellent but adds calibration burden and a non-obvious mapping that complicates the MIDI feedback engine (PRD-0047).

**Resolution:** Use linear amplitude taper. The fader value `f` is the multiplier directly, no curve, no dB conversion. This matches the contract literally encoded in `mixer.channel.{X}.fader ∈ [0, 1]` from PRD-0052 and is what every major hardware DJ mixer with motorised faders (Allen & Heath Xone, Pioneer DJM-A9, Behringer DDM4000) effectively presents to the user — they all expose a linear-in-amplitude curve at the channel fader, reserving dB-tapered curves for the master output knob. The price is that fine work at very low levels is awkward on the fader; that is acceptable because (a) the gain trim above the fader is the correct control for that scenario, and (b) DJs do not perform fine attenuation work at the bottom of a channel fader during a live set, they cut it.

### 1.5.2. Smoothing Time Constant Value

EPIC-0007 §1.3.6 specifies 5–10 ms for kill smoothing, with no explicit value for the continuous gain / fader smoothers. Shorter values (1–3 ms) feel "snappy" but can produce a faintly audible click on instantaneous full-range jumps. Longer values (15–30 ms) eliminate any audible artefact but introduce a perceptible lag between user gesture and audible response, which DJs notice as "rubbery" fader feel during sharp cuts or chop-cut transitions.

**Resolution:** Use 7 ms for both the gain and fader smoothers. This sits just under the threshold at which fader-feel becomes "lazy" during sharp cuts (~10 ms) and well above the threshold at which step changes become audible as clicks (~3 ms). Empirically 5–10 ms is what every cited reference DJ mixer's manual specifies for their digital-controlled VCAs, and 7 ms is the centre of that range. Both smoothers use the same value to keep behaviour predictable across simultaneous ramps; see §1.5.4.

### 1.5.3. Gain-vs-EQ Position in the Channel Chain

EPIC-0007 §1.2.1 specifies the chain `gain → EQ → kills → filter → fader`. The implementation could in principle swap gain and EQ (apply EQ first, then trim), which is what some Allen & Heath isolator-style mixers do, on the grounds that the EQ then operates on a more consistent input level. Doing so, however, makes the trim less useful for matching levels into the EQ's isolator cut region and breaks user expectations from Pioneer / Behringer hardware where trim is unambiguously the first stage.

**Resolution:** Lock the order as `gain → EQ → kills → filter → fader`, exactly as EPIC-0007 §1.2.1 specifies. The gain stage is the first DSP stage inside the channel strip, immediately after the deck DSP output. The fader stage is the last DSP stage inside the channel strip, immediately before the channel-strip output is handed to the A/B-bus accumulator of PRD-0053. This PRD wires the gain and fader stages in those positions; sibling PRDs (PRD-0055, PRD-0056) slot into the pass-through positions between them.

### 1.5.4. Simultaneous Gain and Fader Smoothing

When the DJ moves the gain trim and the channel fader at the same time (common during a transition), both smoothers ramp concurrently. The effective channel multiplier is the product of the two instantaneous smoother outputs. There is a theoretical concern that two concurrently-ramping smoothers could produce a small, smooth, non-monotonic excursion in the product if both targets cross. This is inaudible in practice for ramps of 7 ms.

**Resolution:** No special handling. The product `gain * fader` is computed sample-by-sample inside the channel-strip sample loop; both smoothers advance independently; the product is whatever it is at each sample. This is mathematically the correct behaviour and matches what every analogue mixer with cascaded VCAs does. Adding co-ordination logic (e.g. freezing one ramp while the other completes) would introduce surprising behaviour and is rejected.

### 1.5.5. Gain and Fader Values on Track Load

PRD-0001 specifies that loading a new track resets the pitch (`speedMultiplier = 1.0`) but does not specify the behaviour of gain or fader. PRD-0010 §1.4 explicitly states gain is deck-level state and persists across track loads. The mixer's channel fader is a mixer-level control, not deck-level, and should clearly persist for the same reasons.

**Resolution:** Both `mixer.channel.{X}.gain` and `mixer.channel.{X}.fader` persist unchanged across track-load events on the corresponding deck. A track load resets only pitch and transport state, per PRD-0001 and PRD-0010. This is the only behaviour consistent with how DJs use a mixer: the channel level set up for the next track in cue must not change when the track actually loads in.

### 1.5.6. MIDI Input Bypassing Smoothing

A controller may emit instantaneous value jumps (e.g. snap-back of a motorised fader, or a "set" message from a snapshot recall). One could imagine a fast path that writes directly into the smoother's current value (`setCurrentAndTargetValue`) so the new value is reached in one sample, on the theory that the controller already represents the user's settled intent.

**Resolution:** No bypass path is exposed. All writes — UI drag, MIDI value change, sync engine, future preset recall, programmatic test code — go through the same `mixer.channel.{X}.{gain,fader}` ValueTree property and are picked up by the same `setTargetValue` call on the smoother. Instantaneous (1-sample) target jumps are not exposed. The reason is twofold: every audible click in a mixer eventually traces back to some "fast path" someone added for a good reason, and 7 ms is short enough that even snapshot-recall feels instantaneous to the user. If a future requirement (e.g. emergency mute) demands a true zero-latency snap, it will be added as a separate, clearly named DSP stage rather than as a back-door into these smoothers.
