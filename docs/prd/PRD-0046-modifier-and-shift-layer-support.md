---
status: Not Implemented
epic: EPIC-0005
depends-on: [PRD-0042, PRD-0043, PRD-0044]
---

# 1. PRD-0046: Modifier and Shift Layer Support

## 1.1. Problem

Hardware controllers like the Reloop Contour Interface Edition have far more app commands than physical surfaces. The Contour CE has only four pads per side, yet a DJ needs to access at least eight hot cues per deck, plus delete-cue actions, plus per-cue colour changes — totalling ~24 distinct commands per side from four physical pads. Every DJ controller solves this with **modifier keys (SHIFT, ALT, etc.)** that re-layer the surface: holding SHIFT turns the four hot-cue pads into four hot-cue *delete* buttons, and ALT might turn them into colour pickers.

PRDs 0042 and 0044 already laid groundwork:
- PRD-0042 defines `Modifier` POD structs in `Mapping`, the resolver's hash-bucket support for up to four modifier-layered overloads per MIDI key, and the `ResolvedBinding { category: ModifierSet | ModifierClear }` special return.
- PRD-0044 maintains a per-device `std::atomic<uint32_t> modifierMask` and applies `Set`/`Clear` operations on inbound modifier events.

But what is **not yet defined** is the *semantics* of those operations beyond "set/clear one bit": held-vs-latched modifiers, behaviour when a modifier is held across a device disconnect/reconnect, behaviour when a profile changes mid-hold, behaviour when two modifiers are pressed simultaneously, behaviour when the same physical control is bound to a modifier AND to a regular target in different profiles, and the contract for declaring multi-modifier layers in the JSON schema.

Without explicit semantics, every edge case becomes a silent footgun:
- A momentary SHIFT bit could remain stuck-on if the device disconnects while held, leaving every plain binding inaccessible until the next press.
- A profile switch while SHIFT is held could leave the modifier mask in an inconsistent state, with the next button press dispatching to an unintended SHIFT layer.
- An ambiguous multi-modifier mask (`SHIFT+ALT` vs `SHIFT`-only vs `ALT`-only overloads for the same MIDI key) needs a deterministic match policy or two users with the same mapping will see different behaviour.

DJs relying on SHIFT-layered controls during a live set need rock-solid modifier semantics: every press of SHIFT must do exactly what they expect, every time, with no surprises after a hot-plug, profile switch, or accidental simultaneous press of two modifiers.

## 1.2. Objective

The system must formalise modifier-layer semantics into a deterministic, documented contract usable by mapping authors, the MIDI Learn UI (PRD-0048), and every downstream feature handler. Specifically:

- The system ensures that a modifier binding in a mapping file is identified by `type: modifier` and is assigned a unique stable `bit` index in the range `[0, 31]` per device.
- The system ensures that two distinct modifier *styles* are supported: `momentary` (mask bit follows the physical button state — set on Note On, cleared on Note Off; set on CC value > 0, cleared on CC value == 0) and `latching` (mask bit toggles on every Note On / CC > 0 press; release events are ignored).
- The system ensures that on disconnection of a device, the entire `modifierMask` for that device is cleared atomically before the device's MIDI subscription is torn down, so a held-on-disconnect modifier cannot leave a stuck bit.
- The system ensures that on `activeMappingChanged` for a device, the entire `modifierMask` for that device is reset to 0 atomically before the new mapping takes effect, so a profile-switch-while-held cannot leave an inconsistent state.
- The system ensures that multi-modifier bindings are supported in the JSON schema via a `modifier` field that is either a single string id (single-modifier overload) or an array of string ids (all listed modifiers must be active simultaneously). The mapping parser combines the listed bit indices into a single required mask stored in `Binding::requiredModifierMask`.
- The system ensures that the resolver applies **strict-equality matching** between the current mask and the binding's required mask, **not subset matching**. A binding requiring `SHIFT+ALT` does not match when only `SHIFT` is held; a binding requiring no modifier (mask == 0) does not match when SHIFT is held. This avoids ambiguous double-firing.
- The system ensures that no more than four overloads sharing the same MIDI key are permitted (already enforced by PRD-0042's bucket size); the parser rejects a fifth overload with a structured `ValidationError`.
- The system ensures that the same MIDI key cannot be bound simultaneously to a modifier binding AND to a regular target binding (within the same profile); this is rejected by the parser with a `ValidationError { kind: ModifierTargetConflict, ... }`.
- The system ensures that a modifier-released event (`ModifierClear` for momentary) is dispatched to the modifier state machine *before* any other inbound event that arrives on the same MIDI callback can resolve. This is naturally satisfied by the single-threaded MIDI callback contract from PRD-0040 but is explicitly documented as an invariant.
- The system ensures that the modifier mask is observable from PRD-0048's UI so the user can see which modifiers are currently held.

## 1.3. User Flow

This PRD has no UI of its own; it formalises and hardens behaviour described informally in earlier PRDs.

### 1.3.1. Authoring a SHIFT Layer for the Reloop Profile

1. The mapping author opens `reloop-contour-interface-edition.json` and declares the SHIFT button as a modifier:

   ```json
   "modifiers": [
     { "id": "shift", "midi": { "channel": 1, "status": "note", "data1": 24 }, "style": "momentary" }
   ]
   ```

2. They then declare a binding for HOT CUE 1 plain and a SHIFT-layered overload:

   ```json
   { "target": "deck.A.hotcue.1.trigger", "midi": { "channel": 1, "status": "note", "data1": 25 } },
   { "target": "deck.A.hotcue.1.delete",  "midi": { "channel": 1, "status": "note", "data1": 25 }, "modifier": "shift" }
   ```

3. `MappingParser::parse` assigns `shift` to bit `0`, materialises the two overloads sharing MIDI key `(ch 1, note 25)` in the same hash bucket with `requiredModifierMask = 0` and `= 1` respectively.

### 1.3.2. Pressing SHIFT + HOT CUE 1: Momentary Behaviour

1. The user presses and holds SHIFT. Hardware emits Note On for note 24.
2. `MidiInboundRouter::onMidiInbound` resolves to `ResolvedBinding { category: ModifierSet, intDelta: 0 }`. The router sets bit 0 of the device's `modifierMask` via `fetch_or(1, std::memory_order_release)`.
3. The user presses HOT CUE 1. Hardware emits Note On for note 25. The router calls `BindingResolver::resolve`. The bucket for key `(ch 1, note 25)` contains two entries.
4. The resolver iterates the bucket. The plain entry (`requiredModifierMask = 0`) fails the equality check (current mask is `1`, not `0`). The SHIFT entry (`requiredModifierMask = 1`) succeeds.
5. The resolver returns the SHIFT overload. The router dispatches to `deck.A.hotcue.1.delete` via the Message thread.
6. The user releases HOT CUE 1 (Note Off, note 25, velocity 0). The momentary binding produces `normalisedValue = 0.0f`, which the handler ignores for momentary actions.
7. The user releases SHIFT. Hardware emits Note Off for note 24. The router resolves to `ResolvedBinding { category: ModifierClear, intDelta: 0 }`. The router clears bit 0 via `fetch_and(~1, std::memory_order_release)`.

### 1.3.3. Latching Modifier

1. A future mapping author declares a `LAYER 2` button with `style: latching`:

   ```json
   { "id": "layer2", "midi": { "channel": 1, "status": "note", "data1": 32 }, "style": "latching" }
   ```

2. The first press of the button produces `ModifierSet` and sets bit 1. The hardware releases the button; the corresponding Note Off is *ignored* by the router (latching: only press events toggle the bit).
3. The second press toggles bit 1 back to 0. PRD-0048's UI shows a persistent on/off indicator.

### 1.3.4. SHIFT Held Across Disconnect

1. The user holds SHIFT. Bit 0 of the Contour CE's `modifierMask` is set.
2. The USB cable accidentally disconnects. `MidiDeviceManager` detects the disconnect on the next hot-plug poll (≤ 1 s) and fires `midiDeviceRemoved(deviceId)`.
3. The `MidiInboundRouter` listens for `midiDeviceRemoved` and atomically clears `deviceStates[deviceId].modifierMask` to 0 before the device's MIDI subscription is torn down.
4. The user reconnects the cable. `midiDeviceAdded` fires; the previously cached `modifierMask` is 0; no stuck modifier.

### 1.3.5. Profile Switch While SHIFT Held

1. The user holds SHIFT, then uses the MIDI Learn UI (PRD-0048) to switch profiles.
2. `MappingStore` fires `activeMappingChanged(deviceId)`. The router listens and atomically clears `modifierMask` to 0 *before* swapping the cached `Mapping` pointer.
3. The user releases SHIFT after the switch. The Note Off for note 24 may or may not match a modifier in the new profile. If it does match a modifier in the new profile, that modifier's "release" produces a `ModifierClear` against a bit that is already 0 — a no-op. If it does not match, the event is unresolved and dropped silently.

### 1.3.6. Multi-Modifier Layer (Future Hardware)

1. A future mapping for a Native Instruments Traktor S4 declares two modifiers, `shift` (bit 0) and `alt` (bit 1).
2. A binding declares `"modifier": ["shift", "alt"]` for a four-layer button.
3. `MappingParser::parse` computes the required mask as `0b11` (bits 0 and 1 set).
4. The resolver matches the binding only when the user is holding both SHIFT and ALT simultaneously. Holding only one or the other does not match. Holding neither does not match.

### 1.3.7. Conflict: Same MIDI Key Bound to Modifier and Regular Target

1. A mapping author accidentally declares the SHIFT button as both a modifier AND a regular target:

   ```json
   "modifiers": [{ "id": "shift", "midi": { "channel": 1, "status": "note", "data1": 24 } }],
   "bindings":  [{ "target": "deck.A.transport.play", "midi": { "channel": 1, "status": "note", "data1": 24 } }]
   ```

2. `MappingParser::parse` detects the collision and produces a `ValidationError { kind: ModifierTargetConflict, midiKey: 0x011018, ... }`. Both the modifier and the regular binding are dropped from the materialised `Mapping`. The error is surfaced in PRD-0048's UI.

## 1.4. Acceptance Criteria

- [ ] The system formalises the `modifier` schema field accepting either a string (single modifier id) or an array of strings (all-must-match), and updates `MappingParser::parse` (PRD-0042) to compute `requiredModifierMask` accordingly.
- [ ] The system updates `MappingParser::parse` to assign a unique stable `bit` index in `[0, 31]` to each declared modifier in a mapping file, in declaration order.
- [ ] The system updates the JSON schema to support a `style` field on each modifier (one of `"momentary"` or `"latching"`), defaulting to `"momentary"` if omitted.
- [ ] The system extends the `Modifier` POD (defined in PRD-0042) with a `ModifierStyle` field (enum class with values `Momentary` and `Latching`). `static_assert(std::is_trivially_copyable_v<Modifier>)` remains.
- [ ] The system's `BindingResolver::resolve` (PRD-0042) for a modifier binding returns `ResolvedBinding { category: ModifierSet }` on Note On/CC > 0 and `ResolvedBinding { category: ModifierClear }` on Note Off/CC == 0 for `Momentary` modifiers, and returns `ResolvedBinding { category: ModifierToggle }` on Note On/CC > 0 only (ignoring releases) for `Latching` modifiers.
- [ ] The system extends `MidiTargetCategory` (PRD-0041) to include `ModifierToggle` in addition to `ModifierSet` and `ModifierClear`. The routing table classifies `ModifierToggle` as `MessageThread` (consistent with the other modifier categories — modifiers do not reach the audio thread).
- [ ] The system's `MidiInboundRouter` (PRD-0044) handles `ModifierToggle` by atomically XOR-ing the bit into the device's `modifierMask` via `fetch_xor(1u << bit, std::memory_order_release)`, returning without dispatching to feature handlers.
- [ ] The system enforces strict-equality matching of `currentMask == binding.requiredModifierMask` in the resolver's bucket scan. Subset matching is explicitly disallowed.
- [ ] The system's `MappingParser::parse` produces a `ValidationError { kind: ModifierTargetConflict, midiKey, sourcePath, offset }` and drops both bindings when the same MIDI key is declared as a modifier and as a regular target in the same mapping.
- [ ] The system's `MappingParser::parse` produces a `ValidationError { kind: UnknownModifier, modifierName, sourcePath, offset }` when a binding's `modifier` field references a modifier id that is not declared in the same mapping, and drops only that binding.
- [ ] The system's `MappingParser::parse` produces a `ValidationError { kind: TooManyOverloads, midiKey, ... }` when a fifth overload is declared for an already-full bucket (preserves PRD-0042's 4-overload-per-key cap).
- [ ] The system's `MidiInboundRouter` listens for `MidiDeviceManager::midiDeviceRemoved(deviceId)` and atomically clears the device's `modifierMask` (via `store(0, std::memory_order_release)`) before any subsequent dispatch.
- [ ] The system's `MidiInboundRouter` listens for `MappingStoreListener::activeMappingChanged(deviceId)` and atomically clears the device's `modifierMask` to 0 before swapping the cached `Mapping` pointer.
- [ ] The system exposes `MidiInboundRouter::getModifierMask(uint64_t deviceId) const noexcept -> uint32_t` (read with `std::memory_order_acquire`) for PRD-0048's UI.
- [ ] The system exposes `MidiInboundRouter::getModifierBitName(const Mapping&, uint8_t bit) const -> std::optional<juce::String>` returning the modifier id string for a given bit index, for UI display.
- [ ] The system's modifier mask state is per-device — distinct devices have independent masks and a SHIFT held on the Reloop Contour CE does not affect modifier resolution for a different connected controller.
- [ ] The system supports up to 32 distinct modifiers per mapping (encoded in `uint32_t`). The parser rejects a 33rd modifier in the same mapping with a `ValidationError { kind: TooManyModifiers, ... }`.
- [ ] The system's bundled Reloop profile (PRD-0043) declares `shift` as a `momentary` modifier with the correct MIDI mapping derived from the reference `.tsi`.
- [ ] The system's bundled Reloop profile declares SHIFT-layered overloads for at minimum: `hotcue.{1..4}.trigger` → `hotcue.{1..4}.delete`, `loop.size.halve/double` → `beatjump.minus1/plus1`.
- [ ] The system is covered by `ModifierLayerTests.cpp` in `Tests/` verifying: (a) momentary press/release toggles the bit; (b) latching press toggles bit, release does not; (c) strict-equality matching rejects subset matches; (d) multi-modifier binding requires all listed modifiers simultaneously; (e) device disconnect clears the mask; (f) profile switch clears the mask before activation; (g) `ModifierTargetConflict` and `UnknownModifier` validation errors fire and drop the offending bindings; (h) `getModifierMask` returns the current state via acquire ordering; (i) the bundled Reloop profile loads and exercises SHIFT layers end-to-end.
