---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0026
  - PRD-0063
  - PRD-0064
---

# 1. PRD-0079: Arrangement Snapshot Compiler & SeqLock Publication

## 1.1. Problem

EPIC-0010 must make the recorded arrangement audible by reading the original source files back through the audio thread. The editable arrangement, however, lives in the `daw` `juce::ValueTree` (PRD-0063): a tree of lanes, each holding an arbitrary number of clip nodes with `sourceFileId`, `sourceStartSample`, `sourceEndSample`, `timelineStartSample`, and `gain` properties. The `ValueTree` is a message-thread structure — it allocates, it is mutated arbitrarily by edit commands (E10), recording (E9), and automation (E11), and it is fundamentally unsafe to read from `processBlock`. `CLAUDE.md` forbids the audio thread from walking such a structure: no allocation, no locks, no traversal of a tree whose shape can change mid-read.

Without a defined publication mechanism, every later PRD in this Epic is blocked. The render engine (PRD-0081) needs a way to ask "which clips on which lanes overlap sample N of the timeline, and where do I read their audio from?" — and it must get that answer in real time, race-free, without touching the tree. The transport (PRD-0082), the streaming layer (PRD-0080), and the automation engine (E11) all sit on top of that same answer. If each consumer invents its own ad-hoc snapshotting, the Epic fragments into incoherent, racy reads of a tree that is being edited underneath them.

This is the identical hazard EPIC-0003 solved for tempo with the master clock: independently timed reads of mutable cross-thread state produce incoherent, partial views and audible failure. EPIC-0003 resolved it with a SeqLock double-buffer publishing an immutable `MasterClockSnapshot` (PRD-0026). The arrangement needs the same substrate, scaled from three scalar fields to a compact, sorted, immutable per-lane clip schedule. Until that compiler and its publication contract exist, the audio thread has no safe view of the arrangement, and nothing in EPIC-0010 can produce sound.

## 1.2. Objective

The system provides a message-thread arrangement compiler and a lock-free publication channel such that:

- An `ArrangementSnapshot` immutable data structure (in `Source/Features/Daw/Playback/ArrangementSnapshot.h`) represents the entire playable arrangement as a fixed set of lanes, each holding a contiguous, sorted-by-`timelineStartSample` array of `ClipEvent` records. Each `ClipEvent` carries `sourceFileId`, `sourceReadHandle`, `sourceStartSample`, `sourceEndSample`, `timelineStartSample`, `timelineEndSample`, and `gain`. The snapshot owns no `ValueTree` reference and is safe to read from the audio thread once published.
- An `ArrangementCompiler` runs exclusively on the message thread, transforms the current `daw` `ValueTree` (PRD-0063) into a freshly built `ArrangementSnapshot`, and resolves each clip's `sourceFileId` to a `sourceReadHandle` (an index into the streamer pool defined by PRD-0080 — see §1.5.4).
- An `ArrangementPublisher` wraps a SeqLock double-buffer, mirroring `MasterClockPublisher` (PRD-0026): a `std::atomic<uint32_t> sequence` counter and two pre-allocated `ArrangementSnapshot` buffers. `publish()` writes into the inactive buffer and flips an atomic active-index under the SeqLock; `read()` retries until it observes a coherent, stable snapshot. Both operations allocate nothing, take no lock, and perform no I/O on the audio thread.
- The audio thread reads the published snapshot via `ArrangementPublisher::read()` (or an equivalent coherent-acquire accessor) and NEVER walks the `daw` `ValueTree` directly. Under no concurrent write, the read completes in exactly one iteration.
- Every arrangement edit — a clip recorded (E9), moved/trimmed/split/deleted/gain-changed (E10/PRD-0083), or an automation-induced structural change (E11) — triggers a recompile-and-republish through a single, coalesced entry point, so the audio thread always sees a coherent snapshot that reflects a complete edit, never a half-applied mutation.
- Recompiles triggered by rapid successive edits are debounced/coalesced (§1.5.6) so that a burst of message-thread mutations produces at most one publication per coalescing window, bounding compile cost without starving the audio thread of timely updates.
- Before the first valid snapshot is published, the audio thread reads a well-defined empty snapshot (zero lanes / zero clip events) and renders silence (§1.5.7) — never undefined memory.

## 1.3. Developer / Integration Flow

1. `ArrangementSnapshot.h` defines two plain structs. `ClipEvent` is a trivially-copyable value object: `uint64_t sourceFileId`, `int32_t sourceReadHandle`, `int64_t sourceStartSample`, `int64_t sourceEndSample`, `int64_t timelineStartSample`, `int64_t timelineEndSample`, `float gain`. `ArrangementSnapshot` holds a fixed-capacity set of lanes; each lane holds a count and a contiguous, pre-sized array of `ClipEvent` (capacity bounded per §1.5.1). No `std::vector` growth, no heap pointers, are read on the audio thread; the snapshot is a flat, copyable POD-like aggregate so the SeqLock double-buffer can hold two instances as non-heap members.
2. `ArrangementCompiler::compile(const juce::ValueTree& daw, ArrangementSnapshot& out)` runs on the message thread. It iterates the `daw` tree's lanes (PRD-0063 structure), reads each clip node's crop/timeline/gain properties, resolves `sourceFileId` to a `sourceReadHandle` via the streamer-pool registry (PRD-0080, see §1.5.4), derives `timelineEndSample = timelineStartSample + (sourceEndSample - sourceStartSample)`, and appends a `ClipEvent` into the matching lane. After populating each lane it sorts that lane's `ClipEvent` array by `timelineStartSample` ascending (§1.5.3). The compiler allocates freely — it is on the message thread.
3. `ArrangementPublisher` is constructed once and injected by reference into both the compiler-driver (message side) and the render engine (audio side), with no singleton or global state (mirroring PRD-0026's injection contract). It holds `std::atomic<uint32_t> sequence`, an `std::atomic<uint32_t> activeIndex`, and `ArrangementSnapshot buffer[2]` as non-heap members.
4. `ArrangementPublisher::publish(const ArrangementSnapshot&)` (message thread) selects the inactive buffer index, copies the freshly compiled snapshot into it, increments `sequence` to an odd value, flips `activeIndex` to the just-written buffer, then increments `sequence` to an even value — the same odd/even guard PRD-0026 proved, extended to a double-buffer so the large snapshot is never copied field-by-field under the writer's odd window.
5. `ArrangementPublisher::read(ArrangementSnapshot&)` (audio thread) reads `sequence` and `activeIndex`, copies the indicated buffer, then re-reads `sequence`; if either sample is odd or the two differ, it retries. It allocates nothing, takes no lock, and performs no I/O. The render engine (PRD-0081) calls this (or holds a stable acquired pointer per block) and then walks only the snapshot's sorted lane arrays — never the `ValueTree`.
6. A single `ArrangementRecompileTrigger` (message thread) is the sole entry point that schedules a recompile. Edit commands (PRD-0083), the recorder (E9), and automation structural edits (E11) all call it after mutating the `daw` tree. It coalesces rapid calls (§1.5.6) onto one message-thread async update, runs `ArrangementCompiler::compile` into a scratch snapshot, and calls `ArrangementPublisher::publish`. There is exactly one compile/publish path so all producers stay consistent and reversible.
7. At construction, `ArrangementPublisher` initialises both buffers to the empty snapshot (zero lanes, zero clip events) and `sequence` to an even value, so any audio-thread read before the first real publish returns a coherent empty arrangement and the render engine outputs silence (§1.5.7).
8. A test file under `Tests/` (`ArrangementSnapshotTests.cpp` or similar) covers: deterministic compilation of a known `daw` tree into the expected sorted `ClipEvent` arrays; SeqLock read coherence under a concurrent publish loop (no torn snapshot ever observed); single-iteration read under no contention; coalescing of a burst of trigger calls into one publication; and the empty-snapshot-before-first-publish guarantee.

## 1.4. Acceptance Criteria

- [ ] `ClipEvent` is a trivially-copyable plain struct in `Source/Features/Daw/Playback/ArrangementSnapshot.h` containing exactly: `uint64_t sourceFileId`, `int32_t sourceReadHandle`, `int64_t sourceStartSample`, `int64_t sourceEndSample`, `int64_t timelineStartSample`, `int64_t timelineEndSample`, and `float gain`.
- [ ] `ArrangementSnapshot` is a plain aggregate in the same header holding a fixed-capacity set of lanes; each lane exposes a clip-event count and a contiguous, pre-sized array of `ClipEvent`. It holds no `juce::ValueTree`, no heap-owning pointer that must be dereferenced on the audio thread, and is copyable as a flat value so two instances can live as non-heap members of the publisher.
- [ ] `ArrangementCompiler::compile(const juce::ValueTree&, ArrangementSnapshot&)` runs only on the message thread, reads every clip node of every lane in the `daw` tree (PRD-0063), and writes one `ClipEvent` per clip into the matching lane of the output snapshot.
- [ ] For each compiled `ClipEvent`, `timelineEndSample == timelineStartSample + (sourceEndSample - sourceStartSample)`, and `sourceStartSample`/`sourceEndSample` are copied unmodified from the clip node (this PRD does not clamp or stretch — clamping is PRD-0083's contract).
- [ ] After populating a lane, the compiler sorts that lane's `ClipEvent` array ascending by `timelineStartSample`; ties (equal `timelineStartSample`) sort by ascending `sourceStartSample` for determinism.
- [ ] Each `ClipEvent.sourceReadHandle` is the streamer-pool index resolved from the clip's `sourceFileId` (PRD-0080); if no handle is resolvable, the handle is set to a defined sentinel (`-1`) and the clip is still emitted (so the render engine can skip it, not crash) — see §1.5.4.
- [ ] `ArrangementPublisher` wraps a SeqLock consisting of a `std::atomic<uint32_t> sequence` counter, an `std::atomic<uint32_t> activeIndex`, and an `ArrangementSnapshot buffer[2]`; all are non-heap members (no dynamic allocation at construction).
- [ ] `ArrangementPublisher::publish(const ArrangementSnapshot&)` copies into the inactive buffer, increments `sequence` to an odd value, flips `activeIndex`, then increments `sequence` to an even value. It runs only on the message thread.
- [ ] `ArrangementPublisher::read(ArrangementSnapshot&)` reads `sequence` (and `activeIndex`) before and after copying the active buffer; if either `sequence` sample is odd or the two differ, it retries. It does not allocate memory, take a lock, or perform I/O.
- [ ] Under no concurrent write, `ArrangementPublisher::read()` completes in exactly one iteration.
- [ ] A concurrency test running a continuous publish loop on one thread and a continuous read loop on another never observes a torn snapshot (every read yields a snapshot whose lane counts and `ClipEvent` contents are internally consistent with a single completed publish).
- [ ] Both publisher buffers are initialised to the empty snapshot (zero lanes populated, all lane counts zero) and `sequence` to an even value at construction; a `read()` performed before any `publish()` returns the empty snapshot.
- [ ] A single `ArrangementRecompileTrigger` entry point exists on the message thread; edit commands (PRD-0083), the recorder (E9), and future automation edits (E11) invoke it after mutating the `daw` tree, and it is the only path that calls `compile` + `publish`.
- [ ] The recompile trigger coalesces a burst of successive calls within one coalescing window into a single `compile` + `publish` (§1.5.6); a test that fires N rapid triggers observes exactly one publication for that burst.
- [ ] `ArrangementPublisher` is supplied to consumers by constructor/reference injection (mirroring PRD-0026); no singleton, no global mutable state.
- [ ] No `daw` `ValueTree` property is read or written from the audio thread by any code introduced in this PRD; the audio thread's only arrangement access is `ArrangementPublisher::read()` and reads of the returned `ArrangementSnapshot`.
- [ ] No allocation, lock, or I/O occurs on the audio-thread path (`read()` and snapshot traversal); cross-thread communication is exclusively via the SeqLock atomics.
- [ ] All new source files are located under `Source/Features/Daw/Playback/` as specified in EPIC-0010 §1.3.7.
- [ ] This PRD adds no audio rendering, no streaming read, no transport, and no edit command; it defines only the snapshot structure, the compiler, the recompile trigger, and the publication contract those later PRDs consume.

## 1.5. Grey Areas

### 1.5.1. Snapshot Granularity: Full Recompile vs Incremental

Every edit could either rebuild the entire `ArrangementSnapshot` from scratch (compile all lanes, all clips) or patch only the affected lane/clip into the existing snapshot incrementally. Incremental patching is cheaper for a single-clip move in a large arrangement; full recompile is simpler and impossible to desynchronise from the tree.

**Resolution:** Full recompile. The compile runs on the message thread where cost is not real-time-critical, and a typical DJ arrangement (a few lanes, tens to low-hundreds of clips) compiles in well under a millisecond. Incremental patching introduces a class of bugs where the snapshot drifts from the `ValueTree` after a missed or misordered patch, and it complicates the publication contract (a partial in-place patch is exactly the torn-read hazard the SeqLock exists to prevent). Full recompile guarantees the published snapshot is always a faithful, complete image of the current tree. If profiling ever shows compile cost mattering for pathologically large arrangements, the coalescing window (§1.5.6) already bounds publish frequency; incremental compilation can be added later behind the same publish contract without changing any consumer.

### 1.5.2. Double-Buffer vs Triple-Buffer for the SeqLock

PRD-0026 publishes three scalars under a plain SeqLock where the writer's odd window is a few atomic stores. The arrangement snapshot is far larger (potentially hundreds of `ClipEvent` records), so copying it field-by-field inside an odd `sequence` window would force the audio reader to spin/retry for the full duration of a large copy. A triple-buffer would let the writer prepare a buffer fully off to the side and flip a single pointer.

**Resolution:** Double-buffer with an atomic `activeIndex` flip guarded by the odd/even `sequence`. The writer copies the new snapshot into the inactive buffer *before* entering the odd window; the odd window then guards only the `activeIndex` flip (a single atomic store), so the reader's retry window is as short as PRD-0026's. This captures the essential benefit of a triple-buffer (writer never copies inside the contended window) while keeping exactly two snapshot-sized buffers as non-heap members, matching PRD-0026's "two buffers, no heap" footprint. A true triple-buffer is unnecessary because there is a single writer (the message thread) and publishes are coalesced (§1.5.6), so the writer never needs a third buffer to avoid stalling on an in-flight reader.

### 1.5.3. Clip Event Sort Key

The render engine needs to find, per block, the clips on a lane overlapping the current playhead. Storing each lane's clips sorted by a stable key lets the engine binary-search the active window instead of scanning. The natural key is `timelineStartSample` (when the clip begins on the timeline), per lane.

**Resolution:** Sort each lane's `ClipEvent` array ascending by `timelineStartSample`, with ascending `sourceStartSample` as the deterministic tie-breaker. Per-lane (not global) sorting matches the render model where each lane is summed independently, and `timelineStartSample` is the key the playhead-overlap query needs. EPIC-0010 deliberately leaves full same-lane overlapping-clip crossfades out of scope (§1.2.2); where two clips on one lane do touch or briefly overlap, the sort order plus the render engine's short anti-click ramp (PRD-0081) handle the boundary deterministically. The compiler owns the sort so every consumer receives an already-ordered array and need not re-sort on the audio thread.

### 1.5.4. Representation of Source-Read Handles

Each `ClipEvent` must tell the render engine where to read audio from. It could carry the raw `sourceFileId`, a pointer to a streamer object, or an opaque index into the streamer pool (PRD-0080). A raw pointer into a pool whose lifetime the compiler does not control is unsafe across recompiles; a `sourceFileId` would force the audio thread to do a lookup.

**Resolution:** `sourceReadHandle` is an `int32_t` index into the streamer pool that PRD-0080 owns and pre-allocates. The compiler resolves `sourceFileId → handle` on the message thread at compile time and bakes the integer index into the `ClipEvent`. The audio thread dereferences the pool by index (a bounded array access, no lookup, no allocation). PRD-0080 owns the pool's lifetime and guarantees handles remain valid for the lifetime of any published snapshot referencing them (it must not retire a streamer slot still referenced by the active snapshot). If a `sourceFileId` cannot be resolved (e.g., the streamer pool is not yet primed for a just-recorded clip), the handle is the sentinel `-1` and the render engine skips that clip — the clip still appears in the snapshot so the arrangement shape is complete and the clip becomes audible on the next recompile once its streamer is primed.

### 1.5.5. Memory Ownership & Lifetime of the Immutable Snapshot

The audio thread reads a snapshot that the message thread is concurrently replacing. With a double-buffer, the writer overwrites the *inactive* buffer — but a reader that acquired the previously-active buffer must not have it mutated mid-read. The question is how the old buffer is "retired" safely.

**Resolution:** The SeqLock retry protocol *is* the retirement mechanism — no separate reclamation, reference counting, or RCU is needed. The writer only ever copies into the buffer that `activeIndex` does not currently point at, and it flips `activeIndex` atomically inside the odd `sequence` window. A reader that began copying the old buffer and is then overtaken by a publish observes a `sequence` change and retries, re-reading the new `activeIndex`. Because there is a single writer and publishes are coalesced (so two publishes never sandwich a single reader within one retry), the inactive buffer the writer targets is never the one a reader is mid-copy on once the retry succeeds. Both buffers are owned for the entire program lifetime by the publisher as non-heap members; nothing is ever freed, so there is no use-after-free surface. Streamer-pool slots referenced by handles are retired by PRD-0080, gated on no active snapshot referencing them (§1.5.4).

### 1.5.6. Recompile Debounce / Coalescing on Rapid Edits

A drag-move of a clip, or a recording in progress (E9), can mutate the `daw` tree many times per second. Recompiling and republishing on every single mutation would burn message-thread cycles and publish redundant intermediate snapshots the audio thread would barely observe.

**Resolution:** The `ArrangementRecompileTrigger` coalesces. Each edit calls `triggerRecompile()`, which marks the arrangement dirty and schedules a single asynchronous message-thread update (e.g., via `juce::AsyncUpdater` or a small fixed-interval timer); multiple calls before the update fires collapse into one compile + publish. This bounds publish frequency to at most one per coalescing window regardless of edit burst rate, while a window short enough (sub-frame, on the order of one UI tick) keeps the audible result responsive — the DJ dragging a clip hears the new position effectively immediately on release, and intermediate drag positions need not each become audible. The coalescing window is a single tunable; it does not change the publication contract, so it can be adjusted without touching any consumer.

### 1.5.7. Audio-Thread Behaviour Between Startup and First Valid Snapshot

At application startup, and before any clip is recorded or any edit occurs, the audio thread may call `ArrangementPublisher::read()` before the message thread has compiled anything. The reader must get defined, safe data — never uninitialised buffer memory.

**Resolution:** The publisher initialises both buffers to the canonical empty snapshot (zero lanes populated, every lane count zero) and `sequence` to an even value at construction, so the very first `read()` succeeds in one iteration and returns an empty arrangement. The render engine (PRD-0081) interprets zero clip events as silence and sums nothing into the master output — exactly the correct behaviour for an empty timeline. There is no special "not yet initialised" flag for the audio thread to branch on; the empty snapshot is a fully valid arrangement that happens to contain no clips, so the audio path has a single uniform code path from the first block onward. The first real publish (after a recording or an edit) atomically replaces the empty snapshot, and playback becomes audible with no startup race.
