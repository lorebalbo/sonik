---
status: Implemented
epic: EPIC-0005
depends-on: []
---

# 1. PRD-0040: MIDI I/O Subsystem and Device Manager

## 1.1. Problem

Sonik has no awareness of MIDI hardware. A DJ who connects a MIDI controller — for example the Behringer DDM4000 — sees no indication that the device exists, has no way to discover which controllers macOS or Windows has exposed to the application, and cannot send a single byte of data from hardware to the audio engine. Every subsequent feature in EPIC-0005 (mapping data model, inbound routing, LED feedback, MIDI Learn) is blocked: there is nothing to route, nothing to map, and no surface against which to learn.

The absence of a device-management layer is particularly damaging because MIDI hardware is dynamic: controllers are plugged and unplugged mid-session, USB hubs lose power, drivers reset. Any naive enumeration done once at startup becomes stale the moment a cable is touched. Worse, JUCE delivers incoming MIDI on a dedicated callback thread separate from the JUCE Message Thread and the audio thread, so even after a device is detected, the application has no contract for who consumes its messages and on which thread — a setup that, if rushed, will produce data races against deck atomics, allocations on the MIDI callback thread, or message duplication between subscribers.

Professional DJs using hardware controllers as their primary performance surface are directly affected. Without a stable, hot-pluggable device manager, the application is unusable for the live workflow it intends to support, regardless of how polished the rest of the feature set becomes.

## 1.2. Objective

The system must enumerate every MIDI input and output device exposed by the host OS, keep that enumeration accurate across hot-plug events, and provide a typed contract for subscribers to receive raw MIDI messages without interfering with the audio thread or the Message thread. Specifically:

- The system ensures that, on application startup, every MIDI input and output device currently visible to the host OS is enumerated and assigned a stable in-process device ID.
- The system ensures that devices appearing after startup (a USB controller plugged in mid-session) are detected and registered without restarting the application.
- The system ensures that devices disappearing after startup (a USB controller unplugged) are de-registered, their open input streams closed, and any subscriber notified, without crashing the application.
- The system ensures that every incoming MIDI message from any opened input device is delivered to every registered subscriber via a single well-defined callback signature, on the JUCE MIDI callback thread, with zero allocations and zero locks inside the callback.
- The system ensures that opened output devices expose a thread-safe `sendMessage` method that can be called from the Message thread (only).
- The system ensures that a device's stable in-process ID is derived from the tuple `(manufacturer, product, ordinal)`, so subsequent PRDs can persist mapping bindings by device ID and resolve them across application restarts.
- The system ensures that the entire device-management surface lives under `Source/Features/Midi/` and does not include any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.

No mapping, no routing, no UI for binding edits, and no LED feedback are part of this PRD — only enumeration, hot-plug, open/close lifecycle, subscriber dispatch, and stable IDs.

## 1.3. User Flow

This PRD has no end-user UI; its "users" are the subsequent PRDs in EPIC-0005 (the binding resolver, the inbound routing engine, the MIDI Learn panel) and, indirectly, the DJ who plugs and unplugs hardware. The flow is therefore expressed as a sequence of system events with observable side effects.

### 1.3.1. Application Startup with One Controller Connected

1. `SonikApplication::initialise()` constructs a `MidiDeviceManager` on the Message thread, after `TrackDatabase` and before any UI.
2. `MidiDeviceManager::initialise()` calls `juce::MidiInput::getAvailableDevices()` and `juce::MidiOutput::getAvailableDevices()` to enumerate every MIDI device currently exposed by the host OS.
3. For each enumerated input device, the manager constructs a `MidiDeviceRecord` with fields `(deviceId, juceIdentifier, manufacturer, productName, ordinal, isInput=true, isOpen=false)`. Output devices receive an equivalent record with `isInput=false`.
4. `deviceId` is computed from a SHA-1 hash of the string `"<manufacturer>|<productName>|<ordinal>"`, truncated to 64 bits. The `ordinal` defaults to 0 and is incremented (0, 1, 2, …) when a duplicate `(manufacturer, productName)` is seen during the same enumeration pass, in JUCE enumeration order.
5. The manager does **not** open any input or output device at this stage. Opening is deferred until a subscriber explicitly requests it via `openInput(deviceId)` or `openOutput(deviceId)`. The Behringer DDM4000 record is present in the device list with `isOpen=false`.
6. The manager exposes the current device list via `getDevices()` and publishes a `DeviceListChangeListener` callback to all registered listeners on the Message thread.

### 1.3.2. Subscriber Opens an Input Device

1. A subscriber (in a later PRD, the mapping engine) calls `MidiDeviceManager::openInput(deviceId)` on the Message thread.
2. The manager looks up the `MidiDeviceRecord` by `deviceId`. If not found, the method returns `MidiOpenResult::DeviceNotFound`. If already open, it returns `MidiOpenResult::AlreadyOpen` (idempotent success).
3. The manager calls `juce::MidiInput::openDevice(juceIdentifier, this)`, where `this` is the manager acting as the `juce::MidiInputCallback`. The returned `std::unique_ptr<juce::MidiInput>` is stored in the record and `isOpen` is set to `true`.
4. The manager calls `start()` on the `juce::MidiInput` instance so that messages begin arriving on the JUCE MIDI callback thread.
5. The method returns `MidiOpenResult::Ok`.
6. From this point on, every byte the Behringer DDM4000 produces invokes the manager's `handleIncomingMidiMessage(MidiInput*, const MidiMessage&)` method.

### 1.3.3. Incoming Message Dispatch

1. The OS delivers a MIDI message; JUCE invokes `MidiDeviceManager::handleIncomingMidiMessage` on its dedicated MIDI callback thread.
2. The manager resolves the originating `deviceId` by indexing a pre-allocated `std::array<DeviceIdLookup, MaxDevices>` keyed by the raw `juce::MidiInput*` pointer (set up at `openInput` time on the Message thread, never written from the MIDI callback thread).
3. The manager constructs a small POD `MidiInboundEvent { deviceId, timestampSamples, statusByte, data1, data2 }` on the stack.
4. The manager iterates a pre-allocated, lock-free, fixed-capacity subscriber list (`std::array<MidiInputSubscriber, MaxSubscribers>`) and invokes each subscriber's `onMidiInbound(const MidiInboundEvent&)` synchronously.
5. The callback returns. No memory was allocated, no `juce::String` was constructed, no `std::vector` was resized, no mutex was acquired. All of the above is verified by the acceptance criteria below.

### 1.3.4. Hot-Plug: Device Appears Mid-Session

1. A `juce::Timer` owned by `MidiDeviceManager` fires every 1000 ms on the Message thread.
2. The timer callback re-enumerates `juce::MidiInput::getAvailableDevices()` and `juce::MidiOutput::getAvailableDevices()`.
3. The manager diffs the new list against its current records. Newly-present devices are inserted as `MidiDeviceRecord` entries with `isOpen=false` and assigned a fresh `deviceId` per §1.3.1 step 4.
4. The manager fires `DeviceListChangeListener::midiDeviceAdded(deviceId)` on the Message thread for every new device.
5. Subscribers that pre-registered an interest in a specific `(manufacturer, productName)` pair (via `registerAutoOpenRule`) receive an automatic `openInput(deviceId)` and `openOutput(deviceId)` call. This is the mechanism that will allow a saved Behringer DDM4000 mapping (loaded in a later PRD) to "auto-attach" the moment the user plugs the device in.

### 1.3.5. Hot-Plug: Device Disappears Mid-Session

1. The 1000 ms timer callback re-enumerates and finds that the `juceIdentifier` of an open device is no longer present.
2. The manager calls `stop()` on the corresponding `juce::MidiInput`, then releases the `unique_ptr` (the destructor closes the OS-level handle).
3. The manager's pre-allocated `DeviceIdLookup` slot is cleared (`MidiInput*` set to `nullptr`) on the Message thread. The next inbound callback from any device cannot match the freed pointer, so no stale dispatch occurs.
4. The manager fires `DeviceListChangeListener::midiDeviceRemoved(deviceId)` on the Message thread for every disappeared device.
5. The `MidiDeviceRecord` for the device remains in the list, with `isOpen=false` and a new field `isConnected=false`. This preserves the persistent `deviceId` so that mapping bindings referencing the device survive an unplug/replug cycle and re-attach automatically.

### 1.3.6. Subscriber Opens an Output Device and Sends a Message

1. A subscriber (in a later PRD, the LED feedback engine) calls `MidiDeviceManager::openOutput(deviceId)` on the Message thread.
2. The manager opens the `juce::MidiOutput` via `juce::MidiOutput::openDevice(juceIdentifier)` and stores the `unique_ptr` in the record. `isOpen` is set to true.
3. The subscriber calls `MidiDeviceManager::sendOutput(deviceId, const juce::MidiMessage&)` on the Message thread.
4. The manager looks up the `MidiOutput` and calls `sendMessageNow`. The OS dispatches the bytes to the hardware. The hardware LED responds.
5. `sendOutput` may **only** be called from the Message thread. The method asserts this in Debug builds via `JUCE_ASSERT_MESSAGE_THREAD`. Calling it from the audio thread or the MIDI callback thread is a programming error.

### 1.3.7. Application Shutdown

1. `MidiDeviceManager::~MidiDeviceManager()` runs on the Message thread.
2. The timer is stopped.
3. For every open input device, `stop()` is called and the `unique_ptr` is reset.
4. For every open output device, the `unique_ptr` is reset.
5. The OS releases all MIDI handles. No background thread remains.

## 1.4. Acceptance Criteria

- [ ] The system constructs `MidiDeviceManager` on the JUCE Message thread before any other Feature module that depends on MIDI is constructed.
- [ ] The system calls `juce::MidiInput::getAvailableDevices()` and `juce::MidiOutput::getAvailableDevices()` during initialisation and stores one `MidiDeviceRecord` per device.
- [ ] The system computes each `deviceId` as a 64-bit value derived from a SHA-1 hash of the string `"<manufacturer>|<productName>|<ordinal>"`, truncated to the low 64 bits.
- [ ] The system assigns `ordinal=0` to the first occurrence of a `(manufacturer, productName)` pair in JUCE enumeration order and increments the ordinal for each subsequent duplicate pair seen during the same enumeration pass.
- [ ] The system does not open any MIDI input or output device during initialisation; opening is performed exclusively in response to an explicit `openInput(deviceId)` or `openOutput(deviceId)` call.
- [ ] The system exposes a `getDevices() const` method that returns a snapshot `std::vector<MidiDeviceRecord>` of all currently-known devices, callable from the Message thread.
- [ ] The system exposes a `DeviceListChangeListener` interface with the methods `midiDeviceAdded(uint64_t deviceId)`, `midiDeviceRemoved(uint64_t deviceId)`, and `midiDeviceOpened(uint64_t deviceId)` / `midiDeviceClosed(uint64_t deviceId)`, with all callbacks invoked on the Message thread.
- [ ] The system polls `juce::MidiInput::getAvailableDevices()` and `juce::MidiOutput::getAvailableDevices()` every 1000 ms on the Message thread via a `juce::Timer`.
- [ ] The system fires `midiDeviceAdded` for every newly-detected device on a hot-plug poll and `midiDeviceRemoved` for every disappeared device.
- [ ] The system preserves `MidiDeviceRecord` entries for disappeared devices (with `isConnected=false`, `isOpen=false`) so that subsequent PRDs can re-attach saved mappings by `deviceId` on replug.
- [ ] The system re-uses the same `deviceId` when a device with identical `(manufacturer, productName, ordinal)` reappears after disconnection, so persistent mappings survive an unplug/replug cycle.
- [ ] The system supports `registerAutoOpenRule(manufacturerRegex, productNameRegex)` that automatically calls `openInput` and `openOutput` for any future device whose identification fields match both regexes.
- [ ] The system implements `juce::MidiInputCallback::handleIncomingMidiMessage` such that the function performs no heap allocations, constructs no `juce::String`, calls no `juce::CriticalSection::enter`, and writes no log output.
- [ ] The system delivers every incoming MIDI message to every registered subscriber by invoking `subscriber.onMidiInbound(const MidiInboundEvent&)` synchronously inside the JUCE MIDI callback.
- [ ] The system exposes a `MidiInputSubscriber` interface with a single method `void onMidiInbound(const MidiInboundEvent&) noexcept`, and registers subscribers via `addInputSubscriber(MidiInputSubscriber*)` / `removeInputSubscriber(MidiInputSubscriber*)` called on the Message thread only.
- [ ] The system stores the subscriber list in a fixed-capacity `std::array<MidiInputSubscriber*, 16>` with `std::atomic<int>` count, so iteration on the MIDI callback thread reads no heap-allocated container.
- [ ] The system resolves the originating `deviceId` for each inbound message in O(1) by indexing a pre-allocated `std::array<DeviceIdLookup, 32>` keyed by `juce::MidiInput*`, populated on the Message thread at `openInput` time.
- [ ] The system exposes a `MidiInboundEvent` POD with members `uint64_t deviceId; double timestampSeconds; uint8_t statusByte; uint8_t data1; uint8_t data2;` and a `static_assert(std::is_trivially_copyable_v<MidiInboundEvent>)`.
- [ ] The system exposes `openInput(uint64_t deviceId) -> MidiOpenResult`, `openOutput(uint64_t deviceId) -> MidiOpenResult`, `closeInput(uint64_t deviceId)`, and `closeOutput(uint64_t deviceId)`, all callable on the Message thread only.
- [ ] The system asserts via `JUCE_ASSERT_MESSAGE_THREAD` at the entry of every public Message-thread-only method in Debug builds.
- [ ] The system exposes `sendOutput(uint64_t deviceId, const juce::MidiMessage&)`, callable only on the Message thread, and asserts the thread invariant in Debug builds.
- [ ] The system returns `MidiOpenResult::DeviceNotFound` when `openInput` / `openOutput` is called with an unknown `deviceId`, `MidiOpenResult::AlreadyOpen` when called on an already-open device (idempotent success), and `MidiOpenResult::OsRefused` when the underlying JUCE call fails.
- [ ] The system stops the `juce::MidiInput` and releases its `unique_ptr` for every device removed by a hot-plug poll, before firing `midiDeviceRemoved`.
- [ ] The system fires `midiDeviceClosed` exactly once for any open device removed by a hot-plug poll, and does not fire `midiDeviceRemoved` until after the close callback has returned.
- [ ] The system does not invoke any subscriber callback from a thread other than the JUCE MIDI callback thread for `onMidiInbound`, or the Message thread for `DeviceListChangeListener` methods.
- [ ] The system in its destructor stops every open `juce::MidiInput`, releases every `juce::MidiInput` and `juce::MidiOutput` `unique_ptr`, and stops the hot-plug `juce::Timer` before returning.
- [ ] The system lives entirely under `Source/Features/Midi/` and does not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.
- [ ] The system is covered by a `MidiDeviceManagerTests.cpp` unit test in `Tests/` that fakes `juce::MidiInput::getAvailableDevices()` via a `MidiHostFake` seam and verifies: stable `deviceId` derivation, ordinal disambiguation of duplicates, add/remove callbacks on hot-plug, auto-open-rule matching, idempotent re-open, re-use of `deviceId` after replug, and zero allocations on a stress run of 10,000 simulated inbound messages.
