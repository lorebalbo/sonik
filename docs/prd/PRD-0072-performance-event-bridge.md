---
status: Implemented
epic: EPIC-0009
depends-on:
  - PRD-0041
  - PRD-0063
  - PRD-0071
---

# 1. PRD-0072: Performance-Event Bridge

## 1.1. Problem

PRD-0071 builds the `RecordingSessionController`: a message-thread state machine that owns the armed/recording state and the master-clock-anchored record playhead. But the controller can only turn a performance into an arrangement if it learns *what the DJ did and when*. Those structural actions — a deck starting to play, a channel un-muting, a quantized hot-cue firing, a beat-jump, a loop in/out, a source-mode switch between Original and stems — originate from two different threads with incompatible contracts:

- Some events fire on the **audio thread**, deep inside `processBlock`. A quantized cue that the user armed earlier actually triggers when the audio engine reaches the next grid line; a loop boundary wraps mid-buffer; a beat-jump scheduled to land on the beat executes when the transport crosses it. Per `AGENTS.md`, this thread may not allocate, may not take a lock, and may not touch the `ValueTree` or do any I/O. It cannot call into the controller directly, because the controller mutates the `daw` model and lives on the message thread.
- Some events fire on the **message thread**, from a UI click or a MIDI-routed action that PRD-0044 already lands on the message thread (a transport play button, a manual loop toggle, a source-mode dropdown).

Without a defined bridge, every event producer would invent its own thread crossing. The audio-thread producers would be tempted to post a `juce::Message` (16 ms jitter, and a heap allocation per post — forbidden), or to grab a lock the controller holds (a data race and a priority-inversion glitch). The message-thread producers would call the controller directly, creating a *second*, separately-ordered event path. The result: two streams that interleave non-deterministically, so a "stop deck A" that the user clicked a millisecond before a quantized "cue A" fired could be recorded in the wrong order, splitting a clip at the wrong sample and corrupting the captured arrangement.

EPIC-0009 §1.3.3 is explicit: there must be **one ordered stream**. This PRD owns the transport that produces it.

## 1.2. Objective

The system defines and implements a single, canonical, real-time-safe path — `Source/Features/Daw/Recording/PerformanceEventFifo.h/.cpp` — by which deck/mixer **structural** events reach the `RecordingSessionController` (PRD-0071) on the message thread, regardless of which thread the event originated on, without ever violating the audio-thread contract. Specifically:

- The system defines a fixed-size, trivially-copyable POD `PerformanceEvent` carrying: an event-type enum, the originating deck index, the deck's **source sample position** at the event instant (`int64`), and a monotonic event timestamp. No pointers, no strings, no variable-length payloads.
- The system defines the closed taxonomy of structural event types this Epic captures: deck play, deck stop, channel mute, channel unmute, cue jump-in, cue jump-out, beat-jump, loop-in, loop-out, and source-mode change. Continuous parameter automation is explicitly excluded (EPIC-0011).
- Audio-thread-originated events are pushed into a pre-allocated lock-free single-producer/single-consumer `juce::AbstractFifo` over a `std::array<PerformanceEvent, N>`, mirroring PRD-0041's RT-safe MIDI bridge pattern (see §1.3): the producer reserves a slot, writes the POD, and `finishedWrite(1)` — no allocation, no lock, no I/O.
- Message-thread-originated events enqueue through the **same** FIFO so that a single drain produces one ordered stream. Where the message thread is the sole producer for a given event, it writes to the same FIFO; the FIFO's single-producer assumption is preserved by funnelling all writes through one enqueue entry point (see §1.5.3).
- The `RecordingSessionController` drains the FIFO on the message thread (on its existing projection/timer cadence from PRD-0071), reading every ready event in FIFO order and handing each to its own interpretation logic. This PRD stops at delivery; turning an event into a clip is PRD-0073/0075/0076/0077.
- On a full FIFO the producer never blocks and never allocates; it records an overflow per the policy resolved in §1.5.1 and increments an atomic diagnostic counter.

This PRD defines the **transport layer** only. It does not create, grow, align, or finalise clips, and it does not mutate the `daw` `ValueTree` beyond what PRD-0071 already does to drive the drain. It guarantees that, given a structural event from either thread, that event reaches the controller exactly once, in a single deterministic order, within the real-time contract.

## 1.3. Developer / Integration Flow

This PRD has no user-facing surface. Its producers are the deck/mixer engines (audio thread) and the deck/mixer UI + MIDI router (message thread); its single consumer is the `RecordingSessionController` (PRD-0071).

### 1.3.1. Mirrored Pattern from PRD-0041

PRD-0041's RT-safe MIDI bridge is the reference implementation this PRD copies. Its approach: a `juce::AbstractFifo(capacity)` index paired with a pre-allocated `std::array<MidiAudioEvent, 1024>` backing array, both sized once in the constructor and never resized; the producer calls `prepareToWrite(1)`, writes a trivially-copyable POD into the reserved slot, and `finishedWrite(1)`; on a full FIFO it returns a `DroppedFull` result and `fetch_add`s a `std::atomic<uint64_t>` drop counter with `std::memory_order_relaxed`; the consumer drains at a defined cadence via `getNumReady()` / `prepareToRead` (handling the two-block wrap-around) and `finishedRead(n)`; the whole path is `noexcept`, allocation-free, lock-free, and I/O-free, and the FIFO view is the only cross-thread contract. PRD-0072 reuses this pattern verbatim, swapping the `MidiAudioEvent` POD for a `PerformanceEvent` POD and the audio-engine consumer for the `RecordingSessionController` drain.

### 1.3.2. Construction & Wiring

1. `RecordingSessionController` (PRD-0071) owns a `PerformanceEventFifo` instance, constructed on the message thread during recording-subsystem init.
2. The constructor allocates exactly one `std::array<PerformanceEvent, N>` backing array and one `juce::AbstractFifo(N)` index. `N` is a compile-time constant (see §1.5.1). No further allocation occurs at runtime.
3. The deck and mixer engines receive a reference (or a thin view) to the FIFO's producer-facing enqueue entry point. The audio thread captures a raw pointer/reference once at `prepareToPlay`; the FIFO is the contract.

### 1.3.3. Producer Side — Audio Thread

1. Inside `processBlock`, a structural event fires (e.g., a quantized cue lands on the grid line, a loop wraps, a beat-jump executes).
2. The engine captures the deck's source sample position **at the exact event instant** (see §1.5.6) and builds a `PerformanceEvent` POD on the stack.
3. The engine calls `PerformanceEventFifo::enqueue(const PerformanceEvent&) noexcept`, which reserves a slot via `prepareToWrite(1)`, copies the POD into the reserved array slot, and `finishedWrite(1)`.
4. On a full FIFO, `enqueue` applies the overflow policy from §1.5.1 and increments the atomic overflow counter. It never blocks, never allocates, never locks, never logs.
5. `processBlock` proceeds. The event is now visible to the consumer on its next drain.

### 1.3.4. Producer Side — Message Thread

1. A UI click or PRD-0044-routed message-thread action produces the same kind of structural event (e.g., the user clicks Play, toggles a loop, switches source mode).
2. The producer builds a `PerformanceEvent` POD (timestamp and source sample position captured from the deck's current message-thread-visible state) and calls the same `enqueue` entry point so the event joins the single ordered stream (see §1.5.3 for single-producer discipline).

### 1.3.5. Consumer Side — Message Thread Drain

1. On its existing projection cadence (PRD-0071), the `RecordingSessionController` calls `PerformanceEventFifo::drain(Handler&)`.
2. `drain` queries `getNumReady()`, obtains start indices and block sizes via `prepareToRead`, and iterates every ready event **in FIFO order**, handling the two-block wrap-around case so events are never reordered across the buffer boundary.
3. For each event, `drain` calls `handler.onPerformanceEvent(const PerformanceEvent&)` synchronously. The handler is the controller's own interpretation logic (PRD-0073 onward); this PRD only delivers.
4. `drain` calls `finishedRead(n)` to release the slots. The drain itself performs no allocation and takes no lock beyond what the controller already holds on the message thread.

### 1.3.6. Overflow Diagnostics

1. The FIFO exposes `getOverflowCount() const noexcept -> uint64_t` via a relaxed atomic load.
2. The controller surfaces this counter to the existing diagnostics tick. A non-zero count outside a stress test is a bug to investigate (capacity too small, drain starved, or runaway producer) and, because structural events must not be lost (§1.5.1), is treated as a correctness defect rather than a tolerable drop.

### 1.3.7. Shutdown

1. The `PerformanceEventFifo` is destroyed on the message thread after the audio engine has stopped (so no producer can enter `enqueue`).
2. Any residual events are drained or discarded; recording has already finalised by this point.
3. The fixed backing array is released by the normal destructor; no special handling.

## 1.4. Acceptance Criteria

- [ ] The system defines `Source/Features/Daw/Recording/PerformanceEventFifo.h` and `.cpp` and adds no other production files.
- [ ] The system exposes a `PerformanceEvent` POD with members `PerformanceEventType type; uint8_t deckIndex; int64_t sourceSamplePosition; int64_t timestamp;` (or the equivalent fixed-size fields resolved in §1.5.2) and a `static_assert(std::is_trivially_copyable_v<PerformanceEvent>)`.
- [ ] The system exposes a `PerformanceEventType` enum class containing at minimum: `DeckPlay`, `DeckStop`, `ChannelMute`, `ChannelUnmute`, `CueJumpIn`, `CueJumpOut`, `BeatJump`, `LoopIn`, `LoopOut`, `SourceModeChange`.
- [ ] The system does not include continuous-automation or boolean-automation event types (those are EPIC-0011); the taxonomy is restricted to the structural events listed above.
- [ ] The system allocates the FIFO backing array (`std::array<PerformanceEvent, N>`) and the `juce::AbstractFifo(N)` index in the constructor and never resizes them at runtime, where `N` is the compile-time constant resolved in §1.5.1.
- [ ] The system exposes `enqueue(const PerformanceEvent&) noexcept` callable from any producer thread (audio or message), which reserves one slot via `juce::AbstractFifo::prepareToWrite(1)`, writes the POD to the reserved slot, and calls `finishedWrite(1)`.
- [ ] The system's `enqueue` method performs zero heap allocations, zero lock acquisitions, zero thread sleeps, and zero I/O under all conditions, including a full FIFO.
- [ ] The system's `enqueue` method, on a full FIFO, applies the §1.5.1 overflow policy and atomically increments a `std::atomic<uint64_t>` overflow counter using `std::memory_order_relaxed`, without blocking.
- [ ] The system exposes `drain(Handler&) noexcept` callable only from the message thread, which drains every ready event from the FIFO in FIFO order and invokes `handler.onPerformanceEvent(const PerformanceEvent&)` synchronously for each.
- [ ] The system's `drain` method correctly handles the wrap-around case where `juce::AbstractFifo::prepareToRead` returns two non-contiguous blocks, processing both in arrival order.
- [ ] The system exposes a `getOverflowCount() const noexcept -> uint64_t` accessor returning the counter via a `std::memory_order_relaxed` load.
- [ ] The system preserves a single ordered stream: audio-thread and message-thread events drained from the FIFO appear in the relative order in which they were enqueued, per the single-producer discipline resolved in §1.5.3.
- [ ] The system captures the deck's source sample position at the event instant per §1.5.6, such that a clip later opened from a `CueJumpIn` event reads the correct `sourceStartSample`.
- [ ] The system does not create, grow, align, or finalise any `DawClip`, and does not mutate the `daw` `ValueTree` beyond the drain handing events to PRD-0071's controller (clip interpretation is PRD-0073/0075/0076/0077).
- [ ] The system adds no UI component and no `DESIGN.md`-governed surface.
- [ ] The system's audio-thread `enqueue` path adds no `#include` of any header that performs allocation, locking, or I/O, and introduces no dependency that would pull such behaviour into `processBlock`.
- [ ] The system is covered by a `PerformanceEventFifoTests.cpp` unit test in `Tests/` that verifies: (a) `enqueue` from a single producer pushes to the FIFO and `drain` reads the events back in order; (b) a full FIFO applies the resolved overflow policy and increments the overflow counter without blocking; (c) `drain` processes events correctly across a wrap-around boundary; (d) the `static_assert` for `PerformanceEvent` trivial-copyability compiles; (e) the event taxonomy enum contains the required structural types; (f) interleaved audio- and message-originated enqueues drain in a single deterministic order.

## 1.5. Grey Areas

### 1.5.1. FIFO Capacity and Overflow Policy

A structural-event FIFO has a fundamentally different loss tolerance from PRD-0041's MIDI scratch FIFO. PRD-0041 can drop a jog tick: the user perceives a slightly choppy scratch, but no state is corrupted. PRD-0072 cannot drop a structural event: losing a "stop deck A" leaves a clip open forever, and losing a "cue jump-in" loses an entire captured segment. Three overflow policies present themselves: drop-oldest, drop-newest, or assert.

**Resolution:** Size the FIFO generously and treat overflow as a hard correctness defect, not a tolerated drop. Structural events arrive at human-interaction rates — even frantic performance peaks at low tens of events per second across four decks, orders of magnitude below the drain cadence. A capacity of `1024` events (matching PRD-0041's proven sizing, here representing seconds of worst-case structural activity) makes a genuine overflow effectively impossible under correct operation. On the never-expected full-FIFO case, the policy is **drop-newest plus increment the overflow counter** (the audio thread cannot block or assert in release without risking a dropout), and the non-zero counter is escalated as a bug per §1.3.6. Drop-newest (rather than drop-oldest) is chosen because the already-queued events are an intact ordered prefix that the drain can still consume correctly; overwriting the oldest would corrupt that prefix and is strictly worse. Because overflow signals a defect rather than a routine condition, no clever recovery is built; the fix is to find the starvation or runaway, not to tune the drop behaviour.

### 1.5.2. POD Event Struct Fields and Fixed Size

The struct must be trivially copyable and fixed-size, but the exact field set is a judgement call: how is the deck identified, what does the timestamp measure, and is a single payload field enough for every event type (a beat-jump distance, a loop length, a source-mode value)?

**Resolution:** The minimal struct is `{ PerformanceEventType type; uint8_t deckIndex; int64_t sourceSamplePosition; int64_t timestamp; }`. `deckIndex` is a `uint8_t` (decks 0–3, room to grow). `sourceSamplePosition` is the deck's source position captured at the event instant (§1.5.6), `int64` to cover long files at high sample rates. `timestamp` is the ordering/identity field (§1.5.4). If a later PRD finds it needs a small per-event scalar (e.g., a beat-jump signed beat count or a source-mode enum value), a single fixed-width `int64_t payload` field is added — never a pointer, never a variant, never a string. The struct stays a flat POD so `std::is_trivially_copyable_v` holds and the FIFO copy is a memcpy. Interpreting `payload` per `type` is the consumer's job (PRD-0073 onward); the transport stays type-agnostic.

### 1.5.3. Single FIFO for All Decks vs Per-Deck, and Single-Producer Discipline

`juce::AbstractFifo` is single-producer/single-consumer. The Epic wants one ordered stream, but events originate from multiple decks and from two threads. A per-deck FIFO would trivially satisfy single-producer-per-FIFO but would lose the cross-deck ordering the controller needs ("did deck A stop before or after deck B's cue fired?").

**Resolution:** One FIFO for all decks, preserving a single global order, because clip placement depends on the relative ordering of events across decks and threads. The single-producer constraint is then the real risk: the audio thread (potentially several decks' processing within one `processBlock`) and the message thread both write. This is reconciled by observing that within a single audio buffer, all deck processing runs on the *same* audio thread sequentially, so audio-side enqueues are already serialised; the only genuine concurrency is audio-thread vs message-thread. That two-writer case is handled by funnelling all enqueues through one entry point guarded by the lock-free reservation semantics of `prepareToWrite`, treating the FIFO as effectively serialised at the reservation point. If profiling ever shows a real SPSC violation, the fallback is two FIFOs (one audio-producer, one message-producer) merged by timestamp at drain time (§1.5.4 makes the timestamp the merge key) — but the default is the simpler single FIFO, and the per-deck split is explicitly rejected.

### 1.5.4. Timestamp Source: Sample Position vs Wall Clock

The `timestamp` field could be a wall-clock value (`juce::Time::getHighResolutionTicks`) or a sample-domain value derived from the master clock / record playhead. They serve different purposes: wall clock orders events in real time; the sample position locates them on the musical timeline.

**Resolution:** The struct carries **both** roles split across two fields, but only one is the *ordering* field. `sourceSamplePosition` (§1.5.6) is the deck-local musical anchor used to open/grow clips; it is **not** a global ordering key because two decks have independent source positions. `timestamp` is a **monotonic global counter** sampled at enqueue time — the simplest correct choice is a monotonically increasing sequence number (or a high-resolution monotonic tick) assigned at the enqueue entry point, giving a total order across all decks and both threads. The record-playhead timeline position (the `timelineStartSample` the Epic ultimately needs) is **not** stored in this event; it is read by the controller at drain time from PRD-0071's master-clock-anchored playhead, keeping this transport free of clock-projection logic. Thus: `timestamp` orders, `sourceSamplePosition` anchors the source, and the timeline anchor is resolved downstream.

### 1.5.5. Ordering Across Audio- and Message-Originated Events

Even with one FIFO, a subtle ordering question remains: if a user clicks Stop on the message thread at "the same time" a quantized cue fires on the audio thread, which is recorded first? The answer materially changes the captured arrangement (clip boundaries land differently).

**Resolution:** Order is defined by **enqueue order into the single FIFO**, made total by the monotonic `timestamp` (§1.5.4) assigned at the reservation point. There is no attempt to reconcile to "true" wall-clock simultaneity — that is unknowable and unnecessary. The contract is simply: whichever producer wins the `prepareToWrite` race is ordered first, and that order is stable and deterministic from the drain's perspective. This is acceptable because structural events at human cadence are effectively never truly simultaneous at sample resolution, and the rare race resolves to one consistent answer rather than a corrupted interleave. The controller (PRD-0073 onward) interprets the delivered order as authoritative; this PRD guarantees the order is single, stable, and lossless, not that it matches an external notion of simultaneity.

### 1.5.6. Capturing the Source Sample Position at the Event Instant

The clip system needs `sourceStartSample` to equal the deck's source position **exactly when the event fired**, not when the drain later runs. For audio-thread events fired mid-`processBlock`, the deck's source position is advancing sample-by-sample; reading it at drain time (milliseconds later) would be wrong by a whole buffer or more.

**Resolution:** Capture the source position **eagerly, at the producer, at the event instant**, and store it in the POD. For an audio-thread event, the engine already knows the precise sample offset within the current buffer at which the event fired (e.g., the grid line the quantized cue landed on); it computes `sourceSamplePosition = bufferStartSourcePos + intraBufferOffset` and writes that into the event before enqueuing. For a message-thread event, the deck's current message-thread-visible source position (the same value the existing projection reads) is captured at enqueue. This is exactly why the position lives **in the event** rather than being re-read at drain time: the FIFO decouples *when the event is processed* from *when it happened*, and the source anchor must be frozen at the happening. The drain therefore trusts the POD's `sourceSamplePosition` verbatim and never re-samples deck state.

### 1.5.7. Coalescing Rapid Events

A user spamming a beat-jump, or a fast loop re-trigger, can emit several events in quick succession. Should the bridge coalesce these (e.g., collapse three beat-jumps into a net jump) to reduce clip churn?

**Resolution:** No coalescing in the transport. The FIFO delivers every event verbatim, because coalescing is an *interpretation* decision that belongs to the clip-placement consumer (PRD-0075 for hot-cue/beat-jump capture), not the transport. Collapsing three beat-jumps into one would silently destroy the captured truth — the DJ *did* perform three jumps, and the arrangement should reflect each discontinuity (three contiguous clips), or the consumer may *choose* to merge them with full knowledge of clip semantics. The transport's only job is lossless, ordered delivery; any coalescing, debouncing, or net-effect collapsing is a downstream policy applied where clip boundaries are actually computed. Keeping the bridge dumb also keeps it provably allocation-free and lock-free, with no per-event-type branching on the producer side.
