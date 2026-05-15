---
status: Implemented
epic: EPIC-0005
depends-on: [PRD-0040, PRD-0042, PRD-0043, PRD-0044]
---

# 1. PRD-0047: MIDI Output and LED Feedback Engine

## 1.1. Problem

A DJ controller is not just an input device. Every professional controller — the Reloop Contour Interface Edition included — has LEDs, ring meters, and sometimes screens that the host software is expected to **drive in response to application state**:

- The PLAY/PAUSE button LED must light when the deck is playing and turn off when paused.
- The CUE button LED must blink while the deck is cued at a cue point and not playing.
- The SYNC button LED must light when sync is active for the deck.
- Hot-cue pads must light in the cue's assigned colour when a cue is set on that slot, and remain dark when the slot is empty.
- The loop on/off LEDs must reflect the deck's active-loop state.
- The on-deck VU meters (where supported) must follow the channel's audible level.
- Pitch-fader-position LEDs (where supported) must reflect the soft-takeover state from PRD-0045 (e.g., flashing while disengaged, solid while engaged).

Without LED feedback, the controller is a one-way input device that gives the DJ no visual confirmation that their software state matches their intent. In a dark club, this is a deal-breaker: the DJ cannot see a tablet/laptop in detail, but they can see the LEDs on the controller in their peripheral vision while looking at the crowd.

PRDs 0040–0046 establish the inbound MIDI flow but say nothing about outbound MIDI. The application currently has no mechanism for:
- Translating ValueTree state changes (deck.A.transport.isPlaying = true) into outbound `juce::MidiOutput::sendMessageNow` calls.
- Mapping a logical feedback target (`deck.A.transport.play.led`) to the physical MIDI message that lights the corresponding LED on a specific device.
- Throttling outbound updates so a VU meter does not flood the device with 1000 messages/second.
- Suppressing redundant outbound writes (when the state hasn't changed since the last write).
- Coordinating device boot-up: when a device is hot-plugged, the host must push the *current* application state to it so the LEDs immediately reflect reality.
- Ensuring outbound MIDI never originates from the audio thread (per Epic §1.3.1).

## 1.2. Objective

The system must implement a MIDI output and LED feedback engine that listens to `juce::ValueTree` state changes on the Message thread, translates them through per-binding feedback rules in the active mapping, and sends outbound MIDI messages via `juce::MidiOutput::sendMessageNow` from a single dedicated worker. Specifically:

- The system ensures that every binding in a mapping file MAY declare an optional `feedback` block describing how the binding's target's software state translates to outbound MIDI for that specific physical control.
- The system ensures that the feedback block supports three styles: `binary` (state on / off → fixed Note On velocities), `colour` (state → 16-entry colour palette → per-device velocity table), and `continuous` (state → 7-bit CC value via a linear curve).
- The system ensures that a `MidiFeedbackEngine` singleton (per the Feature, not global) lives on the Message thread and listens to the application's root `juce::ValueTree` for property changes.
- The system ensures that on each property change, the engine looks up every `(device, binding)` that has a `feedback` block whose `source` matches the changed property, computes the outbound MIDI message, and enqueues it for transmission.
- The system ensures that outbound MIDI is transmitted from a dedicated `juce::Thread` (the "MIDI Output Thread") that drains a Message-thread → Output-thread FIFO and calls `juce::MidiOutput::sendMessageNow`. This keeps `sendMessageNow` (which may briefly block on driver-level mutexes on some platforms) off the Message thread.
- The system ensures that outbound MIDI is rate-limited per-device to ≤ 1000 messages/second using a token-bucket throttle on the Output thread, with redundant-message coalescing (identical consecutive messages to the same `(device, status, data1)` triple are collapsed into the latest).
- The system ensures that when a device is hot-plugged (`midiDeviceAdded`), the engine performs a "boot dump": iterates every binding with a `feedback` block, computes the outbound message from the current application state, and enqueues all of them with a 5 ms inter-message stagger to avoid driver buffer overflow.
- The system ensures that when a device is disconnected (`midiDeviceRemoved`), the engine drops every queued outbound message for that device.
- The system ensures that when the active mapping for a device changes (`activeMappingChanged`), the engine performs another boot dump for the new mapping (and conceptually a "blackout dump" first, sending Note Off for every binding in the *old* mapping with a `binary` or `colour` feedback block, to extinguish any LEDs the new mapping does not address).
- The system ensures that outbound MIDI is **never** initiated from the audio thread; the audio thread does not interact with the feedback engine in any way.
- The system ensures that the engine respects the soft-takeover state from PRD-0045: for continuous controls with `softTakeover: pickup` and state `Disengaged`, the feedback (e.g., a "disengaged" indicator LED) overrides the regular feedback rule.

## 1.3. User Flow

### 1.3.1. Loading the Application With a Connected Reloop Contour CE

1. The application starts. `MidiDeviceManager` (PRD-0040) detects the Contour CE; `MappingStore` (PRD-0043) resolves to the bundled Reloop profile.
2. `MidiFeedbackEngine` is constructed by `SonikApplication` and given references to the root `ValueTree`, `MappingStore`, `MidiDeviceManager`, and `SoftTakeoverManager`.
3. Both decks are in their default state (paused, no track loaded). `MidiFeedbackEngine::onActiveMappingChanged(deviceId)` is called on the Message thread.
4. The engine iterates every binding with a `feedback` block in the bundled Reloop profile. For each, it reads the corresponding source value from the ValueTree (e.g., `deck.A.transport.isPlaying = false`) and computes the outbound MIDI message (Note On, note 18, velocity 0 → "PLAY LED off"). Each message is enqueued in the per-device output FIFO.
5. The Output Thread drains the FIFO, calling `device.midiOutput->sendMessageNow(msg)` with a 5 ms stagger.
6. The Contour CE's LEDs settle into the application's idle state: every LED off, hot-cue pads dark.

### 1.3.2. User Presses PLAY on Deck A

1. The user presses the PLAY button on the Contour CE. Inbound MIDI flows through PRD-0040 → PRD-0044 → `DeckMidiHandler::handleCommand`, which sets `ValueTree::getChildWithName("decks").getChildWithName("A").setProperty(IDs::isPlaying, true)`.
2. `MidiFeedbackEngine`'s `ValueTree::Listener::valueTreePropertyChanged` fires on the Message thread.
3. The engine looks up the binding feedback table for `(deviceId=Contour-CE, source=deck.A.transport.isPlaying)`. It finds: `{ style: binary, midi: { status: note, channel: 1, data1: 18 }, onVelocity: 127, offVelocity: 0 }`.
4. The new value is `true` → velocity 127. The engine enqueues Note On, ch 1, note 18, vel 127.
5. The Output Thread drains the FIFO; the PLAY LED lights solid.

### 1.3.3. User Sets Hot Cue 1 With a Colour

1. The user sets hot cue 1 on Deck A with the orange colour preset. `ValueTree` is updated: `deck.A.hotcue.1.isSet = true`, `deck.A.hotcue.1.colourIndex = 4` (palette index for orange).
2. Two property-change events fire on the Message thread.
3. The engine resolves the feedback binding `(deviceId, source=deck.A.hotcue.1)`. The binding declares `{ style: colour, midi: { status: note, channel: 5, data1: 32 }, palette: { 0: 0, 1: 12, 2: 24, ..., 4: 48, ... } }` — a per-device velocity table mapping palette index → velocity.
4. The engine reads `colourIndex = 4`, looks up palette → velocity 48, enqueues Note On, ch 5, note 32, vel 48.
5. The hot-cue pad lights solid orange.

### 1.3.4. Soft-Takeover Disengaged Indicator

1. The user loads a new track to Deck A. `pitch` resets to 0; soft-takeover for the pitch fader transitions to `Disengaged` (PRD-0045).
2. `MidiFeedbackEngine` listens to `SoftTakeoverManager::stateChanged(deviceId, target, newState)` (added in this PRD).
3. For the pitch-fader binding, the engine consults the binding's optional `disengagedFeedback` field: `{ style: binary, midi: { status: note, channel: 1, data1: 19 }, blinkHz: 2 }`.
4. The engine activates a 2 Hz blink for note 19 on channel 1 — a small per-target timer on the Message thread alternates Note On velocities 0 ↔ 127 every 250 ms.
5. When the user crosses the pitch value and soft-takeover transitions to `Engaged`, the engine receives `stateChanged(…, Engaged)`, cancels the blink, and reverts to the regular feedback rule (which, for the pitch fader, is "no feedback" — the LED stays off).

### 1.3.5. Rate-Limiting a VU Meter

1. A future deck-level VU meter binding declares `{ style: continuous, source: deck.A.meter.peak, midi: { status: cc, channel: 1, data1: 64 }, curve: linear }`.
2. The audio engine writes meter samples to the ValueTree at 60 Hz (audio-engine→UI summary path, well below 1000 Hz).
3. Each write fires a property-change event; the engine enqueues an outbound CC message.
4. Under load the engine could in principle be asked to emit more than 1000 messages/second across all bindings; the Output Thread's token-bucket throttle drops the lowest-priority redundant duplicates (per-`(device, status, data1)` coalescing) before they hit the driver.

### 1.3.6. Device Hot-Plug After Application Startup

1. The user connects a second device — a future generic-MIDI controller — mid-set. `MidiDeviceManager` fires `midiDeviceAdded(deviceId)`.
2. `MidiFeedbackEngine::onDeviceAdded` triggers a boot dump for the new device. The engine iterates every binding in the resolved mapping, computes the message from current state, and enqueues all of them (5 ms staggered).
3. Within a fraction of a second, the new device's LEDs reflect the application's current state without the user touching anything.

## 1.4. Acceptance Criteria

- [ ] The system defines `MidiFeedbackEngine` under `Source/Features/Midi/` as a Message-thread-owned class implementing `juce::ValueTree::Listener`, `MappingStoreListener`, `MidiDeviceManagerListener`, and `SoftTakeoverManagerListener`.
- [ ] The system extends the mapping JSON schema to support an optional `feedback` block on each binding with the following structure: `{ style: "binary" | "colour" | "continuous", source: <ValueTree path string>, midi: { status: "note" | "cc", channel: u8, data1: u8 }, onVelocity?: u8, offVelocity?: u8, palette?: {<index>: u8, ...}, curve?: "linear" | "linearInverse" }`.
- [ ] The system extends `MappingParser::parse` (PRD-0042) to populate a `std::optional<FeedbackBinding>` field on every parsed `Binding`. `FeedbackBinding` is a POD trivially-copyable struct.
- [ ] The system defines `FeedbackStyle` as an enum class with values `Binary`, `Colour`, `Continuous`.
- [ ] The system defines `FeedbackBinding` containing: `FeedbackStyle style`, `uint8_t midiStatus`, `uint8_t midiChannel`, `uint8_t midiData1`, `uint8_t onVelocity`, `uint8_t offVelocity`, `TargetIndex sourceTarget`, `uint8_t paletteVelocities[16]`, `FeedbackCurve curve`. Static-asserted trivially copyable.
- [ ] The system defines an optional `disengagedFeedback` field on `Binding` of the same `FeedbackBinding` type, plus a `float blinkHz` field (range `0.0–10.0`, where `0` means "solid no-blink").
- [ ] The system builds and maintains a flat `std::vector<std::pair<ValueTreePath, std::vector<FeedbackEntry>>>` index on `MidiFeedbackEngine::onActiveMappingChanged`, allowing O(log n) lookup from property path to outbound-message generators per device.
- [ ] The system's `valueTreePropertyChanged` reads the new value, computes the outbound `juce::MidiMessage` per the binding's style and curve, and pushes it into the per-device output FIFO.
- [ ] The system defines the per-device output FIFO as a `juce::AbstractFifo`-backed `OutboundMidiFifo` with capacity 1024, storing `OutboundMidiEvent { uint64_t deviceId, juce::MidiMessage message, std::chrono::steady_clock::time_point earliestSendTime }`.
- [ ] The system defines a dedicated `MidiOutputThread : juce::Thread` that wakes on FIFO writes, drains pending events in send-time order, applies per-device token-bucket throttling (capacity 1000 tokens, refill 1000 tokens/sec, 1 token per message), and calls `device.midiOutput->sendMessageNow(message)`.
- [ ] The system's `MidiOutputThread` coalesces redundant consecutive messages: when draining, if the next message and the current message share the same `(deviceId, status, data1)` triple, the older message is discarded and only the latest is sent.
- [ ] The system performs a **boot dump** on `MidiDeviceManagerListener::midiDeviceAdded(deviceId)` and on `MappingStoreListener::activeMappingChanged(deviceId)`: enqueues outbound messages for every binding's `feedback` block, computed from current `ValueTree` state, with a 5 ms `earliestSendTime` stagger per message.
- [ ] The system performs a **blackout dump** preceding the boot dump on `activeMappingChanged`: enqueues Note On velocity 0 / CC 0 for every `binary` or `colour` feedback binding in the *outgoing* mapping that is not addressed by the *incoming* mapping.
- [ ] The system drops every queued outbound message for `deviceId` on `MidiDeviceManagerListener::midiDeviceRemoved(deviceId)`.
- [ ] The system handles soft-takeover state changes via a new `SoftTakeoverManagerListener` interface (added in this PRD; implemented by `MidiFeedbackEngine`). On `stateChanged(deviceId, target, Disengaged)`, the engine activates the binding's `disengagedFeedback` (including starting a Message-thread timer for `blinkHz > 0`). On `stateChanged(…, Engaged)`, the engine cancels the timer and emits the binding's regular feedback for the current source value.
- [ ] The system never calls `juce::MidiOutput::sendMessageNow` from the audio thread. `MidiFeedbackEngine` and `MidiOutputThread` have no audio-thread entry points.
- [ ] The system never calls `juce::MidiOutput::sendMessageNow` from the Message thread; all sends originate on `MidiOutputThread`.
- [ ] The system asserts via `JUCE_ASSERT_MESSAGE_THREAD` on every public method of `MidiFeedbackEngine` except those invoked by `MidiOutputThread`.
- [ ] The system's bundled Reloop profile (PRD-0043) declares `feedback` blocks for every lit control on the Contour CE: PLAY/CUE/SYNC LEDs, hot-cue pads with the 16-colour palette, loop on/off LEDs, deck-active LED, optional VU meters per the device's CE-revision capabilities. The blackout dump on profile-swap turns all of them off cleanly.
- [ ] The system is covered by `MidiFeedbackEngineTests.cpp` in `Tests/` verifying: (a) property change produces enqueued outbound message; (b) binary style emits the configured on/off velocities; (c) colour style emits the per-palette-index velocity; (d) continuous style emits the linear-curve CC; (e) boot dump enqueues every feedback binding's current value; (f) blackout dump on profile-change emits Note On velocity 0 for old-mapping-only bindings; (g) device-disconnect drops queued messages; (h) the token-bucket throttle caps output at 1000 msg/s; (i) consecutive-same-key coalescing collapses duplicates; (j) disengaged blink fires at the declared `blinkHz`; (k) `sendMessageNow` is never invoked on the Message thread or audio thread.
