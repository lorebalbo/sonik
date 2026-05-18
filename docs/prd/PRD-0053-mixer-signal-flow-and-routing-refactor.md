---
status: Not Implemented
epic: EPIC-0007
depends-on:
  - PRD-0002
  - PRD-0052
---

# 1. PRD-0053: Mixer Signal-Flow and Routing Refactor

## 1.1. Problem

The current audio engine, defined by PRD-0002 (`docs/prd/PRD-0002-audio-engine-core.md`), sums each deck's per-deck linear-gain buffer directly into the stereo output bus. This direct-sum contract leaves no structural place for the per-channel signal chain a professional DJ mixer requires: trim, 3-band EQ with kills, filter, channel fader, A/B crossfader assignment, crossfader, and master gain with metering. Every sibling PRD in EPIC-0007 (channel-strip DSP in PRD-0054, EQ in PRD-0055, filter in PRD-0056, crossfader in PRD-0057, master stage and metering in PRD-0058) needs a real, in-place pipeline to plug into. Without that pipeline, those PRDs would each be forced to re-litigate where the summing stage lives, how buffers are owned, and how parameters are snapshotted on the audio thread — which would produce inconsistent contracts and almost certainly violate the audio-thread immutable rules in `AGENTS.md`.

The problem is also one of safety: introducing four channels, EQs, filters, a crossfader, and metering simultaneously with their DSP implementations carries a high risk of audible regression. PRD-0002's hard-clip safety net and the current bit-exact sum are working today and are trusted; any refactor that changes audible output before the DSP blocks are validated would destabilise the engine.

## 1.2. Objective

The system provides a structured audio-engine pipeline that replaces the direct per-deck sum from PRD-0002 with the signal flow declared in EPIC-0007 §1.2.1:
`decode → deck DSP → ChannelStripProcessor (gain → EQ → kills → filter → fader) → A/B bus → CrossfaderStage → MasterStage → hard-clip → output bus`.

In this PRD specifically:

- The pipeline is installed as a skeleton only. Every DSP block introduced (gain stage, EQ, band kills, filter, channel fader, A/B assign, crossfader, master gain, metering) is implemented as a real-time-safe pass-through identity transform.
- Audible output is bit-equivalent to the pre-refactor PRD-0002 sum, modulo the existing hard-clip safety net, which is preserved verbatim.
- The header-level class contracts for `ChannelStripProcessor`, `ABBus`, `CrossfaderStage`, and `MasterStage` exist under `Source/Features/Mixer/Routing/`, ready for sibling PRDs to fill in.
- All per-channel scratch buffers, master scratch buffers, and DSP state objects are pre-allocated at `prepareToPlay` time and never re-allocated on the audio thread.
- Per-block parameter reads from the ValueTree-backed atomics defined by PRD-0052 are wired in (snapshotted once per block into stack locals), even though the pass-through blocks ignore the values.
- PRD-0002's "sum per-deck buffers directly" clause is explicitly superseded by this PRD; PRD-0002's hard-clip safety net and atomic-gain contract remain authoritative.

## 1.3. Developer / Integration Flow

1. The audio device callback enters `AudioProcessor::processBlock` exactly as it does today.
2. Each deck's decode and deck-level DSP (transport, time-stretch, stems) runs as before, producing one stereo buffer per deck. This stage is unchanged by this PRD.
3. For each of the up to 4 channels (matching deck count), the engine invokes `ChannelStripProcessor::process(channelIndex, deckOutputBuffer, channelScratchBuffer, paramSnapshot)`. In this PRD, the channel strip copies the deck output into the channel scratch buffer unchanged (identity transform). If the channel has no deck assigned, the channel scratch buffer is zeroed and the strip is short-circuited (see §1.5.5).
4. Before invoking the strip, the engine reads every relevant parameter from the ValueTree-backed atomics published by PRD-0052 into a stack-local `ChannelStripSnapshot` struct (gain, eq high/mid/low, killHigh/killMid/killLow, filter, fader, assignA, assignB). Pass-through DSP in this PRD ignores the values, but the snapshot machinery exists, runs once per block, performs no allocation, takes no locks, and writes nothing back.
5. The engine invokes `ABBus::accumulate(channelScratchBuffer, assignA, assignB, busABuffer, busBBuffer)` per channel. In this PRD, regardless of the snapshot's `assignA`/`assignB` flags, every non-silent channel is summed into BOTH bus A and bus B with unity gain. This preserves audible parity with the pre-refactor direct sum (see §1.5.1).
6. The engine invokes `CrossfaderStage::process(busABuffer, busBBuffer, masterScratchBuffer, crossfaderSnapshot)`. In this PRD the stage sums A and B with unity gain into the master scratch buffer, which — combined with step 5 — produces a signal bit-equivalent to the previous direct sum (each deck contributes once to A and once to B, then A+B equals twice each deck; this is corrected by the `0.5` unity-equivalent scaling described in §1.5.1).
7. The engine invokes `MasterStage::process(masterScratchBuffer, outputBus, masterSnapshot)`. In this PRD the stage copies the master scratch buffer into the output bus unchanged (identity transform). No metering is computed; metering hooks are reserved for PRD-0058.
8. The hard-clip safety net from PRD-0002 runs on the output bus, after the master stage and before the buffer is returned to the device. This placement is unchanged in intent from PRD-0002 (see §1.5.2).
9. On `prepareToPlay(sampleRate, blockSize)`, the engine sizes and zeroes all per-channel scratch buffers (4 × stereo × blockSize), both A and B bus buffers (stereo × blockSize), and the master scratch buffer (stereo × blockSize). All DSP state objects within each `ChannelStripProcessor`, `CrossfaderStage`, and `MasterStage` are reset. No DSP-block allocation occurs on the audio thread (see §1.5.4).
10. On `releaseResources`, all scratch buffers are released. On a subsequent `prepareToPlay` with new sample-rate or block-size parameters, the buffers are re-allocated on the message thread before the audio callback resumes.

## 1.4. Acceptance Criteria

- [ ] A new directory `Source/Features/Mixer/Routing/` exists and contains header files declaring the classes `ChannelStripProcessor`, `ABBus`, `CrossfaderStage`, and `MasterStage` with public method signatures matching the integration flow in §1.3.
- [ ] `ChannelStripProcessor` is a plain real-time-safe C++ class. It is NOT a `juce::AudioProcessor` and NOT a `juce::dsp::ProcessorBase` subclass (see §1.5.3).
- [ ] `ChannelStripProcessor`, `ABBus`, `CrossfaderStage`, and `MasterStage` each expose a `prepareToPlay(double sampleRate, int blockSize, int numChannels)` (or equivalent) method that pre-allocates and zeroes every internal buffer and state object.
- [ ] The audio engine instantiates exactly one `ChannelStripProcessor` per channel (up to 4), one `ABBus`, one `CrossfaderStage`, and one `MasterStage`, owned via `std::unique_ptr` and constructed on the message thread.
- [ ] Per-channel scratch buffers, both A and B bus buffers, and the master scratch buffer are pre-allocated in `AudioProcessor::prepareToPlay` and reused across every `processBlock` call. No `new`, `malloc`, `std::vector::push_back`, `std::vector::resize`, `juce::AudioBuffer::setSize` (with `avoidReallocating=false`), or any other allocation occurs on the audio thread.
- [ ] A `ChannelStripSnapshot` POD struct is defined and populated once per block, per channel, by reading from the ValueTree-backed `std::atomic` values declared by PRD-0052. The snapshot is a stack local; it is not allocated on the heap.
- [ ] A `CrossfaderSnapshot` and a `MasterSnapshot` POD struct are likewise defined and populated once per block from PRD-0052 atomics.
- [ ] The audio-thread code paths added by this PRD allocate no memory, take no locks, perform no file/network/logging I/O, and communicate with non-audio threads exclusively through `std::atomic` reads or lock-free FIFOs.
- [ ] In pass-through mode, `ChannelStripProcessor::process` copies its input buffer into the channel scratch buffer with no audible modification (bit-equivalent samples).
- [ ] In pass-through mode, `ABBus::accumulate` adds every non-silent channel scratch buffer into BOTH `busA` and `busB` with unity gain, regardless of the snapshot's `assignA`/`assignB` flags. This routing rule is documented in code as a transitional behaviour owned by PRD-0053 and is the contract to be replaced by PRD-0057.
- [ ] In pass-through mode, `CrossfaderStage::process` produces `masterScratch[n] = 0.5 * (busA[n] + busB[n])` for every sample, which combined with the bus-A and bus-B duplication in the previous criterion yields a signal bit-equivalent to the direct per-deck sum from PRD-0002 (within floating-point associativity tolerance).
- [ ] In pass-through mode, `MasterStage::process` copies its input buffer into the output bus with no audible modification.
- [ ] If a channel has no deck assigned (no track loaded on the corresponding deck, or the deck index is out of range), the engine skips `ChannelStripProcessor::process` entirely for that channel and the channel scratch buffer is treated as silent by `ABBus::accumulate` (skipped, not zero-summed). See §1.5.5.
- [ ] The hard-clip safety net inherited from PRD-0002 runs on the output bus AFTER `MasterStage::process` and BEFORE the buffer is returned to the device callback. Its threshold, semantics, and ordering are preserved unchanged from PRD-0002.
- [ ] An automated unit test compares the output of the new pipeline (skeleton, pass-through) against a reference direct-sum of the same deck buffers for a set of representative input signals (silence, sine, two-deck mixed sines, near-clipping levels). Output samples must match within a floating-point tolerance of `1e-6` per sample.
- [ ] This PRD explicitly amends PRD-0002 (`docs/prd/PRD-0002-audio-engine-core.md`): the clause stating "sum the decks' per-deck-gain buffers directly into the stereo output bus" is superseded by the pipeline declared in EPIC-0007 §1.2.1 and skeletoned by this PRD. PRD-0002's hard-clip safety net and atomic-gain contract remain authoritative and are unchanged.
- [ ] No DSP-bearing parameter (EQ frequencies, filter cutoff, fader curve, crossfader curve, master gain taper, meter ballistics) is implemented in this PRD; those implementations belong to PRD-0054 through PRD-0058 and any attempt to implement them here is out of scope.
- [ ] All new files compile under the project's C++20 / CMake build with no warnings beyond the existing baseline, and the existing test suite continues to pass.

## 1.5. Grey Areas

### 1.5.1. Preserving Audible Parity During the Refactor

The new pipeline introduces an A/B bus and a crossfader stage before PRD-0057 is implemented. If the crossfader-assign flags in PRD-0052 default to "A only" or "B only", and the crossfader pass-through naively sums A and B at unity, the pre-refactor "every deck contributes equally to the master sum" behaviour would not hold. Two viable options exist: (a) route every channel to BOTH A and B with unity gain in `ABBus`, and have the crossfader pass-through sum `0.5 * (A + B)`; or (b) introduce a parallel "thru" path bypassing the A/B bus entirely until PRD-0057 lands.

**Resolution:** Adopt option (a). In `ABBus`, regardless of the snapshot's `assignA`/`assignB` values, every non-silent channel is summed into BOTH bus A and bus B at unity. In `CrossfaderStage::process`, the master scratch buffer is computed as `masterScratch[n] = 0.5 * (busA[n] + busB[n])`. This is algebraically equivalent to a centered crossfader with a linear pan law and produces a signal bit-equivalent to the pre-refactor direct sum. The advantages over option (b) are that the A/B bus pathway is the real production pathway, so we exercise it from day one; there is no second "thru" code path to remove and re-test later; and PRD-0057 can replace `CrossfaderStage::process` with its real curve and `ABBus::accumulate` with its real assign-flag honouring logic in one focused change, without first disabling a transitional bypass. The transitional both-buses routing is documented in code as a behaviour owned and reversed by PRD-0057.

### 1.5.2. Hard-Clip Placement

PRD-0002 specifies a hard-clip safety net on the output bus. The new pipeline introduces a master stage immediately before the output bus. The ambiguity is whether the hard-clip runs (a) inside `MasterStage` as its final operation, (b) outside `MasterStage` immediately after, or (c) only at the device-callback boundary as it does today.

**Resolution:** Keep the hard-clip exactly where PRD-0002 places it: on the output bus, after `MasterStage::process` returns and before the buffer is handed back to the device callback. The hard-clip is a safety net belonging to the audio engine, not to the mixer. Embedding it inside `MasterStage` would couple the master DSP to a global safety contract and make it harder for future PRDs (e.g. an alternate output target) to honour the same safety net. PRD-0002's clip threshold, semantics, and ordering are preserved verbatim. This PRD merely guarantees that the master stage's output passes through that same clip before reaching the device.

### 1.5.3. ChannelStripProcessor Class Shape

A channel strip could be modelled as a `juce::AudioProcessor` (giving it a parameter tree, prepare/release/process lifecycle, and host-style integration), as a `juce::dsp::ProcessorBase` subclass (lighter weight, JUCE DSP idioms), or as a plain real-time-safe C++ class.

**Resolution:** Use a plain real-time-safe C++ class. Sonik is not a plugin host, has no need to expose channel strips to a third-party host, has no need for `AudioProcessor` parameter automation (parameters live in the central `ValueTree`), and does not benefit from the `AudioProcessor`-imposed thread-safety and lifecycle overhead. `juce::dsp::ProcessorBase` is also unnecessary because the strip is composed of bespoke DSP blocks (EQ, kills, filter, fader) implemented in sibling PRDs, not a JUCE DSP graph. A plain class with explicit `prepareToPlay`, `process`, and `releaseResources` methods is the simplest contract, has the smallest footprint, and is the easiest to make audibly identical to the direct sum in pass-through mode.

### 1.5.4. Sample-Rate and Buffer-Size Changes Mid-Session

The audio device's sample-rate or buffer-size can change at runtime (user switches output device, system reconfigures Core Audio). The new pipeline must handle this without leaking pre-allocated buffers, without allocating on the audio thread, and without producing audible glitches beyond what the device transition itself produces.

**Resolution:** Follow the same pattern the rest of the engine already uses. JUCE's `AudioProcessor` lifecycle guarantees that `releaseResources` is called and then `prepareToPlay` is called again with the new parameters before the audio callback resumes. In `prepareToPlay`, the engine calls each pipeline stage's `prepareToPlay(sampleRate, blockSize, numChannels)`; each stage releases its current scratch buffers (`std::vector::clear` is acceptable because this runs on the message thread) and re-allocates them sized for the new block. No mid-callback resizing ever occurs. The audio thread cannot observe a partially-sized buffer because the device callback is paused for the duration of `prepareToPlay`. Each stage also resets any internal DSP state (smoothers, filter histories) to a known-zero starting point so the first block after a device change is deterministic.

### 1.5.5. CPU on Silent Channels

When a channel has no track loaded on its deck, running the full DSP graph on a silent buffer wastes CPU. Three options: (a) always run the DSP graph and let it process zeros; (b) memset the channel scratch to zero and run the DSP graph anyway (still wasted CPU, but symmetric); (c) short-circuit the channel entirely — skip the strip, and have `ABBus::accumulate` skip the channel rather than sum zeros.

**Resolution:** Adopt option (c). The audio engine already knows which decks have a buffer loaded (PRD-0002 tracks deck-level "has buffer" state). In `processBlock`, the engine queries this state once per channel per block and, if the channel has no source, it (i) does not invoke `ChannelStripProcessor::process` for that channel, (ii) does not zero or touch the channel scratch buffer, and (iii) signals `ABBus::accumulate` to skip the channel (e.g. via a "channelActive" bool array passed alongside the scratch buffers). This short-circuit is purely a CPU optimisation; it produces audible output identical to running the pass-through DSP on zeros and then summing zeros into the bus. When sibling PRDs (PRD-0054 onward) implement real DSP, they MUST preserve this short-circuit so that a four-deck-loaded session does not pay EQ/filter CPU for empty channels. The "channelActive" signal is sampled once per block to keep the check off the per-sample inner loop.
