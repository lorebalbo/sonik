---
name: "EPIC-0005: MIDI Controller System"
status: Open
---

# 1. EPIC-0005: MIDI Controller System

## 1.1. Goal and Vision

Make Sonik a fully hardware-driven DJ application. Every interactive control exposed in the UI — transport (play, cue, sync), pitch fader, gain and EQ knobs, crossfader, jog wheel (bend and scratch), loops, hot cues, beat-jump, library navigation, and deck loading — must be addressable from a connected MIDI controller, and the corresponding hardware indicators (LEDs, lit pads, transport buttons) must reflect application state in real time.

The system must support an open, versioned, human-readable mapping format so users and the community can author profiles for any controller, must ship a curated default profile for the **Reloop Contour Interface Edition** (first reference hardware), must allow **MIDI Learn** for any unsupported device, and must obey the application's real-time audio contract: MIDI messages targeting jog/scratch reach the audio thread through lock-free FIFOs in under 5 ms, with zero allocations or locks on the audio path.

The guiding principle is that the MIDI module is a **bidirectional adapter**, never an owner of state. It translates hardware events into the same atomics and ValueTree writes that the existing UI controls already produce, and translates state changes back into outbound MIDI for LED feedback. No existing Feature module needs to know that MIDI exists.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

The following capabilities are included in this Epic:

- MIDI device enumeration (inputs and outputs) via JUCE `MidiInput` / `MidiOutput`, with hot-plug detection (devices appearing or disappearing mid-session) and stable device-ID resolution by `(manufacturer, product, ordinal)`
- A real-time-safe message bridge that routes incoming MIDI from the JUCE MIDI callback thread to either the audio thread (jog/scratch atomics) or the Message thread (state-tree writes, UI updates), using lock-free FIFOs and atomics — never locks, never allocations on the audio path
- An abstract **Control Target Registry** that defines stable string IDs (e.g., `deck.A.transport.play`, `deck.B.jog.scratch`, `deck.A.hotcue.3.trigger`, `mixer.crossfader`) for every mappable application command, decoupled from UI layout
- A versioned **JSON mapping schema** describing, per binding: the inbound MIDI message (channel, status, data1), the optional 14-bit-CC LSB pair, the target control ID, the value-curve transform (linear, signed-bit jog delta, two's-complement, toggle, momentary), an optional modifier mask, an optional per-binding soft-takeover override, and an optional outbound `feedback` declaration
- Persistent mapping storage as JSON files in the user app-data folder (`~/Library/Application Support/Sonik/MidiMappings/` on macOS, equivalent on Windows), with schema validation on load and crash-safe fallback to bundled defaults on malformed input
- **Bundled default profile for the Reloop Contour Interface Edition**, curated from the reference `.tsi` and the Reloop manual, with LED feedback declarations for the controller's lit transport and cue buttons
- **Bundled "Generic MIDI" fallback profile** for unknown devices, enabling immediate MIDI Learn without prior configuration
- Inbound command dispatch wiring every existing application surface (transport, pitch, gain, EQ, crossfader, loop, hot cue, beat-jump, sync, library nav, track load-to-deck) to the binding resolver, including **14-bit CC pairing** for high-resolution faders
- **Soft-takeover (pickup mode)** for continuous controls (pitch, gain, EQ, crossfader, filter): hardware writes are suppressed until the hardware value crosses the current software value, with per-binding override (`always` / `never` / `pickup`)
- **Modifier / SHIFT-layer support**: bindings flagged `type: modifier` set a per-device modifier mask; other bindings declare a required mask
- **MIDI Output / LED feedback engine**: state-tree and atomic listeners on the Message thread translate state changes into outbound MIDI per the active profile's `feedback` declarations, including the 16-color hot-cue palette (from PRD-0012) mapped to per-device MIDI velocity tables
- **MIDI Learn UI**: a Settings panel listing detected devices, active profile per device, a per-binding profile editor, a MIDI-Learn workflow (click target → wiggle hardware → bind), conflict detection (warn + replace/cancel), per-binding curve/modifier/soft-takeover overrides, and "Reset to defaults" / "Duplicate profile" actions
- Hard latency budget: inbound MIDI → audio-thread atomic delivery for jog/scratch must complete in under 5 ms p99 at 44.1/48 kHz

### 1.2.2. Out of Scope

The following are explicitly excluded from this Epic:

- Mapping **import/export UI** with file picker, preview, and version migration (deferred to a v2 MIDI Epic; in v1, JSON files are user-editable on disk in a known folder)
- **USB-port disambiguation** for multiple identical physical devices connected simultaneously (the device-ID resolver reserves an `ordinal` field so this is forward-compatible without schema change)
- **Rekordbox / Serato / Traktor mapping import** (parsing proprietary `.tsi`, `.xml`, `.midimap` formats — would require reverse-engineering each vendor's command vocabulary and is not necessary to drive Sonik's controls)
- **HID controllers** (Native Instruments Traktor S-series, Pioneer DDJ HID-mode controllers): this Epic covers MIDI only. HID is a separate Epic.
- **Audio routing from controller-integrated soundcards** (controllers with built-in audio interfaces): the audio interface side is handled by the existing Audio Engine; only the MIDI surface of such controllers is in scope here.
- **Per-deck headphone cue MIDI buttons routed to actual cue audio**: requires a dedicated headphone cue routing PRD (already deferred in EPIC-0004) before MIDI can wire to it
- **Mapping marketplace / cloud sync** of community profiles

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Thread Model

JUCE delivers MIDI messages on its dedicated **MIDI callback thread**. The MIDI subsystem must never perform allocations, locks, file I/O, or logging inside the callback — JUCE makes no real-time guarantee about this thread, but the callback runs synchronously with hardware reception and any delay propagates into the user's perceived input latency.

Inside the MIDI callback, each incoming message is resolved through the active `BindingResolver` (a pure function over the loaded mapping, no allocations) and dispatched into **one of two paths** depending on the resolved target's category:

- **Audio-thread path** (jog scratch, jog bend, scratch speed multiplier): the resolved `(target, value)` is written into a fixed-capacity lock-free FIFO (`juce::AbstractFifo` over a pre-allocated `std::array<MidiEvent, N>`). The audio thread drains the FIFO at the top of `processBlock` and updates the relevant `std::atomic` deck values (e.g., `jogSpeedMultiplier`, `jogBendOffset`, `jogScratchActive` from PRD-0018). This path is the **only** way MIDI reaches the audio thread.
- **Message-thread path** (transport, pitch, gain, EQ, crossfader, loop, hot cue, beat-jump, sync, library nav): the resolved `(target, value)` is posted via `juce::MessageManager::callAsync`, which then calls the existing ValueTree setters / command handlers. This is identical to the path a mouse click takes today, so no Feature module needs MIDI-aware code.

For **outbound** MIDI (LED feedback), state-tree listeners run on the Message thread and emit MIDI synchronously through `juce::MidiOutput::sendMessageNow`. Outbound MIDI never originates on the audio thread.

### 1.3.2. Latency Budget

Inbound MIDI → audio-thread atomic delivery, measured from JUCE MIDI callback entry to the next `processBlock` reading the atomic, must complete in **under 5 ms p99** at 44.1 kHz / 48 kHz with audio buffer sizes ≤ 256 frames. This budget is reserved for the jog/scratch path. The Message-thread path has no hard budget (UI events tolerate 16 ms) but should target under 10 ms p99 for transport and cue feel.

### 1.3.3. Mapping Schema (Versioned JSON)

Each mapping file is a JSON document of the form:

```json
{
  "schemaVersion": 1,
  "device": {
    "manufacturer": "Reloop",
    "product": "Contour Interface Edition (MIDI)",
    "match": { "midiName": "Reloop Contour Interface Edition (MIDI)" }
  },
  "modifiers": [
    { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24, "type": "modifier" } }
  ],
  "bindings": [
    {
      "target": "deck.A.transport.play",
      "midi":   { "channel": 1, "status": "note", "data1": 14 },
      "transform": "momentary",
      "modifier": null,
      "feedback": { "channel": 1, "status": "note", "data1": 14, "onValue": 127, "offValue": 0,
                    "source": "valueTree", "key": "deck.A.isPlaying" }
    },
    {
      "target": "deck.A.pitchFader",
      "midi":   { "channel": 1, "status": "cc", "data1": 9, "data1Lsb": 41 },
      "transform": "linear14",
      "softTakeover": "pickup"
    },
    {
      "target": "deck.A.jog.scratch",
      "midi":   { "channel": 1, "status": "cc", "data1": 34 },
      "transform": "signedBitDelta",
      "ticksPerRevolution": 128
    }
  ]
}
```

The schema is **versioned** (`schemaVersion: 1`). Future breaking changes increment the version; the loader migrates older versions forward, never rejects them.

### 1.3.4. Control Target Registry

Every mappable application command has a stable string ID, defined in a single header (`Source/Features/Midi/ControlTargetRegistry.h`). IDs follow the pattern `<domain>.<scope>.<command>[.<index>]`. Examples:

- `deck.A.transport.play`, `deck.A.transport.cue`, `deck.A.transport.sync`
- `deck.A.pitchFader`, `deck.A.pitchRange.cycle`
- `deck.A.gain`, `deck.A.eq.high`, `deck.A.eq.mid`, `deck.A.eq.low`
- `deck.A.jog.scratch`, `deck.A.jog.bend`, `deck.A.jog.touch`
- `deck.A.loop.in`, `deck.A.loop.out`, `deck.A.loop.size.halve`, `deck.A.loop.size.double`, `deck.A.loop.active.toggle`
- `deck.A.hotcue.1.trigger` … `deck.A.hotcue.8.trigger`, `deck.A.hotcue.1.delete` (under SHIFT)
- `deck.A.beatjump.minus1`, `deck.A.beatjump.plus1`, etc.
- `mixer.crossfader`, `mixer.master.gain`, `mixer.headphones.gain`
- `library.scroll.up`, `library.scroll.down`, `library.load.deckA`, `library.load.deckB`, `library.focus.search`

**Decks are addressed by absolute ID (A/B/C/D), not by focus.** This is the natural fit for hardware like the Reloop Contour CE that has dedicated left/right transport sections wired to specific decks. The "focused-deck" routing model is not supported in v1.

### 1.3.5. Device-ID Resolution

A device is identified by the tuple `(manufacturer, product, ordinal)`. JUCE exposes `manufacturer` and `name` on each `juce::MidiDeviceInfo`. The `ordinal` defaults to 0 and is incremented when multiple devices share a `(manufacturer, product)` pair. **v1 uses ordinal 0 only** — the resolver simply picks the first match. The field exists in the schema so v2 can add USB-port disambiguation without a breaking change.

A device-name regex is permitted in the `device.match.midiName` field so vendors that vary suffixes (e.g., " (MIDI)", " Port 1") across OS versions still match.

### 1.3.6. Soft-Takeover (Pickup Mode)

For every continuous binding declared with `softTakeover: pickup`, the MIDI subsystem maintains a small per-binding state machine: `Idle → Engaged`. On profile load and on every load-to-deck event that resets the software value, the state resets to `Idle`. While `Idle`, the resolver computes the absolute distance between the incoming hardware value and the current software value; the hardware write is **suppressed** until the hardware value crosses the software value (sign of `hw - sw` flips since the last sample). On crossing, state advances to `Engaged` and all subsequent writes pass through directly.

`softTakeover: always` forces engagement immediately (no pickup), `softTakeover: never` is identical to `always`. Default for continuous bindings is `pickup`; default for momentary/toggle bindings is `always` (soft-takeover does not apply to buttons).

### 1.3.7. Modifier / SHIFT Layers

Modifiers are bindings flagged with `type: modifier`. The MIDI subsystem maintains a per-device 32-bit modifier mask. Each modifier binding owns one bit; pressing the hardware button sets the bit, releasing clears it (for `momentary` modifiers) or toggles it (for `toggle` modifiers — not used by Contour CE).

Every non-modifier binding declares an optional required modifier mask. The resolver matches a binding only when the active mask equals the required mask. This allows the same physical MIDI message to be bound to two different targets at two layers (e.g., `PLAY` plain → `deck.A.transport.play`; `SHIFT+PLAY` → `deck.A.transport.stutter`).

### 1.3.8. Crash-Safe Mapping Load

User mappings are loaded at application startup before any UI is shown. Each file is parsed with `juce::JSON::parse`; on parse failure, schema validation failure, or unknown `schemaVersion > current`, the system logs the file path and the validation error (via `DBG` on the Message thread only — never on the MIDI or audio thread), discards the file, and falls back to the bundled default for the matched device. **A malformed mapping must never prevent the application from launching.**

### 1.3.9. Module Boundary Rule

`Source/Features/Midi/` must not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/` directly. The only cross-module dependencies allowed are:

- The root `juce::ValueTree` (for posting state changes and for outbound LED-feedback listeners)
- A set of pre-allocated lock-free FIFOs to the audio thread for jog/scratch (declared in a shared `AudioEngineMidiBridge.h` header that lives in `Source/Features/AudioEngine/` and is consumed read-only by MIDI)
- The `ControlTargetRegistry` header (single source of truth for target IDs)

All feature interaction is expressed through these three contracts, exactly as the UI is.

### 1.3.10. Bundled Default Profiles

The build system embeds the bundled JSON profiles into the application binary via JUCE's `BinaryData` resource pipeline. The default profiles directory is `Resources/MidiMappings/` and contains at least:

- `reloop-contour-interface-edition.json` — full Reloop Contour CE mapping with LED feedback
- `generic-midi.json` — empty `bindings` array, used as a blank canvas for MIDI Learn against unknown devices

On first launch, defaults are copied (lazily, on demand) into the user mapping directory only if the user makes edits, so the bundled defaults always remain the source of truth for un-customized profiles.

## 1.4. PRD Roadmap

- [x] PRD-0040: MIDI I/O Subsystem & Device Manager
- [x] PRD-0041: RT-Safe MIDI Message Bridge
- [x] PRD-0042: Control Target Registry & Mapping Data Model
- [x] PRD-0043: Mapping Storage, Loading & Crash-Safe Defaults (with bundled Reloop Contour CE & Generic MIDI profiles)
- [x] PRD-0044: MIDI Input Routing & Inbound Command Dispatch (incl. 14-bit CC, value-curve transforms)
- [x] PRD-0045: Soft-Takeover (Pickup Mode) for Continuous Controls
- [ ] PRD-0046: Modifier / Shift Layer Support
- [x] PRD-0047: MIDI Output & LED Feedback Engine
- [ ] PRD-0048: MIDI Learn UI & Mapping Manager
