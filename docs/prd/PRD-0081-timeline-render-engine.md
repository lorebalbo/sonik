---
status: Not Implemented
epic: EPIC-0010
depends-on:
  - PRD-0058
  - PRD-0079
  - PRD-0080
---

# 1. PRD-0081: Timeline Render Engine

## 1.1. Problem

EPIC-0010 must make the recorded arrangement audible. Two foundations now exist: PRD-0079 compiles the message-thread `daw` `ValueTree` into a compact, immutable playback schedule and publishes it to the audio thread via a SeqLock double-buffer, and PRD-0080 pre-rolls each active clip's source crop (Original file via PRD-0003 decoders, or Instrumental/Vocal from the EPIC-0002 stem cache) into pre-allocated lock-free ring buffers, resampled to the project rate. But nothing yet turns those two foundations into sound: there is no component inside the `processBlock` chain that, for the current transport block, asks the schedule which clips are active, copies their samples out of the streamer ring buffers, applies per-clip gain, and sums the active lanes into the master output.

This is the single most safety-critical component in the Epic. It runs entirely on the real-time audio thread and must obey `AGENTS.md` (§"The Audio Thread") without exception: no allocation, no locks, no I/O, no walking of the `ValueTree`, and cross-thread communication exclusively through `std::atomic` or the lock-free structures the upstream PRDs publish. It must also be click-free: clip starts, clip ends, and edit-induced discontinuities (a clip that just moved, or a schedule that just recompiled mid-playback) must not produce audible pops. EPIC-0010 §1.3.1 mandates short (e.g. 64-sample) anti-click ramps at every boundary, consistent with EPIC-0002's crossfade approach. Without this PRD the arrangement is a silent data model; with it, the DJ hears their reconstructed set through the same master output stage (PRD-0058 / EPIC-0007) the decks already use.

## 1.2. Objective

The system provides `TimelineRenderer`, a real-time-safe per-block player living at `Source/Features/Daw/Playback/TimelineRenderer.h/.cpp`, such that:

- `TimelineRenderer::renderBlock` runs inside the `processBlock` chain and, per audio block, reads the SeqLock-published `ArrangementSnapshot` (PRD-0079) once into a coherent local view, determines which clips on which lanes overlap the current transport playhead range `[playhead, playhead + numSamples)`, and produces a summed stereo block.
- For each active clip it copies the corresponding source samples from that clip's `ClipStreamer` ring buffer (PRD-0080) at the streamer's current read position, applies the clip's per-clip gain (a linear multiplier resolved from the snapshot), and accumulates the result into the renderer's internal lane-sum buffer.
- All active lanes are summed into a single stereo bus that feeds the existing master output stage (PRD-0058): the renderer's output is the input to `MasterStage` exactly as the crossfader bus is for the decks, so the arrangement plays through the same output path, metering, and hard-clip safety net.
- Click-free boundaries: a short fixed-length ramp (64 samples, see §1.5.5) is applied at every clip start, clip end, and any block where the active-clip set for a lane changes versus the previous block (an edit-induced or schedule-recompile discontinuity). Ramps are computed from pre-allocated state; no ramp ever allocates.
- The transport playhead position is supplied to the renderer per block as a single sample-accurate value (an `int64` sample count) read from the atomic the DAW transport (PRD-0082) publishes; this PRD consumes that atomic and does not own play/pause/stop control (see §1.5.3).
- When the arrangement is empty, or no clip overlaps the current block, the renderer outputs silence (a cleared buffer) with no special-casing beyond the normal sum-of-zero-lanes (§1.5.4).
- If a clip's streamer reports an underrun (the ring buffer does not contain the samples the renderer asked for), the renderer substitutes silence for that clip's contribution for the affected samples and records the underrun for off-thread logging; it never blocks, never reads disk, and never logs from the audio thread (§1.5.7).
- Every audio-thread path in `TimelineRenderer` performs no allocation, takes no lock, performs no I/O, and never reads the `ValueTree`; all buffers (lane-sum scratch, ramp state, per-clip read cursors) are pre-allocated in a `prepare(double sampleRate, int blockSize, int maxLanes, int maxClipsPerLane)` hook.

## 1.3. Developer / Integration Flow

1. `TimelineRenderer::prepare(double sampleRate, int blockSize, int maxLanes, int maxClipsPerLane)` pre-allocates: one stereo lane-sum scratch buffer sized `blockSize`, one stereo accumulation buffer for the final master-feed output sized `blockSize`, a fixed-capacity array of per-clip read state (cursor, last-applied-gain, ramp phase) sized `maxLanes * maxClipsPerLane`, and the 64-sample ramp coefficient table (computed once: a linear or equal-power fade, see §1.5.5). Nothing here is touched again on the audio thread except by value.
2. Each block, the host audio callback calls `TimelineRenderer::renderBlock(juce::AudioBuffer<float>& masterFeed, int numSamples)`. The renderer first clears `masterFeed`.
3. The renderer reads the playhead: `const int64 playhead = transportPlayhead.load(std::memory_order_acquire);` where `transportPlayhead` is the `std::atomic<int64_t>` published by PRD-0082. The block covers timeline samples `[playhead, playhead + numSamples)`.
4. The renderer acquires a coherent view of the schedule by reading the PRD-0079 SeqLock snapshot: it retries the read if the sequence counter changed mid-read (the SeqLock contract), copying only POD handles (lane index, clip start/end timeline sample, source-read handle, gain) into a local fixed-capacity view — never dereferencing the `ValueTree`. The snapshot is already sorted per lane by `timelineStartSample` (PRD-0079), so overlap resolution is a forward scan.
5. For each lane in the snapshot, the renderer finds the clip(s) whose `[timelineStartSample, timelineEndSample)` intersects the block range. For each such clip it computes the source-read offset (how far into the clip the playhead currently is) and asks that clip's `ClipStreamer` (resolved by the snapshot's source-read handle) to copy `count` samples from its ring buffer into the lane-sum scratch, applying the per-clip linear gain during the copy.
6. Boundary ramps: if a clip begins partway through this block (its `timelineStartSample` falls inside the block), the renderer applies the 64-sample fade-in ramp starting at that offset. If a clip ends inside the block, it applies the 64-sample fade-out ramp ending at that offset. If the active-clip set for a lane differs from the previous block (detected by comparing a small per-lane fingerprint of active clip handles), the renderer cross-ramps: fade out the previous block's tail contribution and fade in the new one over 64 samples, mirroring EPIC-0002's anti-click crossfade.
7. Lane summation: each lane's scratch contribution is accumulated into `masterFeed` (a simple add). With `maxLanes` lanes this is a bounded loop; no per-lane allocation occurs. Summing headroom and gain staging are addressed in §1.5.2 (the renderer sums at unity and relies on the master gain + hard-clip safety net downstream).
8. The completed `masterFeed` buffer is handed to `MasterStage::process` (PRD-0058) by the owning processor: the arrangement now flows through the master gain smoother, the master meter tap, and the PRD-0002 hard-clip safety net, exactly as the deck/mixer path does.
9. Underrun handling: if a `ClipStreamer` copy reports fewer samples available than requested, the renderer zero-fills the remainder of that clip's contribution for the block and increments a relaxed `std::atomic<uint32_t>` underrun counter (per clip or global, see §1.5.7). A message-thread watcher (owned by PRD-0080's streaming layer or a small renderer-side poller) reads the counter off-thread and logs; the audio thread never logs.
10. A new unit/integration test file under `Tests/` (e.g. `TimelineRendererTests.cpp`) drives the renderer with a synthetic `ArrangementSnapshot` and stub `ClipStreamer` ring buffers filled with known signals, advances the playhead block by block, and asserts: correct sample-accurate clip placement, correct per-clip gain application, correct lane summation, click-free output at clip boundaries (no inter-sample step above a threshold), and silence for empty arrangements and underruns.

## 1.4. Acceptance Criteria

- [ ] `TimelineRenderer` exists at `Source/Features/Daw/Playback/TimelineRenderer.h/.cpp` with a `prepare(double sampleRate, int blockSize, int maxLanes, int maxClipsPerLane)` hook and a `renderBlock(juce::AudioBuffer<float>&, int numSamples)` entry point.
- [ ] `renderBlock` reads the PRD-0079 SeqLock-published `ArrangementSnapshot` using the SeqLock retry protocol and copies only POD handles into a fixed-capacity local view; it never dereferences or walks the `daw` `ValueTree`.
- [ ] The transport playhead is read once per block from a `std::atomic<int64_t>` published by PRD-0082 via `load(std::memory_order_acquire)`; the renderer does not own or mutate transport state.
- [ ] For each clip whose `[timelineStartSample, timelineEndSample)` overlaps the block range, the renderer copies the correct source samples from that clip's `ClipStreamer` ring buffer (PRD-0080) at the correct source-read offset and applies the clip's per-clip linear gain.
- [ ] All active lanes are summed into a single stereo bus, and that bus is fed to `MasterStage::process` (PRD-0058) as the input the master gain, master meter, and PRD-0002 hard-clip safety net operate on.
- [ ] A 64-sample anti-click ramp is applied at every clip start that falls inside a block and every clip end that falls inside a block; output exhibits no inter-sample discontinuity above the test threshold at those boundaries.
- [ ] When the active-clip set on a lane changes between consecutive blocks (edit, move, or schedule recompile mid-playback), the renderer cross-ramps over 64 samples (fade out old, fade in new); no click is produced at the recompile boundary.
- [ ] An empty arrangement (no clips) and any block with no overlapping clip produce a cleared (silent) `masterFeed` buffer with no allocation and no special branch beyond summing zero lanes.
- [ ] A `ClipStreamer` underrun causes the renderer to zero-fill the missing samples for that clip's contribution and increment a relaxed atomic underrun counter; the audio thread performs no logging, no allocation, and no blocking on underrun.
- [ ] All `TimelineRenderer` audio-thread paths perform no memory allocation, take no locks, and perform no disk/network/console I/O. The lane-sum scratch, accumulation buffer, per-clip read state, and ramp coefficient table are all pre-allocated in `prepare` and never resized on the audio thread.
- [ ] The renderer never reads the `daw` `ValueTree` on the audio thread; the only audio-thread inputs are the SeqLock snapshot (PRD-0079), the streamer ring buffers (PRD-0080), and the playhead atomic (PRD-0082).
- [ ] At least one test in `Tests/TimelineRendererTests.cpp` drives the renderer with a synthetic snapshot and stub streamers and verifies: (a) sample-accurate clip placement against the playhead, (b) per-clip gain applied correctly, (c) multiple lanes sum correctly, (d) no click at clip start/end and at a mid-playback schedule swap, (e) silence on empty arrangement, (f) silence (not garbage) on a forced streamer underrun.
- [ ] No transport control (play/pause/stop/seek), no snapshot compilation, no streaming/decoding, no editing, and no parameter automation is implemented by this PRD; those belong to PRD-0082, PRD-0079, PRD-0080, the edit-command layer, and EPIC-0011 respectively.

## 1.5. Grey Areas

### 1.5.1. Overlapping Clips on the Same Lane: Anti-Click Ramp vs Full Crossfade

Two clips on the same lane can overlap in timeline time (e.g. after a move that drags one clip's tail past the next clip's head). The renderer could (a) apply only the short anti-click ramp at each clip's boundary and let the overlap region simply sum both clips, or (b) compute a full equal-power crossfade across the entire overlap region.

**Resolution:** Short anti-click ramp only (option a). EPIC-0010 §1.2.2 explicitly defers "crossfades between overlapping clips on the same lane beyond the short anti-click ramp" to a future enhancement, and §1.3.1 mandates only the 64-sample ramp. In the overlap region both clips' samples are summed at unity (after their own per-clip gain), with each clip carrying its own 64-sample fade-in at its start and fade-out at its end. This is correct, click-free, and matches the Epic's scope. A future PRD may add user-drawn, full-length equal-power crossfades by widening the ramp into a per-overlap fade curve without changing this PRD's summing contract.

### 1.5.2. Summing Headroom and Gain Staging

Summing N lanes at unity gain can exceed 0 dBFS when multiple loud clips overlap. The renderer could (a) sum at unity and rely on the downstream master gain + hard-clip safety net, (b) apply an automatic `1/N` or `1/sqrt(N)` attenuation, or (c) apply a soft limiter inside the renderer.

**Resolution:** Sum at unity (option a). The renderer's job is faithful reconstruction; it must not silently alter levels, because the DJ recorded the arrangement at the levels they performed, and an automatic per-lane attenuation would make playback quieter than the performance and would change as clips are added/removed (a moving target). The existing master output stage (PRD-0058) already provides a master gain knob for the DJ to set headroom, and PRD-0002's hard-clip safety net catches genuine overage as a last resort. Summing at unity keeps gain staging explicit and under the DJ's control, exactly as the deck/mixer path already works. If clip counts grow large enough that overage becomes routine, a future PRD can add an optional, opt-in mix-bus trim — but never an automatic, silent one.

### 1.5.3. How the Playhead Position Reaches the Renderer

The renderer needs the current transport playhead each block, but transport play/pause/stop/seek control is owned by PRD-0082, not this PRD. The position could be passed (a) as a function argument computed by the caller, (b) read from a `std::atomic<int64_t>` the transport publishes, or (c) the renderer could advance its own internal counter.

**Resolution:** Read from a `std::atomic<int64_t>` published by PRD-0082 (option b). The renderer must not own playback state — that would couple it to transport semantics (loop region, pause-at-position, stop-resets-to-zero) that belong to PRD-0082. A single atomic playhead read with `memory_order_acquire` at the top of `renderBlock` gives a sample-accurate, lock-free hand-off and keeps the renderer a pure function of (snapshot, streamers, playhead). The renderer never advances the playhead itself; PRD-0082's transport advances it (typically by `numSamples` per block while playing) and the renderer is a read-only consumer. This keeps the two PRDs cleanly decoupled and testable in isolation (tests inject a playhead value directly).

### 1.5.4. Empty Arrangement and No-Clip Silence

When the arrangement is empty, or the playhead is in a region no clip covers, the renderer must produce silence. It could (a) special-case "no active clips" and skip work, or (b) treat it as the degenerate sum of zero lanes with a cleared buffer.

**Resolution:** Cleared-buffer sum of zero lanes (option b). `renderBlock` clears `masterFeed` first thing, then accumulates each active lane; if there are no active lanes, the buffer simply stays cleared and is handed downstream as silence. This needs no special branch, no early return that risks leaving a stale buffer, and no allocation. The master stage and hard-clip safety net run on silence harmlessly. This is the simplest correct behaviour and avoids a class of bugs where an early-return path forgets to clear the output and passes garbage to `MasterStage`.

### 1.5.5. Ramp Length (64 Samples) and Edit Discontinuities

EPIC-0010 §1.3.1 suggests a 64-sample ramp. The exact length and curve (linear vs equal-power) and whether edit-induced discontinuities use the same ramp are open.

**Resolution:** A fixed 64-sample ramp for all boundaries — clip start, clip end, and edit/recompile discontinuities — computed once in `prepare` into a coefficient table. 64 samples is ~1.45 ms at 44.1 kHz: long enough to remove the click, short enough to be inaudible as a fade and to preserve transient attacks at clip starts. The curve is equal-power (`sin`/`cos`) for the cross-ramp case (so an old-clip-to-new-clip swap holds constant power, matching EPIC-0002's crossfade), and the same table's rising/falling halves serve the simple fade-in/fade-out at isolated starts/ends. A fixed length (rather than a sample-rate-scaled one) keeps the coefficient table constant and the ramp logic branch-free; at higher sample rates 64 samples is shorter in time but still ample to suppress the discontinuity. If a future need arises (e.g. very low sample rates), the length can be made a `prepare`-time parameter without changing the audio-thread code.

### 1.5.6. Lane → Output Routing

Each lane could route to its own mixer channel (per-lane EQ/filter/fader) or all lanes could sum directly to the master. The Epic models multiple lanes but the mixer-routing question is open.

**Resolution:** All lanes sum directly to the master output in this Epic (the renderer produces one stereo bus feeding `MasterStage`). EPIC-0010's render engine "sums the active lanes into the master output" (§1.2.1, §1.3.7); per-lane mixer routing (sending each timeline lane through a `ChannelStripProcessor` with independent EQ/filter/fader) is not in this Epic's scope. The renderer therefore exposes a single summed bus, not per-lane buses. A future Epic that wants per-lane mixer control can change the renderer to emit one buffer per lane and route each through the mixer; this PRD's contract (sum to master) is the documented EPIC-0010 behaviour and the simplest correct substrate for automation (EPIC-0011) and export (EPIC-0012) to build on.

### 1.5.7. Behaviour When a Streamer Underruns

A `ClipStreamer` ring buffer may not contain the samples the renderer requests (background reader fell behind after a seek, a slow disk, or a just-primed streamer). The renderer must not block or read disk. Options: (a) output silence for the missing samples and log off-thread, (b) repeat the last available sample (hold), (c) stall the whole block.

**Resolution:** Silence for the missing samples plus an off-thread-logged underrun count (option a). Stalling the block (option c) is forbidden — the audio thread must never block, and a stall would glitch every lane, not just the underrunning clip. Holding the last sample (option b) produces an audible DC-ish artefact and is harder to reason about than a clean gap. Substituting silence for only the affected clip's contribution (the other lanes still play) is the least-bad audible outcome and is trivially real-time-safe: zero-fill the remainder of that clip's scratch contribution. The renderer increments a relaxed `std::atomic<uint32_t>` underrun counter; a message-thread poller (PRD-0080's streamer supervisor or a small renderer-side timer) reads and logs it off the audio thread, so underruns are diagnosable without any audio-thread I/O. Underruns should be rare in practice (PRD-0080 pre-rolls ahead of the playhead and re-primes on seek); when they occur the DJ hears a brief gap on one clip rather than a full-mix dropout.
