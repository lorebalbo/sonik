---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0003
  - PRD-0021
  - PRD-0079
---

# 1. PRD-0080: Clip Streaming Source-Read Layer

## 1.1. Problem

EPIC-0010's render engine (PRD-0081) must turn the recorded arrangement into
audible audio by reading each clip's source crop and summing the active lanes
into the master output, sample-accurately and click-free, inside `processBlock`.
But the source audio lives on disk — FLAC/MP3/WAV/AIFF originals decoded by
PRD-0003, and Instrumental/Vocal stems persisted by EPIC-0002's stem cache as
32-bit float WAV under `~/Library/Caches/Sonik/Stems`. Reading any of that from
the audio thread is forbidden by `CLAUDE.md`: no disk I/O, no allocation, no
locks in the real-time path. A naïve approach — decode the whole arrangement to
RAM up front — does not scale: a 90-minute recorded set across four decks and
three lanes (Original / Instrumental / Vocal) per deck could reference dozens of
multi-minute source files at full resolution, and holding every sample of every
referenced source in memory simultaneously is wasteful for clips the playhead
may never reach.

The render engine therefore needs a layer between "clip references a source
crop on disk" and "audio thread copies project-rate samples." That layer must
pre-roll source audio *ahead* of the playhead on background threads, into
pre-allocated lock-free ring buffers, so that by the time the playhead arrives
at a clip the samples are already resident and the audio thread does nothing but
copy. It must re-prime those buffers off the audio thread whenever the user
seeks, scrubs, or edits a clip (any of which invalidates the pre-rolled data).
It must never allocate when playback starts or stops — the streamers are a
pooled, pre-allocated resource. And because a clip's source may be at 96 kHz
while the project runs at 44.1 kHz, the layer must resample to the project rate
*during the background read* so the audio thread is never handed off-rate
samples. Without this layer, the render engine cannot exist within the
real-time contract.

## 1.2. Objective

The system provides a streaming source-read layer
(`Source/Features/Daw/Playback/ClipStreamer.h/.cpp`) that serves clip audio to
the render engine without any disk I/O on the audio thread, such that:

- Each active clip in the published arrangement snapshot (PRD-0079) is bound to
  a **ClipStreamer** instance drawn from a pre-allocated **pool**. The streamer
  owns a pre-allocated, lock-free ring buffer (`juce::AbstractFifo` + a backing
  `AudioBuffer<float>` sized at construction) into which a background reader
  thread continuously pre-rolls project-rate samples ahead of the playhead.
- The audio thread interacts with a streamer through one real-time-safe call
  only — `readInto(AudioBuffer<float>& dest, int destStartSample, int numSamples)`
  — which copies already-resampled, project-rate samples out of the ring buffer.
  This call performs no allocation, takes no lock, and performs no I/O; if the
  buffer is underrun (samples not yet ready) it outputs silence for the
  starved region and flags the underrun via an atomic counter for diagnostics.
- The background reader sources samples from either the **Original** file (via
  PRD-0003's decode infrastructure / `AudioFormatReader`) or the **Instrumental
  / Vocal** stem (via the EPIC-0002 stem cache, see §1.5.6), selected per the
  clip's lane, reads from the clip's source crop window
  `[sourceStartSample, sourceEndSample)`, resamples to the project rate when the
  source rate differs (§1.3 step 5), and writes the result into the ring buffer.
- **Pre-roll lead** is maintained: the reader keeps the ring buffer filled to a
  target lead time (the buffer capacity in seconds, see §1.5.1) ahead of the
  audio thread's read position, refilling as the audio thread drains it. The
  reader runs on a dedicated background thread (or shared reader thread-pool,
  see §1.5.2) — never on the audio thread or the message thread.
- **Re-prime on discontinuity**: any seek, scrub, or edit that changes the
  playhead position or a clip's crop window triggers a re-prime of the affected
  streamers — performed entirely off the audio thread. A re-prime resets the
  ring buffer read/write pointers, recomputes the source read position from the
  new playhead, and refills the lead. The audio thread observes the re-prime via
  an atomic "generation" handshake (§1.3 step 6); it never blocks on the reader.
- **No audio-thread allocation on transport changes**: starting, pausing, or
  stopping DAW playback (PRD-0082) acquires/releases streamers from the pool but
  never allocates ring buffers or decoder state on the audio thread. The pool is
  sized for the worst case (§1.5.2) and fully constructed before playback.
- **Sample-rate reconciliation**: a high-quality resampler (see §1.5.3) inside
  each streamer's reader converts source-rate samples to project-rate samples
  during the background read. The project rate is fixed per session and obtained
  from the master-grid service (EPIC-0008); the audio thread always receives
  project-rate samples and performs no rate conversion.
- **Missing-source resilience**: if a clip's source file is missing (the
  EPIC-0008 E12 relocation state), the streamer enters a placeholder mode and
  feeds silence at the project rate (§1.5.5) rather than failing — the render
  engine keeps running and the clip is visibly flagged elsewhere.

Out of scope: the audio-thread summing of streamer outputs into the master
(PRD-0081), arrangement snapshot compilation (PRD-0079), the DAW transport
itself (PRD-0082), and clip editing commands (a later PRD).

## 1.3. Developer / Integration Flow

1. PRD-0079's arrangement compiler publishes an immutable playback schedule:
   sorted clip events per lane, each carrying a **source-read handle** (file
   path or stem-cache key, lane, `sourceStartSample`, `sourceEndSample`, source
   sample rate, clip gain, `timelineStartSample`). This PRD consumes those
   handles; it does not build the schedule.
2. A message-thread **ClipStreamerPool** is constructed at engine init with `N`
   pre-allocated `ClipStreamer` instances (§1.5.2). Each streamer pre-allocates
   its ring buffer (`AudioBuffer<float>` sized to the pre-roll capacity at the
   project rate, stereo) and a re-usable `LagrangeInterpolator` (or chosen
   resampler) plus scratch read buffers. No streamer allocates after this point.
3. When the schedule changes (recompile) or playback is armed, a **message-thread
   binding step** assigns streamers from the pool to the clips that fall within
   the look-ahead horizon of the current playhead. Each bound streamer is handed
   its clip's source-read handle and an initial source read position derived from
   the playhead. Streamers for clips outside the horizon are returned to the
   pool. This binding runs on the message thread; the audio thread reads the
   resulting streamer set via the SeqLock-published snapshot (PRD-0079).
4. Each bound streamer's **background reader** opens (or reuses) an
   `AudioFormatReader` for the Original file (PRD-0003 decode path) or a reader
   over the stem-cache 32-bit float WAV (EPIC-0002), seeks to the source read
   position within `[sourceStartSample, sourceEndSample)`, reads a block of
   source-rate samples, and writes resampled project-rate samples into the ring
   buffer until the lead target is met. It then sleeps/waits until the audio
   thread drains enough to warrant another refill (condition-variable wakeups on
   the *background* side only — never on the audio thread).
5. **Sample-rate reconciliation** happens here: if the source rate ≠ project
   rate, the reader feeds source samples through the resampler at ratio
   `sourceRate / projectRate` and writes the project-rate output to the ring
   buffer. If the rates match, samples are copied through without resampling.
   The reader respects the crop end: when the source read position reaches
   `sourceEndSample`, the streamer feeds silence (clip has ended) or is unbound
   by the next message-thread recompile.
6. **Audio thread** (driven by PRD-0081): for each active clip in the snapshot,
   the render engine calls `streamer.readInto(dest, offset, numSamples)`. The
   streamer copies `numSamples` of project-rate audio from its ring buffer. A
   per-streamer atomic **generation counter** lets the audio thread detect a
   re-prime in flight: if the generation observed at read time does not match
   the snapshot's expected generation, the streamer outputs silence for that
   block (the re-prime has not yet completed) rather than serving stale samples.
7. **Re-prime path**: on seek/scrub (PRD-0082) or edit (later PRD), the
   message-thread binding step bumps each affected streamer's generation,
   recomputes its source read position from the new playhead, and signals its
   background reader to flush the ring buffer and refill from the new position.
   The audio thread, seeing the bumped generation, emits a short anti-click ramp
   to silence (PRD-0081 owns the ramp) until the new generation's samples are
   ready, then resumes copying.
8. A **missing-source** streamer (source path unresolved, EPIC-0008 E12) skips
   reader setup entirely and its `readInto` returns project-rate silence; an
   atomic `placeholder` flag is exposed so the UI / diagnostics can surface it.
9. Streamers expose atomic diagnostics (underrun count, fill level, placeholder
   flag, current generation) readable from any thread without locking, for the
   render-engine health metrics and tests.

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Daw/Playback/ClipStreamer.h/.cpp` exists and defines a
      `ClipStreamer` plus a `ClipStreamerPool`, both constructed and fully
      pre-allocated on the message thread before any playback begins.
- [ ] Each `ClipStreamer` owns a pre-allocated lock-free ring buffer
      (`juce::AbstractFifo` over a fixed-size stereo `AudioBuffer<float>`) sized
      to the pre-roll capacity at the project rate; no ring buffer is allocated,
      resized, or freed after construction.
- [ ] `ClipStreamer::readInto(AudioBuffer<float>& dest, int destStartSample, int numSamples)`
      is real-time-safe: it performs no heap allocation, takes no lock, performs
      no disk I/O, and uses only `std::atomic` / `juce::AbstractFifo` for
      cross-thread coordination. A test verifies it is allocation-free under a
      JUCE allocation sentinel (or equivalent) across a full block read.
- [ ] The background reader runs on a dedicated background thread (or a shared
      reader thread-pool, per §1.5.2) and pre-rolls project-rate samples into the
      ring buffer ahead of the audio thread's read position, maintaining the
      configured pre-roll lead (§1.5.1).
- [ ] The reader sources Original-lane audio via the PRD-0003 decode path
      (`AudioFormatReader`) and Instrumental/Vocal-lane audio from the EPIC-0002
      stem cache (32-bit float WAV under `~/Library/Caches/Sonik/Stems`),
      selected by the clip's lane in the source-read handle.
- [ ] The reader honours the clip crop window `[sourceStartSample, sourceEndSample)`:
      reading begins at the source position mapped from the playhead and stops at
      `sourceEndSample`; samples outside the crop are never emitted.
- [ ] Sample-rate reconciliation: when a source's sample rate differs from the
      project rate, the reader resamples to the project rate during the
      background read using a high-quality resampler (§1.5.3); when the rates
      match, no resampling is applied. The audio thread always receives
      project-rate samples and performs no rate conversion. A test loads a
      48 kHz source into a 44.1 kHz project and asserts the streamed output is at
      the project rate with correct duration.
- [ ] Streamers are pooled and pre-allocated: a `ClipStreamerPool` holds `N ≥`
      the worst-case concurrent clip count (§1.5.2). Acquiring and releasing a
      streamer on transport start/stop performs no allocation; a test starts and
      stops playback repeatedly and asserts zero audio-thread allocations.
- [ ] Seek / scrub / edit triggers a re-prime performed entirely off the audio
      thread: the streamer's generation counter is bumped, its ring buffer is
      flushed, and the background reader refills from the recomputed source
      position. The audio thread observes the generation mismatch and outputs
      silence (PRD-0081 ramps it) until the new generation is ready.
- [ ] A buffer underrun (audio thread reads faster than the reader fills) outputs
      silence for the starved region and increments an atomic underrun counter;
      it never blocks the audio thread and never produces a glitch beyond the
      missing samples.
- [ ] A missing source file (EPIC-0008 E12 relocation state) puts the streamer in
      placeholder mode: `readInto` returns project-rate silence, an atomic
      `placeholder` flag is set, and no reader thread spins on a non-existent
      file. A test binds a streamer to a missing path and asserts silent,
      non-crashing, allocation-free playback.
- [ ] Streamer diagnostics (underrun count, fill level, placeholder flag,
      current generation) are exposed as atomics readable from any thread without
      locking.
- [ ] No summing, mixing, or master-output logic is added by this PRD; the layer
      only delivers per-clip project-rate samples on request. Summing is
      PRD-0081's responsibility.
- [ ] No arrangement-snapshot compilation is added by this PRD; the layer
      consumes the source-read handles published by PRD-0079.
- [ ] No DAW transport (play/pause/stop/seek) is added by this PRD; the layer
      exposes the re-prime entry point that PRD-0082's transport calls on seek.

## 1.5. Grey Areas

### 1.5.1. Ring Buffer Size and Pre-Roll Lead Time

A streamer's ring buffer capacity is the maximum audio it can hold ahead of the
playhead. Too small and a momentary scheduler stall or disk hiccup underruns;
too large and `N` pooled streamers consume excessive RAM (e.g., 48 streamers ×
2 s stereo at 96 kHz ≈ 70 MB, manageable; at 8 s ≈ 280 MB, wasteful).

**Resolution:** Default to a **2-second** stereo pre-roll capacity per streamer,
with the reader maintaining at least **1 second** of lead ahead of the audio
thread before reporting "ready." Two seconds comfortably absorbs background
decode jitter and OS disk latency for the file sizes EPIC-0010 targets, while
keeping the pool's worst-case memory footprint modest. The capacity is a single
named constant (`kPreRollSeconds`) so it can be tuned per platform if profiling
shows underruns on slower disks; it is fixed per session (no dynamic resizing,
which would allocate). Hi-res 192 kHz projects pay more RAM per second but the
2 s default still bounds the pool to well under half a gigabyte.

### 1.5.2. Streamer Pool Size vs Max Concurrent Clips

The pool must cover the worst case: 4 decks × 3 lanes (Original / Instrumental /
Vocal) = 12 simultaneously audible source reads, plus overlaps at clip
boundaries where an outgoing clip's anti-click tail coexists with an incoming
clip on the same lane (doubling some lanes briefly), plus a safety margin.

**Resolution:** Pre-allocate a pool of **24 streamers** (12 steady-state × 2 for
boundary overlaps), all constructed at engine init. This bounds memory
(24 × 2 s default ≈ 35 MB at 96 kHz) and guarantees no allocation when a new
clip becomes active. If the binding step (§1.3 step 3) ever needs more streamers
than the pool holds (pathological dense-edit arrangements), it prioritises clips
nearest the playhead and lets distant clips bind lazily as earlier ones free up;
the look-ahead horizon (a few seconds) ensures near-playhead clips always win.
The reader threads are a **shared fixed-size thread-pool** (e.g., 4 worker
threads servicing all 24 streamers via a work queue), not one OS thread per
streamer — 24 dedicated threads would waste scheduler resources, while a small
pool keeps every active streamer's lead topped up round-robin.

### 1.5.3. Resampler Choice and Quality

Candidates: `juce::ResamplingAudioSource` (wraps an interpolator with a pull
model), a bare `juce::LagrangeInterpolator` (used by PRD-0003 for load-time
resampling), `juce::WindowedSincInterpolator` (higher quality, more CPU), or an
external library (Rubberband — overkill, that's a time-stretcher not a plain
resampler).

**Resolution:** Use **`juce::LagrangeInterpolator`** per channel, matching the
resampler PRD-0003 already uses for load-time conversion, so the project has one
consistent resampling quality story and no new dependency. The interpolator is
allocated once per streamer at construction (it holds a small fixed filter
state), reset on every re-prime, and driven by the background reader — never the
audio thread. Lagrange is high enough quality for sample-rate reconciliation of
already-mastered source material at the small ratios common here (48→44.1,
96→44.1). If future profiling or golden-ear review finds Lagrange insufficient
for extreme ratios, the resampler is swapped behind the streamer's internal
interface without touching the audio-thread contract; `WindowedSincInterpolator`
is the drop-in upgrade. `ResamplingAudioSource` is rejected because its pull
model and internal buffering fight the explicit ring-buffer design here.

### 1.5.4. Re-Prime Latency on Seek: Brief Silence vs Blocking

When the user seeks, the pre-rolled ring buffers are stale; the new samples are
not yet read. Options: (a) the audio thread blocks until the reader refills
(forbidden — that's a lock/wait on the audio thread), (b) the audio thread emits
brief silence (a few ms while the reader primes the new position) then resumes,
(c) keep a tiny synchronous read on the message thread to pre-fill a few blocks
before unblocking the audio thread.

**Resolution:** Option **(b)** — brief silence, ramped. On seek, the
message-thread re-prime bumps the generation and kicks the reader; the audio
thread, detecting the generation mismatch, emits an anti-click ramp to silence
(PRD-0081 owns the ramp shape) and keeps emitting silence until the streamer
reports the new generation's lead is met (typically a few milliseconds for a
warm decoder, longer for a cold file open). Blocking the audio thread (a) is
non-negotiably forbidden. A synchronous message-thread pre-fill (c) is a
possible latency optimisation for the *first* block after a seek, but it risks
stalling the message thread on a slow disk and is deferred; the brief-silence
model is correct, simple, and within the real-time contract. Seeks during DAW
review are not performance-critical the way live deck cueing is, so a few
milliseconds of silence on seek is acceptable.

### 1.5.5. Missing Source File (E12 Relocation): Placeholder Silence

A clip may reference a source file that has moved or been deleted (EPIC-0008's
E12 relocation state). The streamer cannot read it. Options: (a) fail the whole
render, (b) feed placeholder silence and flag the clip, (c) attempt
auto-relocation from the reader thread.

**Resolution:** Option **(b)** — placeholder silence. A streamer bound to an
unresolved source sets an atomic `placeholder` flag, skips all reader/decoder
setup, and returns project-rate silence from `readInto`. The render engine keeps
running and the rest of the arrangement plays normally; the missing clip is
surfaced visually by the timeline UI (a separate concern) reading the same
`placeholder` flag or the EPIC-0008 relocation state. Auto-relocation (c) is a
library-layer / message-thread concern (and exists elsewhere in Sonik's missing
-file handling), not something the real-time read layer should attempt from a
reader thread. Failing the whole render (a) is hostile: one missing file should
never silence a whole recorded set.

### 1.5.6. Stem-Cache WAV: Streamed From Disk vs Held in RAM

EPIC-0002's stem separation produces Instrumental/Vocal buffers and persists
them as 32-bit float WAV under `~/Library/Caches/Sonik/Stems`; EPIC-0002 also
holds freshly-separated stem buffers in RAM. A clip on the Instrumental/Vocal
lane could either re-read the cached WAV from disk through the streamer (like any
other source) or reference the in-RAM stem buffer directly (zero disk I/O, but
only valid while EPIC-0002 still holds it).

**Resolution:** **Stream the stem-cache WAV from disk** through the same reader
path as Original sources, treating the 32-bit float WAV as just another
`AudioFormatReader` source. This is the only approach that is uniform and
correct across sessions: the in-RAM stem buffers from EPIC-0002 are a transient,
deck-scoped resource tied to the live separation, not a durable arrangement-wide
store, and a recorded set replayed in a later session (or after the deck that
produced the stem is unloaded) must still find its stem audio — which only the
on-disk cache guarantees. Streaming from the cache WAV also keeps the streamer
implementation uniform (one reader path, one resampling path) rather than
forking into an "in-RAM stem" special case. An *optimisation* — when the live
in-RAM stem buffer is provably still resident and matches the clip's source
key, bind the streamer to it and skip the disk read — is deferred as a future
enhancement; it is a pure performance win layered behind the same `readInto`
contract and is not required for correctness.

### 1.5.7. Shared Streamer When Two Clips Reference the Same Source Region

Two clips on different lanes (or a split that produced two clips from one
source) may reference overlapping or identical source regions of the same file
at the same playhead time. Binding a separate streamer (and separate decoder +
ring buffer) to each duplicates disk reads and memory.

**Resolution:** For the initial implementation, **bind one streamer per active
clip** (no sharing) — simple, correct, and within the pool budget (§1.5.2),
since the worst-case clip count already accounts for all 12 lanes. De-duplicating
streamers across clips that read the identical source region at the same time is
a worthwhile optimisation but introduces shared-ownership and lifetime
complexity (which clip's re-prime governs the shared buffer?) that is not worth
the cost before profiling proves duplicate reads are a bottleneck. The split case
(one source, two contiguous non-overlapping crops) reads *different* regions
anyway and gains nothing from sharing. If profiling later shows identical-region
duplication is common (e.g., a clip auditioned on both Original and a stem lane
simultaneously), a content-keyed streamer cache can be introduced behind the
binding step without changing the audio-thread `readInto` contract.
