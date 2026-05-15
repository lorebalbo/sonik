---
status: Implemented
epic: EPIC-0005
depends-on: [PRD-0040, PRD-0041, PRD-0042, PRD-0043]
---

# 1. PRD-0044: MIDI Input Routing and Inbound Command Dispatch

## 1.1. Problem

PRD-0040 delivers raw MIDI events. PRD-0041 provides a thread-safe bridge. PRD-0042 resolves an event into a typed `ResolvedBinding`. PRD-0043 makes mappings persist. But none of these PRDs actually *do anything* in the application yet: moving a channel fader on the Behringer DDM4000 still has no effect because no module takes a `ResolvedBinding` and turns it into a mixer-state or deck-state change.

The wiring layer between "we know what the user means" and "the user's intent reaches the deck atomics, the `juce::ValueTree`, the library, and the mixer" is the heart of the MIDI subsystem. Without it, every previous PRD is dead weight.

This wiring carries unusual risk because it has to integrate with **every existing Feature module** without introducing reverse dependencies into the MIDI module. Every existing surface — transport, pitch fader, jog wheel, hot cues, loops, beat-jump, library — exposes a different public contract: some are ValueTree writes (`isPlaying`, `gain`), some are direct atomic writes (`jogSpeedMultiplier`, `jogBendOffset`), some are method calls on a state manager (`HotCueManager::trigger(int padIndex)`, `LoopManager::setLoopIn(int sample)`), and some are command-bus messages (library scroll, load-to-deck). A naive implementation would have the MIDI router include every feature's header — circular dependencies, broken module boundaries, and a router that has to be modified every time any feature changes.

The router must also enforce the **routing decision** declared by PRD-0041 for every event: jog scratch/bend/touch go via the audio FIFO; everything else goes via `callAsync` to the Message thread. Getting this wrong means audio glitches on transport buttons or sluggish scratching.

Finally, the router carries **toggle-state semantics**: a `Toggle`-classified target needs to read the current ValueTree state on the Message thread and flip it. The MIDI callback thread cannot read the ValueTree (no thread safety guarantees for `juce::ValueTree::getProperty` from arbitrary threads), so toggles must be resolved on the Message thread after the `callAsync` lands.

Without this PRD, the Behringer DDM4000 stays inert and no later PRD has a downstream to plug into.

## 1.2. Objective

The system must implement the end-to-end inbound MIDI command-dispatch pipeline: receive `MidiInboundEvent` from PRD-0040, look up the active mapping from PRD-0043, resolve it to a `ResolvedBinding` via PRD-0042, route through PRD-0041's bridge to either the audio thread or the Message thread, and translate the resolved binding into the appropriate Feature-module call so the application surface responds identically to a mouse interaction. Specifically:

- The system ensures that every `MidiInboundEvent` produced by PRD-0040 is processed by a single `MidiInboundRouter` registered as a `MidiInputSubscriber`.
- The system ensures that the router is the only `MidiInputSubscriber` in the application — there is exactly one MIDI input consumer (other subscribers may exist for diagnostics, but none consume the same event stream for routing).
- The system ensures that, for every inbound MIDI event, the router (a) looks up the active mapping for the originating device via `MappingStore::getActiveMappingForDevice`, (b) maintains the per-device `ResolverState` and `ModifierMask`, (c) calls `BindingResolver::resolve`, and (d) calls `MidiMessageBridge::dispatch` with the resolved category, deck index, normalised value, int delta, and source device id.
- The system ensures that the router itself contains no Feature-module-specific code; all dispatch destinations are reached through a single `MidiCommandHandler` interface registered at application construction time. The router does not include any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.
- The system ensures that the application provides one or more `MidiCommandHandler` implementations (`DeckMidiHandler`, `MixerMidiHandler`, `LibraryMidiHandler`) that live **outside** `Source/Features/Midi/` and consume the routed events to write to the appropriate ValueTree nodes, deck atomics, or feature-module state managers. The handlers are owned by `SonikApplication` and injected into the router via constructor.
- The system ensures that `Toggle`-classified targets flip the corresponding ValueTree or feature-state value on the Message thread, atomically with respect to the GUI, using the current value as read by the handler.
- The system ensures that `RelativeDelta`-classified targets (jog scratch / jog bend) reach the audio thread via the `AudioMidiEventHandler` interface (registered with `MidiMessageBridge` per PRD-0041), which forwards into the audio engine's `jogSpeedMultiplier`, `jogBendOffset`, and `jogScratchActive` atomics defined by PRD-0018.
- The system ensures that the modifier-mask updates produced by `ResolvedBinding { category: ModifierSet | ModifierClear }` are applied to the per-device `ModifierMask` before any subsequent event for the same device is resolved, with appropriate memory ordering.
- The system ensures that **soft-takeover (pickup mode)** for continuous controls is left to PRD-0045; this PRD passes the `SoftTakeoverPolicy` field through to the handler but does not implement the suppression logic itself.
- The system ensures that **14-bit CC pairing** is handled inside `BindingResolver::resolve` (per PRD-0042), so this PRD requires no special treatment — the resolver produces a normalised float as if it were a single 7-bit message.

This PRD wires the entire inbound MIDI command surface. Soft-takeover (PRD-0045), modifier-layer composition (PRD-0046), LED feedback (PRD-0047), and the MIDI Learn UI (PRD-0048) are separate concerns that build on this routing core.

## 1.3. User Flow

This PRD has no end-user UI surface, but produces the first visible behaviour change in EPIC-0005: hardware buttons and knobs actually drive Sonik.

### 1.3.1. Application Startup: Router Construction

1. `SonikApplication::initialise()` constructs (in this order, on the Message thread): `MidiDeviceManager` (PRD-0040), `MidiMessageBridge` (PRD-0041), `MappingStore` (PRD-0043), then `MidiInboundRouter` last.
2. The application constructs the three command handlers — `DeckMidiHandler`, `MixerMidiHandler`, `LibraryMidiHandler` — owned by `SonikApplication` and given references to the root `juce::ValueTree`, the deck atomics view (audio-thread-shared via `AudioEngineMidiBridge.h`), and the appropriate Feature-module state managers.
3. The router is given a single `MidiCommandHandler*` reference. The handler is in practice a `CompositeMidiCommandHandler` that dispatches to the three concrete handlers based on the target's `MidiTargetCategory`. The composite lives in `Source/SonikApplication.cpp` so the router itself has no Feature awareness.
4. `MidiInboundRouter::initialise()` registers itself as a `MidiInputSubscriber` on `MidiDeviceManager` and as the `MessageThreadSink` on `MidiMessageBridge`. The audio engine separately registers itself as the `AudioMidiEventHandler` on `MidiMessageBridge`.
5. `MappingStore` is asked to open the Behringer DDM4000 input (and output, since the mixer also emits LED-driving MIDI) if the device is connected; the device is auto-opened via the `registerAutoOpenRule` registered against the bundled DDM4000 profile's device match.

### 1.3.2. Hardware Button Press: Channel A CUE

1. The user presses the **CUE** button on channel 1 of the Behringer DDM4000. The hardware emits `Note On, ch 1, note 0, velocity 127`.
2. `MidiDeviceManager` delivers a `MidiInboundEvent { deviceId, ts, 0x90, 0, 127 }` synchronously on the JUCE MIDI callback thread.
3. `MidiInboundRouter::onMidiInbound(event)` runs. It indexes its per-device state (a `std::array<DeviceState, MaxOpenDevices>` keyed by `deviceId` via a small in-thread hash, pre-allocated at first sight of the device) and retrieves the device's `ResolverState`, `ModifierMask`, and a `std::shared_ptr<const Mapping>` copy.
4. The router calls `BindingResolver::resolve(*mapping, resolverState, event, currentMask)`. The DDM4000 profile matches `(channel=1, status=note, data1=0)` to `mixer.channel.A.cue` (`MidiTargetCategory::MixerChannelCue`, `Toggle`).
5. The router calls `MidiMessageBridge::dispatch(MixerChannelCue, 0, 1.0f, 0, deviceId)`. The bridge's routing table classifies `MixerChannelCue` as `MessageThread`, so it schedules a `MessageManager::callAsync` containing a `MidiMessageEvent`.
6. The Message thread picks up the lambda and calls `MidiInboundRouter::onMidiMessageThreadEvent(event)`. The router calls `commandHandler.handle(event)`.
7. The composite handler dispatches by category: `MixerChannelCue` → `MixerMidiHandler::handleChannelCue(channelIndex, normalisedValue)`. The handler reads the current `cueEnabled` ValueTree property on the channel node, toggles it, and writes it back. The existing mixer-channel observer fires, the channel's headphone-cue routing updates, and the on-screen channel CUE button highlight updates via the existing UI binding. The DDM4000's channel CUE LED is also driven on by PRD-0047's outbound feedback engine.
8. Total round trip: ~3 ms from hardware press to ValueTree write (Message-thread `callAsync` overhead ≈ 1–2 ms, ValueTree write and observer fan-out < 1 ms) — well under the 50 ms human perception threshold.

### 1.3.3. Hardware Jog Wheel: Scratching Deck A (Future Jog-Capable Controller)

The DDM4000 has no jog wheel. This scenario describes the audio-thread routing path that the router must continue to support for future jog-capable controllers (and for users who MIDI-Learn jog targets against the Generic MIDI profile, see PRD-0048).

1. The user touches the platter (`jog.touch` Note On) then rotates it on a connected jog-capable controller. The controller emits scratch CC messages at ~80 Hz.
2. Each CC arrives in `MidiInboundRouter::onMidiInbound` on the MIDI callback thread.
3. Resolver produces `ResolvedBinding { category: JogScratch, deckIndex: 0, normalisedValue: 0.0f, intDelta: <signed bit decoded>, ... }`.
4. The router calls `MidiMessageBridge::dispatch(JogScratch, 0, 0.0f, intDelta, deviceId)`. The bridge's routing table classifies `JogScratch` as `AudioThread`, so it writes a `MidiAudioEvent` into the lock-free FIFO. No `callAsync`, no allocation.
5. At the top of the next `processBlock` (within ≤ 5 ms), the audio engine drains the FIFO and calls `AudioMidiEventHandler::applyAudioMidiEvent(event)`. The handler maps `(JogScratch, deckIndex=0, intDelta)` into the existing `jogSpeedMultiplier` / `jogScratchActive` atomics for Deck A, exactly as the mouse jog UI does today (per PRD-0018).
6. The audio engine produces scratch-modulated audio for the current buffer. The user hears the scratch with imperceptible latency.

### 1.3.4. SHIFT-Layered Action: Delete Hot Cue 1 (Future Controller With SHIFT)

The DDM4000 reference profile declares no modifiers. This scenario describes the resolver path that the router must continue to support for user-authored profiles bound to controllers with a dedicated SHIFT (or ALT, etc.) surface — typically third-party hardware learned against the Generic MIDI profile.

1. The user holds the controller's SHIFT button. SHIFT is itself a binding (`type: modifier`) producing `ResolvedBinding { category: ModifierSet, deckIndex: 255 /* global */, intDelta: 0 /* bit index */ }`.
2. The router's `onMidiInbound` recognises the `ModifierSet` category and **sets bit 0 of the device's `ModifierMask`** atomically (`std::memory_order_release`), without calling `dispatch`. No event reaches Feature modules.
3. The user presses HOT CUE 1 while SHIFT is held. The resolver's hash lookup finds two overloads for `(ch 1, note X)`: the plain binding to `deck.A.hotcue.1.trigger` and the SHIFT-layered binding to `deck.A.hotcue.1.delete`. The resolver consults `currentMask` (read with `std::memory_order_acquire`) and selects the SHIFT overload.
4. The resolved binding routes to the Message thread, the composite handler dispatches `HotCueDelete` to the `DeckMidiHandler`, which calls `HotCueManager::deleteCue(deckIndex=0, padIndex=0)`.
5. The user releases SHIFT. The momentary modifier binding produces `ResolvedBinding { category: ModifierClear, ... }` and the router clears bit 0.

### 1.3.5. Library Scroll & Load to Deck

1. The user turns the menu/edit encoder on the DDM4000. The hardware emits relative-CC `signedBitDelta` messages.
2. The resolver materialises `ResolvedBinding { category: LibraryScrollUp | LibraryScrollDown, intDelta: ±1 }`.
3. The router dispatches via `callAsync`. `LibraryMidiHandler::handleLibraryScroll(direction, magnitude)` writes the new selected row index into the existing library ValueTree node consumed by the `LibraryComponent`.
4. The user presses LOAD ▸ Deck A on a connected controller (DDM4000 has no LOAD button; this scenario assumes either a secondary controller or a key-command bound to a DDM4000 spare button via MIDI Learn). The handler writes the selected track's `filePath` into `IDs::pendingLoadPath` on Deck A (per PRD-0034's track-loading protocol). The existing `DeckShellComponent` listener picks it up and calls `AudioFileLoader::loadFile`, exactly as a drag-and-drop would.
5. The track loads. MIDI is fully integrated with the existing library flow without any library code knowing MIDI exists.

### 1.3.6. Unhandled Target

1. A future Sonik release introduces a new target `deck.A.filter` in `ControlTargetRegistry.h`, but the developer forgets to add a handler for it in `DeckMidiHandler`.
2. The user binds a knob to it via MIDI Learn (PRD-0048). The mapping persists.
3. On knob rotation, the resolver produces `ResolvedBinding { category: FilterKnob, ... }`. The router dispatches to the composite handler.
4. The composite handler has no registered route for `FilterKnob`. It logs a one-shot `DBG` warning naming the unhandled category and returns. No crash, no UI freeze.
5. The next time the same category arrives, the composite handler suppresses the warning to avoid log spam (one-shot per category per session).

## 1.4. Acceptance Criteria

- [ ] The system defines `MidiInboundRouter` under `Source/Features/Midi/` and constructs it on the Message thread after `MidiDeviceManager`, `MidiMessageBridge`, and `MappingStore`.
- [ ] The system's `MidiInboundRouter` registers itself as a `MidiInputSubscriber` on `MidiDeviceManager` via `addInputSubscriber(this)`.
- [ ] The system's `MidiInboundRouter` registers itself as the `MessageThreadSink` on `MidiMessageBridge` via `setMessageThreadSink(this)`.
- [ ] The system's `MidiInboundRouter` maintains a `std::array<DeviceState, 8> deviceStates` indexed by a tiny per-router hash of `deviceId`, with each `DeviceState` containing a `ResolverState`, an `std::atomic<uint32_t> modifierMask`, and a `std::shared_ptr<const Mapping> activeMapping` cache.
- [ ] The system's `MidiInboundRouter` listens for `MappingStoreListener::activeMappingChanged(deviceId)` on the Message thread and refreshes the cached `activeMapping` pointer for that device (via copy-assignment to the atomic-shared-ptr or a `std::mutex` covering only the write).
- [ ] The system's `MidiInboundRouter::onMidiInbound(const MidiInboundEvent&) noexcept` performs zero heap allocations.
- [ ] The system's `onMidiInbound` retrieves the device's cached `Mapping` via a wait-free atomic load (no shared mutex acquisition on the MIDI callback thread); when no mapping is cached, the event is dropped silently and an atomic `unmatchedEventCount` is incremented for diagnostics.
- [ ] The system's `onMidiInbound` calls `BindingResolver::resolve(mapping, resolverState, event, currentModifierMask)` to produce a `std::optional<ResolvedBinding>`. On `nullopt`, the event is dropped silently.
- [ ] The system's `onMidiInbound`, on a resolved binding with `category == ModifierSet`, sets the corresponding bit in the device's `modifierMask` via `fetch_or(1u << bit, std::memory_order_release)` and returns without dispatching.
- [ ] The system's `onMidiInbound`, on a resolved binding with `category == ModifierClear`, clears the corresponding bit via `fetch_and(~(1u << bit), std::memory_order_release)` and returns without dispatching.
- [ ] The system's `onMidiInbound`, for all other categories, calls `MidiMessageBridge::dispatch(category, deckIndex, normalisedValue, intDelta, deviceId)` and returns the result.
- [ ] The system's `MidiInboundRouter::onMidiMessageThreadEvent(const MidiMessageEvent&)` is invoked by `MidiMessageBridge` on the Message thread for every `MessageThread`-classified event.
- [ ] The system's `onMidiMessageThreadEvent` calls `MidiCommandHandler::handle(const MidiMessageEvent&)` on the injected handler reference.
- [ ] The system defines a `MidiCommandHandler` interface with a single method `void handle(const MidiMessageEvent&)` invoked on the Message thread.
- [ ] The system defines an `AudioMidiEventHandler` implementation owned by the audio engine, registered with `MidiMessageBridge::setAudioMidiEventHandler`, that maps each `MidiAudioEvent` to the appropriate audio-thread atomic write (e.g., `JogScratch` → `jogSpeedMultiplier`, `JogBend` → `jogBendOffset`, `JogTouch` → `jogScratchActive`) on the correct deck index, performing zero allocations.
- [ ] The system's audio handler asserts in Debug builds (`jassert`) that the event's `deckIndex` is within the valid range `[0, deckCount)` before writing the atomic.
- [ ] The system defines a `CompositeMidiCommandHandler` in `Source/SonikApplication.cpp` (or a sibling file in `Source/`) that owns references to the per-domain handlers (`DeckMidiHandler`, `MixerMidiHandler`, `LibraryMidiHandler`) and dispatches each `MidiMessageEvent` by `MidiTargetCategory` to the appropriate domain handler.
- [ ] The system's `CompositeMidiCommandHandler` logs a one-shot `DBG` warning the first time a category is encountered that has no registered dispatch, suppressing subsequent warnings for the same category in the same session.
- [ ] The system defines `DeckMidiHandler` under `Source/Features/Deck/` (or `Source/SonikApplication`) that, for every per-deck `MidiTargetCategory`, writes to the appropriate ValueTree node or feature-state manager: `TransportPlay` → toggle `isPlaying` on the deck node; `TransportCue` → call `CueManager::triggerCue(deckIndex)`; `TransportSync` → toggle `syncEnabled`; `PitchFader` → write `pitch` (mapped through current `pitchRange`); `Gain` → write `gain`; `EqHigh/Mid/Low` → write the corresponding EQ properties when present; `JogTouch` → currently mapped to audio-path (handled by `AudioMidiEventHandler`); `LoopIn/Out` → call `LoopManager::setLoopIn/Out` at current playhead; `LoopToggle` → toggle `Loop.active`; `LoopSizeHalve/Double` → call `LoopManager::halveSize/doubleSize`; `HotCueTrigger` → call `HotCueManager::trigger(padIndex)`; `HotCueDelete` → call `HotCueManager::deleteCue(padIndex)`; `BeatJumpMinus/Plus` → call `BeatJumpManager::jump(direction, size)`; `QuantizeToggle` → toggle `quantizeEnabled`; `SlipToggle` → toggle `slipEnabled`; `KeyShiftPlus/Minus` → increment/decrement `keyShift`; `KeyLockToggle` → toggle `keyLockEnabled`; `MasterTempoToggle` → toggle `isMasterTempo`.
- [ ] The system defines `MixerMidiHandler` that, for the global mixer category targets (`Crossfader`, `MasterGain`, `HeadphonesGain`, `HeadphoneCueToggle`), writes to the appropriate ValueTree mixer node. If the mixer ValueTree node does not yet exist (no Mixer Feature implemented at the time of this PRD's merge), the handler logs a one-shot `DBG` warning and no-ops.
- [ ] The system defines `LibraryMidiHandler` that, for `LibraryScrollUp/Down`, advances the library selection index in the library ValueTree node (or via the existing library command bus); for `LibraryLoadDeck`, writes the currently-selected track's file path to the target deck's `pendingLoadPath` per PRD-0034's track-loading protocol; for `LibraryFocusSearch`, focuses the library search box via the existing focus mechanism.
- [ ] The system's `MidiInboundRouter` does not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`. All dispatch goes through the `MidiCommandHandler` interface.
- [ ] The system's `CompositeMidiCommandHandler` and the three per-domain handlers may include Feature-module headers (they are application-level glue, not part of the MIDI feature module).
- [ ] The system passes the `SoftTakeoverPolicy` from `ResolvedBinding` through `MidiMessageEvent` (extending PRD-0041's POD if necessary, with `static_assert(std::is_trivially_copyable_v<MidiMessageEvent>)` preserved) so PRD-0045 can intercept on the Message-thread path.
- [ ] The system's end-to-end latency from hardware MIDI callback to deck atomic write (for jog targets) is verified ≤ 5 ms p99 in a stress test, and from MIDI callback to ValueTree property change (for Message-thread targets) is verified ≤ 16 ms p99 in a stress test.
- [ ] The system is covered by `MidiInboundRouterTests.cpp` in `Tests/` verifying: (a) inbound events targeting `TransportPlay` produce a Message-thread `handle` call with the correct category and deck index; (b) inbound jog events produce audio-FIFO writes; (c) modifier set/clear correctly updates the per-device mask and is observed by subsequent `resolve` calls; (d) an event for a device with no cached mapping is dropped silently and increments `unmatchedEventCount`; (e) `activeMappingChanged` notifications update the cached mapping pointer atomically; (f) a stress test of 100,000 events on a producer thread produces zero allocations in `onMidiInbound`; (g) the composite handler logs exactly one warning per unhandled category.
