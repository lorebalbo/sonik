---
status: Implemented
epic: EPIC-0005
depends-on: [PRD-0040]
---

# 1. PRD-0041: RT-Safe MIDI Message Bridge

## 1.1. Problem

PRD-0040 makes MIDI messages flow into the application on the JUCE MIDI callback thread, where every subscriber receives every byte. But the subscribers in subsequent PRDs need those messages to reach two very different destinations, each governed by a strict thread contract:

- **The audio thread**, which processes the next `processBlock` and must read jog scratch deltas, jog bend, and scratch-engage state from atomics with sub-buffer latency. The audio thread is forbidden from taking locks, allocating, or doing I/O (per `AGENTS.md`). It cannot poll a queue protected by a mutex; it cannot receive a JUCE message asynchronously fast enough to scratch a track in time.
- **The Message thread**, which owns the `juce::ValueTree` and every UI control. Transport button presses, pitch fader moves, hot-cue triggers, EQ knob turns, and library navigation must land here so the existing setters and observers fire identically to a mouse click.

Without a defined bridge, every MIDI consumer would invent its own thread crossing — some with locks (data races against the audio thread), some with `new` inside the callback (memory allocator contention), some with `juce::Message` posts (16 ms scheduling jitter, useless for scratching). The result would be a system that works for buttons but produces audible glitches and choppy scratching the moment the user touches a jog wheel.

DJs scratching a jog wheel at 33⅓ RPM produce roughly 80 inbound CC messages per second per direction. A vinyl-style scratch in both directions generates short bursts of 200+ messages/second. Every one of those messages must reach the audio thread within the same buffer period as it arrived, or the scratch will sound like a stutter rather than a transition.

## 1.2. Objective

The system must define and implement a single, canonical, real-time-safe bridge that takes a `MidiInboundEvent` (delivered by PRD-0040 on the MIDI callback thread) and routes it deterministically to either the audio thread (via a lock-free FIFO of pre-typed events) or the Message thread (via `juce::MessageManager::callAsync`), based solely on a routing classification carried by the event. Specifically:

- The system ensures that every `MidiInboundEvent` is classified into exactly one routing category — `AudioThread` or `MessageThread` — by a pure function that performs no allocations, no locks, and no I/O.
- The system ensures that `AudioThread`-classified events are written to a lock-free, single-producer / single-consumer FIFO using `juce::AbstractFifo` over a pre-allocated, fixed-capacity array of `MidiAudioEvent` PODs, and that the audio thread drains the FIFO at the top of every `processBlock` before any audio processing.
- The system ensures that `MessageThread`-classified events are dispatched via `juce::MessageManager::callAsync` to a `MessageThreadSink` interface implemented by downstream consumers (the inbound routing engine in PRD-0044).
- The system ensures that the audio-thread path delivers any inbound MIDI event to a `processBlock` reading the FIFO within the **same audio buffer period** in which the event arrived, with a hard p99 budget of **5 ms** at 44.1 / 48 kHz with audio buffer sizes ≤ 256 frames.
- The system ensures that the FIFO drops, but never blocks or allocates, when full: the producer (MIDI callback thread) returns a `BridgeWriteResult::DroppedFull` and increments a `std::atomic<uint64_t>` drop counter that diagnostics can read; the audio thread is never starved by a full FIFO.
- The system ensures that classification is **data-driven**, not hard-coded: a small table maps a `MidiTargetCategory` enum (set by PRD-0042's binding resolver) to a routing decision, so future categories can be added without touching the bridge.
- The system ensures that the bridge introduces no new dependencies on `Source/Features/Deck/`, `Source/Features/Mixer/`, or `Source/Features/Library/`. Its only cross-module touchpoint is a shared header `Source/Features/AudioEngine/AudioEngineMidiBridge.h` that declares the audio-thread-facing FIFO.

This PRD defines the **transport layer** only. It does not interpret MIDI messages, does not resolve bindings, and does not know what `MidiTargetCategory` a particular event belongs to — that job is PRD-0042's. PRD-0041 simply guarantees that, given a classified event, it reaches the right thread within the right time budget without violating the real-time contract.

## 1.3. User Flow

This PRD has no user-facing surface. Its consumers are the inbound routing engine (PRD-0044) on the producer side and `AudioProcessor::processBlock` on the consumer side. The flow is expressed as a sequence of thread-contracted events.

### 1.3.1. Construction & Wiring

1. `SonikApplication::initialise()` constructs a `MidiMessageBridge` on the Message thread after `MidiDeviceManager` (PRD-0040) and before the audio engine starts processing.
2. The constructor allocates exactly two fixed-capacity buffers: a `std::array<MidiAudioEvent, 1024>` for the audio-thread FIFO, and a `juce::AbstractFifo(1024)` index. Both are stored as `const`-sized members; no further allocation occurs at runtime.
3. The constructor stores a reference to the shared `AudioEngineMidiBridge` view that the audio engine reads in `processBlock`. The view is a thin struct exposing `const MidiAudioEvent*` and the `juce::AbstractFifo&` to the audio thread.
4. The audio engine, on its `prepareToPlay`, snapshots a pointer to the view. No further coordination is needed: the FIFO is the contract.

### 1.3.2. Producer Side — Inbound MIDI Callback Thread

1. PRD-0044's inbound routing engine subscribes to `MidiDeviceManager` (PRD-0040) and receives `onMidiInbound(const MidiInboundEvent&) noexcept` on the MIDI callback thread.
2. PRD-0044's resolver determines a `MidiTargetCategory` (e.g., `JogScratch`, `JogBend`, `TransportPlay`, `PitchFader`, `HotCue`, `LibraryNav`) and a normalised `float value` or `int delta`.
3. PRD-0044 calls `MidiMessageBridge::dispatch(category, deckIndex, normalisedValue, intDelta)` on the MIDI callback thread.
4. `dispatch` reads the routing classification for `category` from a `constexpr std::array<RoutingClass, MidiTargetCategoryCount>` table.
5. If the classification is `AudioThread`, `dispatch` reserves a slot in the `juce::AbstractFifo`, writes a `MidiAudioEvent { category, deckIndex, value, delta, sampleTimestamp }` POD into the pre-allocated array slot, and `finishedWrite(1)`. On a full FIFO, it returns `BridgeWriteResult::DroppedFull` and atomically increments `droppedFullCount`.
6. If the classification is `MessageThread`, `dispatch` constructs a small `MidiMessageEvent` POD on the stack and calls `juce::MessageManager::callAsync([sink = this->messageThreadSink, event] { sink->onMidiMessageThreadEvent(event); })`. **This is the only allocation permitted in this path**, performed via JUCE's lambda capture; per AGENTS.md guidance and JUCE's documented behaviour, this allocation does not occur on the audio thread and is acceptable on the MIDI callback thread, which is not real-time-audio-critical.
7. `dispatch` returns `BridgeWriteResult::Ok` and the callback thread continues.

### 1.3.3. Consumer Side — Audio Thread

1. At the top of `processBlock`, before any audio processing, the audio engine calls `MidiMessageBridge::drainAudioThreadFifo(AudioMidiEventHandler& handler)`.
2. `drainAudioThreadFifo` queries `juce::AbstractFifo::getNumReady()` and obtains start indices and block sizes via `prepareToRead`.
3. For each ready event, the method calls `handler.applyAudioMidiEvent(const MidiAudioEvent&)` synchronously. The handler is an interface implemented by the audio engine itself; it writes the value to the appropriate `std::atomic` deck variable (`jogSpeedMultiplier`, `jogBendOffset`, `jogScratchActive` for jog events; future audio-thread targets if added).
4. The method calls `finishedRead(numEvents)` to release the slots.
5. `processBlock` proceeds with audio synthesis. The next `processBlock` call sees the updated atomics.
6. Total drain cost is bounded: at most 1024 events per call, each handled in O(1). Realistic per-buffer drain is < 10 events even during heavy scratching.

### 1.3.4. Consumer Side — Message Thread

1. The `juce::MessageManager` delivers the queued lambda from §1.3.2 step 6.
2. The lambda invokes `MessageThreadSink::onMidiMessageThreadEvent(const MidiMessageEvent&)` on the Message thread.
3. The sink (PRD-0044's `MidiInboundRouter`) translates the event into a ValueTree write or a deck-state call. The existing observer pattern fires UI updates.

### 1.3.5. Overflow Path — FIFO Full

1. The producer attempts to write to the audio FIFO. `juce::AbstractFifo::prepareToWrite` returns blockSize1 + blockSize2 = 0.
2. The producer returns `BridgeWriteResult::DroppedFull` and increments `droppedFullCount.fetch_add(1, std::memory_order_relaxed)`.
3. The audio thread is not blocked. The dropped event is logged later via a Message-thread diagnostics tick (a `juce::Timer` polling `droppedFullCount` once per second). If the counter ever exceeds zero outside a stress test, it is a bug to investigate (capacity too small, audio thread starved, or producer runaway).
4. **No audible glitch occurs for the dropped event**, but the user perceives a missed jog tick. This is preferable to an audio underrun.

### 1.3.6. Shutdown

1. `MidiMessageBridge::~MidiMessageBridge()` runs on the Message thread after `MidiDeviceManager` has been destroyed (so no more producers can enter `dispatch`).
2. Any remaining events in the audio FIFO are discarded; the audio engine has already stopped by this point.
3. The Message-thread diagnostics `juce::Timer` is stopped.
4. The fixed-capacity buffer is released as a normal C++ destructor; no special handling required.

## 1.4. Acceptance Criteria

- [ ] The system constructs `MidiMessageBridge` on the Message thread after `MidiDeviceManager` and before the audio engine begins processing.
- [ ] The system allocates the audio-thread FIFO backing array (`std::array<MidiAudioEvent, 1024>`) and the `juce::AbstractFifo(1024)` index in the constructor and never resizes them at runtime.
- [ ] The system exposes a `MidiAudioEvent` POD with members `MidiTargetCategory category; uint8_t deckIndex; float normalisedValue; int16_t intDelta; int64_t sampleTimestamp;` and a `static_assert(std::is_trivially_copyable_v<MidiAudioEvent>)`.
- [ ] The system exposes a `MidiMessageEvent` POD with members `MidiTargetCategory category; uint8_t deckIndex; float normalisedValue; int16_t intDelta; uint64_t deviceId;` and a `static_assert(std::is_trivially_copyable_v<MidiMessageEvent>)`.
- [ ] The system exposes a `MidiTargetCategory` enum class containing at minimum: `JogScratch`, `JogBend`, `JogTouch`, `TransportPlay`, `TransportCue`, `TransportSync`, `PitchFader`, `Gain`, `EqHigh`, `EqMid`, `EqLow`, `Crossfader`, `LoopIn`, `LoopOut`, `LoopSizeHalve`, `LoopSizeDouble`, `LoopToggle`, `HotCueTrigger`, `BeatJumpMinus`, `BeatJumpPlus`, `LibraryScrollUp`, `LibraryScrollDown`, `LibraryLoadDeck`, `LibraryFocusSearch`.
- [ ] The system exposes a `constexpr std::array<RoutingClass, MidiTargetCategoryCount> routingTable` mapping each category to `RoutingClass::AudioThread` or `RoutingClass::MessageThread`, defined at compile time and verified by a `static_assert` over enum cardinality.
- [ ] The system classifies `JogScratch`, `JogBend`, `JogTouch` as `AudioThread` and all other listed categories as `MessageThread`.
- [ ] The system exposes a `dispatch(MidiTargetCategory category, uint8_t deckIndex, float normalisedValue, int16_t intDelta, uint64_t deviceId) noexcept -> BridgeWriteResult` method callable from the MIDI callback thread.
- [ ] The system's `dispatch` method, when the routing class is `AudioThread`, calls `juce::AbstractFifo::prepareToWrite(1)`, writes the event to the reserved slot, calls `finishedWrite(1)`, and returns `BridgeWriteResult::Ok`.
- [ ] The system's `dispatch` method, on a full audio FIFO, returns `BridgeWriteResult::DroppedFull` and atomically increments a `std::atomic<uint64_t> droppedFullCount` using `std::memory_order_relaxed`.
- [ ] The system's `dispatch` method, when the routing class is `MessageThread`, calls `juce::MessageManager::callAsync` with a lambda that invokes the registered `MessageThreadSink::onMidiMessageThreadEvent(const MidiMessageEvent&)` on the Message thread.
- [ ] The system's `dispatch` method performs zero heap allocations on the `AudioThread` routing path under all conditions including a full FIFO.
- [ ] The system's `dispatch` method performs zero `juce::CriticalSection::enter` calls, zero `std::mutex::lock` calls, and zero thread sleeps on either routing path.
- [ ] The system's `dispatch` method is marked `noexcept`.
- [ ] The system exposes `drainAudioThreadFifo(AudioMidiEventHandler& handler) noexcept` callable only from the audio thread, drains every ready event from the FIFO, and invokes `handler.applyAudioMidiEvent(const MidiAudioEvent&)` synchronously for each event.
- [ ] The system's `drainAudioThreadFifo` method performs zero heap allocations, zero locks, and zero I/O.
- [ ] The system's `drainAudioThreadFifo` method correctly handles the wrap-around case where `juce::AbstractFifo::prepareToRead` returns two non-contiguous blocks, processing both in order.
- [ ] The system exposes a `setMessageThreadSink(MessageThreadSink*)` method callable on the Message thread only, asserting via `JUCE_ASSERT_MESSAGE_THREAD` in Debug builds.
- [ ] The system exposes a `MessageThreadSink` interface with the single method `void onMidiMessageThreadEvent(const MidiMessageEvent&)`.
- [ ] The system exposes an `AudioMidiEventHandler` interface with the single method `void applyAudioMidiEvent(const MidiAudioEvent&) noexcept`.
- [ ] The system exposes a `getDroppedFullCount() const noexcept -> uint64_t` accessor returning the current drop counter via `std::memory_order_relaxed` load.
- [ ] The system runs a `juce::Timer` at 1 Hz on the Message thread that reads `getDroppedFullCount()` and, when it has increased since the previous tick, logs a warning via `DBG` (Message thread is permitted to log).
- [ ] The system's audio-thread FIFO has a fixed capacity of 1024 events. This is derived from a worst-case scratch burst (80 ticks/s × 8 simultaneous decks × 0.5 s burst = 320 events) with a 3× safety margin.
- [ ] The system delivers every inbound `AudioThread`-classified event to the audio engine within the same audio buffer period (≤ 256 frames at 44.1/48 kHz) under nominal load, verified by a stress test that injects 10,000 events at a controlled rate and asserts p99 callback-to-drain latency below 5 ms.
- [ ] The system delivers every inbound `MessageThread`-classified event to the registered sink within the next message-loop iteration, with no buffering or coalescing.
- [ ] The system declares the audio-thread-facing FIFO view in `Source/Features/AudioEngine/AudioEngineMidiBridge.h`, owned by `MidiMessageBridge` in `Source/Features/Midi/`, and consumed read-only by the audio engine.
- [ ] The system does not `#include` any header from `Source/Features/Deck/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.
- [ ] The system is covered by a `MidiMessageBridgeTests.cpp` unit test in `Tests/` that verifies: (a) `AudioThread` routing pushes to the FIFO; (b) `MessageThread` routing schedules a `callAsync`; (c) full-FIFO returns `DroppedFull` and increments the counter; (d) drain processes events in FIFO order across a wrap-around boundary; (e) `static_assert`s for POD triviality and routing-table completeness compile; (f) a stress test of 100,000 events on a producer thread produces zero allocations on the consumer thread (validated by a custom allocator hook in test build).
