---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0081
  - PRD-0092
  - PRD-0080
---

# 1. PRD-0099: Offline Render Driver

## 1.1. Problem

EPIC-0012 promises that the DJ can **export the finished mix** to an audio file
via an offline render that reuses the timeline engine (PRD-0081) and the
automation applier (PRD-0092). Everything needed to *play* the arrangement
already exists by the time this PRD is reached: the timeline render / clip
summing path (PRD-0081) mixes positioned, cropped, gain-staged clips into an
output buffer; the streaming clip reader (PRD-0080) feeds source samples to that
path; the automation applier (PRD-0092) evaluates continuous and boolean
breakpoints per block; and master-tempo automation (PRD-0089, evaluated through
PRD-0092) drives the grid so grid-aligned content follows the recorded tempo.

What does **not** yet exist is a way to run that whole stack **without the audio
device** — deterministically, faster than real time, on a background thread —
and capture the result into a buffer. The live path is pulled by the audio
hardware callback at a fixed rate; it tolerates underruns by dropping samples
and relies on the streaming reader's asynchronous prefetch to hide disk latency.
None of that is acceptable for an offline render: an export must be
**bit-identical to an ideal, glitch-free playback** of the same arrangement,
must never drop a block because a disk read was slow, and must complete as fast
as the machine allows rather than in real time.

This PRD builds the **offline render driver**: the deterministic block loop that
advances the playhead across the arrangement (or a selected region), pulls the
existing render engine and automation applier at each block with **synchronous,
fully-resolved** source reads, and hands the summed output to the exporter
(PRD-0100) as a buffer or a stream of blocks. It owns no file encoding, no UI,
and no live-playback concern — only the non-real-time driving of the engine.

## 1.2. Objective

The system provides an `OfflineRenderDriver`
(`Source/Features/Daw/Export/OfflineRenderDriver.h/.cpp`) such that:

- Given a render range (the whole arrangement or a selected region / loop) and a
  target sample rate, the driver produces a sample-accurate float buffer (or a
  sequence of float blocks) that is **bit-identical to an ideal real-time
  playback** of the same arrangement at the same rate, with no dropped or
  duplicated samples.
- The driver advances the playhead **block-by-block** over a deterministic loop,
  calling the existing PRD-0081 render engine and PRD-0092 automation applier at
  each block exactly as the live engine would, but **without** the live audio
  device and **without** blocking the message thread.
- Clip reads go through PRD-0080's streaming reader in a **synchronous,
  full-read** mode: each block waits for the required source samples to be
  available rather than returning silence on a prefetch miss, so the render never
  underruns.
- Per-block evaluation of clip gains, lane/group gains, and automation
  breakpoints (continuous and boolean) uses the **same** PRD-0092 applier code
  the live path uses, guaranteeing automation parity between render and playback.
- **Master-tempo automation** (PRD-0089, via PRD-0092) is evaluated per block to
  drive the grid during render, so grid-aligned content (warped clips, quantised
  edits) follows the recorded tempo exactly as it does on playback.
- The driver runs on a **background thread**, owned by the caller (the exporter,
  PRD-0100); it never touches the real-time audio thread and never blocks the
  JUCE message thread.
- The driver reports **progress** (fraction of the range rendered) via a
  thread-safe callback or atomic, and supports **cancellation**: a cancel request
  stops the loop at the next block boundary, leaves no partial state behind, and
  returns a clearly-flagged incomplete result.
- The render **range** is parameterised: whole arrangement (time zero to the end
  of the last clip plus any configured tail) or an explicit `[start, end)` region
  supplied by the caller.

## 1.3. Developer / Integration Flow

1. The exporter (PRD-0100) constructs an `OfflineRenderConfig` describing: the
   `daw` arrangement snapshot to render (the PRD-0081 compiled render model), the
   render range (`WholeArrangement` or an explicit `[startSample, endSample)` at
   the render sample rate), the **render sample rate**, the **block size** for the
   offline loop, and a **tail policy** (see §1.5.3).
2. The exporter creates an `OfflineRenderDriver` with that config plus the shared
   dependencies it does not own: the PRD-0081 render engine, the PRD-0092
   automation applier, the PRD-0080 streaming reader (placed into synchronous
   full-read mode for the render's lifetime), and the PRD-0089 master-tempo
   source.
3. The exporter calls `driver.render(sink, progress, cancelToken)` on a
   **background thread**. `sink` is either a pre-sized output `AudioBuffer<float>`
   the driver fills, or a block callback the driver invokes per block (the
   streaming form, see §1.5.x and PRD-0100's choice). `progress` is a thread-safe
   progress reporter; `cancelToken` is an atomically-checked cancel flag.
4. `render()` seeks the engine to the range start, resets the automation applier
   and grid to the values that obtain at that sample position (so a region render
   starting mid-arrangement reproduces the in-flight automation and tempo state),
   then enters the deterministic block loop.
5. For each block of `blockSize` samples (the last block may be shorter):
   - The driver computes the block's absolute sample range within the
     arrangement.
   - It evaluates **master-tempo automation** (PRD-0089/0092) for the block start
     to set the grid tempo, so warped/grid-aligned reads use the recorded tempo.
   - It evaluates **automation breakpoints** (PRD-0092) — continuous gains, boolean
     mutes/states — for the block, producing the per-block parameter values the
     render engine consumes.
   - It pulls the **render engine** (PRD-0081) for the block, which reads each
     active clip via the **synchronous** PRD-0080 reader, applies crop, clip gain,
     lane/group gain, and automation, and sums into the block output.
   - It writes the block into the `sink` (buffer region or callback).
   - It updates `progress` and checks `cancelToken`; if cancelled, it breaks out
     and returns `RenderResult::Cancelled`.
6. After the last in-range block, the driver renders the configured **tail**
   (effect / fade tail, see §1.5.3) by continuing the loop with no new clip onsets
   until the tail length elapses or the engine reports silence, then returns
   `RenderResult::Completed` with the final sample count.
7. The exporter (PRD-0100) takes the filled buffer / received blocks and encodes
   them. The driver itself writes no file and performs no format conversion beyond
   producing float samples at the render sample rate.
8. A new test file (`Tests/OfflineRenderDriverTests.cpp`) drives a synthetic
   arrangement (a couple of clips with known content, one continuous automation
   lane, one boolean lane, one master-tempo breakpoint) through the driver and
   asserts the output buffer is bit-identical to a reference produced by stepping
   the same engine manually — plus tests for region rendering, cancellation
   leaving a flagged partial result, tail handling, and missing-source policy.

## 1.4. Acceptance Criteria

- [ ] `OfflineRenderDriver` exists at
      `Source/Features/Daw/Export/OfflineRenderDriver.h/.cpp` and exposes a
      `render(sink, progress, cancelToken)` entry point taking an
      `OfflineRenderConfig` (arrangement snapshot, render range, sample rate,
      block size, tail policy) supplied at construction.
- [ ] Rendering a whole arrangement produces a float buffer whose length equals
      the arrangement length (range end minus range start) plus the configured
      tail, at the configured render sample rate, with no dropped or duplicated
      samples.
- [ ] The driver advances the playhead block-by-block and calls the **existing**
      PRD-0081 render engine and PRD-0092 automation applier per block; it adds no
      duplicate render or automation logic of its own beyond the loop, seek, and
      tail handling.
- [ ] Clip source reads are performed through PRD-0080's reader in a
      **synchronous full-read** mode: each block blocks until the required samples
      are available (no prefetch-miss silence), so an offline render never
      underruns regardless of disk speed.
- [ ] The rendered output for a fixed arrangement is **deterministic**: two
      renders of the same arrangement at the same sample rate and block size
      produce **bit-identical** buffers.
- [ ] The rendered output is **bit-identical to an ideal real-time playback**: an
      integration test compares the driver's output against a reference produced
      by stepping the same PRD-0081/PRD-0092 stack manually block-by-block over the
      same range, and the buffers match sample-for-sample.
- [ ] Continuous-automation parity: a continuous gain lane evaluated by PRD-0092
      during render produces the same per-sample gain envelope as the live path
      for the same arrangement.
- [ ] Boolean-automation parity: a boolean lane (e.g. a clip mute) evaluated by
      PRD-0092 during render toggles at the same sample positions as the live path.
- [ ] Master-tempo parity: a master-tempo automation breakpoint (PRD-0089, via
      PRD-0092) evaluated during render drives the grid such that grid-aligned /
      warped content lands at the same sample positions as on playback.
- [ ] Region rendering: given an explicit `[start, end)` range starting
      mid-arrangement, the driver seeds automation, grid tempo, and clip read
      positions to the in-flight state at `start`, so the region's output matches
      the corresponding slice of a whole-arrangement render.
- [ ] The driver runs on a caller-supplied **background thread**; it spawns no
      audio-thread work, takes no audio-device callback, and never blocks the JUCE
      message thread. It uses no `processBlock`-forbidden constructs on any
      real-time thread because it touches none.
- [ ] Progress is reported via a thread-safe mechanism (atomic or callback) at a
      block-or-coarser granularity (see §1.5.5); the reported fraction reaches
      `1.0` exactly on `Completed`.
- [ ] Cancellation: setting the cancel token causes the loop to stop at the next
      block boundary, return `RenderResult::Cancelled` with the number of samples
      rendered so far, leave no shared state mutated, and free all resources via
      RAII. A cancelled render is safe to discard or restart.
- [ ] Missing / unresolved source handling follows the §1.5.7 policy and ties to
      PRD-0097: an unresolved clip source is rendered as silence for that clip's
      span (the render continues) and the result carries a flag / list of clips
      that rendered silent, rather than aborting the whole export.
- [ ] Tail handling follows §1.5.3: the configured tail length (or
      engine-reported silence) is rendered after the last in-range clip so effect
      / fade tails are not truncated.
- [ ] The driver performs **no file encoding** and **no format conversion**: it
      emits float samples at the render sample rate and hands them to the
      PRD-0100 sink. WAV/FLAC/MP3 writing is entirely PRD-0100's concern.
- [ ] `Tests/OfflineRenderDriverTests.cpp` covers: whole-arrangement bit-identity
      vs reference, determinism (two identical renders), region rendering,
      continuous + boolean + master-tempo automation parity, cancellation partial
      result, tail rendering, and missing-source-silence behaviour.

## 1.5. Grey Areas

### 1.5.1. Determinism vs the Real-Time Path

The live path tolerates prefetch misses by emitting silence and catching up; the
offline path must not. The two paths therefore differ in their read behaviour
even though they share the same render and automation code.

**Resolution:** The offline driver places PRD-0080's reader into a **synchronous
full-read** mode for the render's lifetime: each block waits until the required
source samples are decoded and available before the render engine consumes them,
so a render never underruns and never substitutes silence for a slow read. This
is the *only* sanctioned behavioural difference from the live path, and it makes
the render *more* faithful, not less: it produces the output a glitch-free
real-time playback *would* have produced if the disk were infinitely fast.
Everything downstream of the read — gain staging, automation, summing, grid — is
the identical PRD-0081/PRD-0092 code path, guaranteeing the render is
bit-identical to an ideal playback. The synchronous mode is a render-scoped
configuration of the existing reader, not a forked reader implementation.

### 1.5.2. Render Range: Whole Arrangement vs Region / Selection

The export can target the whole arrangement or a selected region / loop. A region
render that starts mid-arrangement must reproduce the automation, grid-tempo, and
clip-read state that would be *in flight* at the region start, not start from
defaults.

**Resolution:** The driver accepts a render range of either `WholeArrangement`
or an explicit `[startSample, endSample)`. On `render()`, before the loop, it
**seeds** the engine to the range start: it evaluates the automation applier and
master-tempo automation at `startSample` to obtain the in-flight parameter and
tempo values, and it seeks each active clip's read position to the sample it
would be reading at `startSample`. This guarantees that a region render is a
byte-exact slice of the corresponding portion of a whole-arrangement render —
the region's first block already carries the correct automation envelope and
tempo. The whole-arrangement case is simply the special range `[0, arrangementEnd)`
with seeding at sample zero (defaults).

### 1.5.3. Tail Handling (Effect / Fade-Out Tails)

If the last clip ends at sample `N`, naively stopping the render at `N` truncates
any reverb / delay tail or scheduled fade-out that extends past the last clip's
content.

**Resolution:** The driver supports a configurable **tail policy** with two
modes: (a) a **fixed tail length** in samples appended after the last in-range
clip onset region, during which the loop continues with no new clip content so
effect tails and fades decay naturally; and (b) an **engine-silence** mode that
keeps rendering tail blocks until the render engine reports the summed output has
been below a small threshold for a configured number of consecutive blocks, then
stops. The default for whole-arrangement export is the engine-silence mode capped
by a maximum tail length (to bound pathological infinite-feedback cases); region
exports default to **no tail** (the region is rendered exactly as bounded) unless
the caller requests otherwise. This keeps tails intact for the common "export the
whole set" case without surprising the user who asked for a precise region.

### 1.5.4. Sample Rate & Bit Depth of Render vs Session

The session has a project sample rate (EPIC-0010); the export may request a
different rate or bit depth (PRD-0100 / the dialog). The driver must be clear
about which conversions it owns.

**Resolution:** The driver renders at a single **render sample rate** supplied in
its config; it does **not** perform bit-depth conversion or dithering — those are
float-to-integer concerns owned entirely by PRD-0100's encoder. If the requested
render rate differs from the project rate, the rate reconciliation is the same
one EPIC-0010 already defines for the engine (the engine reads sources and warps
the grid at the engine's configured rate); the driver simply configures the
engine to the render rate before the loop and emits float samples at that rate.
The driver therefore has exactly one numeric output format — float at the render
sample rate — and pushes all integer / dither / format decisions downstream.

### 1.5.5. Block Size for the Offline Loop

The offline loop can use any block size; it is decoupled from the audio device's
buffer size. Larger blocks reduce per-block overhead; smaller blocks bound
automation-evaluation quantisation and progress granularity.

**Resolution:** The offline block size is a config parameter, defaulting to a
moderate value (e.g. 1024 samples) independent of the live device buffer size.
Because PRD-0092 evaluates automation per block and the render must be
bit-identical to live playback, the **default offline block size matches the live
engine's processing block size** so that any block-boundary automation
quantisation is identical between render and playback; the bit-identity test
asserts this. A caller may override the block size for throughput, accepting that
doing so could change block-boundary automation sampling — so the bit-identity
guarantee is contracted **for the default (live-matching) block size**, and the
override is a documented performance knob, not the default path.

### 1.5.6. Progress Granularity & Cancel Safety

Progress reported per block could be very chatty on a long export; cancel must be
checked often enough to feel responsive but must leave the render in a clean
state.

**Resolution:** Progress is computed every block as `samplesRendered /
totalSamples` but **published** to the caller at a coarser cadence (e.g. at most
every few milliseconds of wall-clock or every N blocks) via a thread-safe atomic
/ callback, so a long render does not flood the UI thread. The cancel token is
checked **every block** (cheap, an atomic load) so cancellation is responsive even
when progress is published less often. Cancel safety is guaranteed by structuring
the loop so that a block is either fully written to the sink or not started: on
cancel the driver breaks **between** blocks, never mid-block, returns the exact
sample count completed, and relies on RAII for all resource cleanup. No shared
mutable state outside the driver's own scope is touched, so a cancelled render is
trivially discardable.

### 1.5.7. Automation / Tempo Evaluation Parity With Live

The whole value of reusing PRD-0092 is parity; any divergence (e.g. evaluating
automation at a different point within the block, or skipping master-tempo
re-evaluation) would make the export sound subtly different from what the DJ
heard.

**Resolution:** The driver evaluates automation and master tempo at **exactly the
same point in the block** the live engine does (block-start sample position),
using the **identical** PRD-0092 applier and PRD-0089 tempo source — no offline
re-implementation. The seeding step (§1.5.2) ensures region renders start from the
correct in-flight state. The parity tests (continuous, boolean, master-tempo)
assert sample-position equality against the live reference, and the overall
bit-identity test (§1.4) is the backstop: if any evaluation diverged, the buffers
would not match. This makes parity a *tested invariant* rather than an assumed
property.

### 1.5.8. Unresolved / Missing Sources (Skip vs Abort, ties to PRD-0097)

A clip whose source id does not resolve to a readable file (moved, deleted, or
not yet relocated) cannot be read during render. The driver must decide whether to
abort the whole export or continue.

**Resolution:** **Skip the clip, render silence for its span, and continue** — do
**not** abort. Aborting a long export at the last clip because one source moved is
hostile; the DJ would rather get the mix with one gap than nothing. The driver
renders the unresolved clip's timeline span as silence (its lane/group/automation
still process normally around the gap) and returns a `RenderResult::Completed`
that carries a **list of clips that rendered silent** due to unresolved sources.
This ties directly to PRD-0097's missing-source handling: PRD-0097 owns surfacing
and relocating missing sources before/at export time; this driver owns the
fallback behaviour if a source is still unresolved when the render runs. PRD-0100
/ the export UI decides whether to warn the user pre-export, proceed with the
flagged gaps, or offer relocation first — the driver's contract is simply "never
abort on a missing source; render silence and report it."
