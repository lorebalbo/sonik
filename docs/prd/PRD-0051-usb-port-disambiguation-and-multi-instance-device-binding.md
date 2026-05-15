---
status: Not Implemented
epic: EPIC-0006
depends-on: [PRD-0040, PRD-0042, PRD-0043, PRD-0048]
---

# 1. PRD-0051: USB-Port Disambiguation and Multi-Instance Device Binding

## 1.1. Problem

PRD-0040 computes `deviceId = SHA-1(manufacturer | product | ordinal)` and uses `ordinal = 0` only. This works perfectly for one Behringer DDM4000 on one USB port. It breaks the moment a serious DJ does what serious DJs do: plug **two identical Behringer DDM4000 mixers** side-by-side, one for the left half of a four-deck show, one for the right.

Concretely, with the v1 resolver:

- Both physical devices receive identical `deviceId` values (same `manufacturer | product | 0`).
- The Behringer DDM4000 bundled profile resolves to the same `deviceId` for both, so both devices receive the same MIDI bindings.
- Pressing the Channel A CUE button on the left DDM4000 and the Channel A CUE button on the right DDM4000 both dispatch to `mixer.channel.A.cue` — there is no way to distinguish them.
- LED feedback from PRD-0047 sends outbound MIDI to the *first* device returned by `MidiDeviceManager::getMidiOutputForDevice(deviceId)` — the second device's LEDs go dark.
- MIDI Learn (PRD-0048) cannot bind a control on the right DDM4000 to `deck.C.transport.play` because the underlying deviceId is identical to the left one.

The v1 mapping schema reserves an `ordinal` field in `device.match` so this would be a non-breaking schema extension — but the runtime doesn't populate ordinal from anything, and there is no UI to tell Sonik "this physical port is the right deck's controller, the other is the left deck's." Without that, multi-instance setups are dead.

The professional answer is to fingerprint each physical USB port using OS-provided endpoint identifiers (`juce::MidiDeviceInfo::identifier` — stable across reboots on both macOS Core MIDI and Windows WinRT MIDI for the same physical USB connection) and let the user explicitly bind a profile to a physical port via the Settings UI. The user plugs both Behringer DDM4000 mixers in, opens Settings, sees both devices listed with distinct port identifiers, and clicks "This is Deck A/B" on one and "This is Deck C/D" on the other.

This is also a **non-breaking** extension: existing v1 profiles without an `identifierHint` continue to match any port (preserving today's behaviour); profiles with an `identifierHint` get port-specific binding.

## 1.2. Objective

The system must extend the v1 device-ID resolver to support per-physical-USB-port disambiguation, populate the v1-reserved `ordinal` and a new `identifierHint` field from OS-provided endpoint identifiers, and surface a "Bind to This Port" UI in the Settings panel. Specifically:

- The system ensures that `MidiDeviceManager::computeDeviceId(const juce::MidiDeviceInfo&) -> uint64_t` is replaced by `computeDeviceId(const juce::MidiDeviceInfo&, const std::vector<juce::MidiDeviceInfo>& siblings) -> uint64_t` where `siblings` is the full list of currently-connected MIDI input infos.
- The system ensures that the new `computeDeviceId` derives a stable per-physical-port fingerprint:
  1. If `info.identifier` is non-empty and unique among all currently-connected devices, use `SHA-1(manufacturer | product | identifier)` truncated to 64 bits.
  2. Otherwise, fall back to v1 `SHA-1(manufacturer | product | ordinal)` where `ordinal` is the 0-based index of this device among all siblings sharing the same `(manufacturer, product)`, ordered by `info.identifier` lexically.
- The system ensures that the `deviceId` for any given physical USB port is **stable across reboots** when the OS-reported `identifier` is stable, which it is on macOS Core MIDI and Windows WinRT MIDI for the same physical port.
- The system ensures that the v1 mapping schema's `device.match` block is extended with an optional `identifierHint: string | { regex: string }` field, matched against `info.identifier`.
- The system ensures that profile resolution priority becomes: (a) user profile with matching `device.match` *including* `identifierHint` (exact port match) > (b) user profile with matching `device.match` without `identifierHint` (any port) > (c) bundled profile with matching `device.match` *including* `identifierHint` > (d) bundled profile with matching `device.match` without `identifierHint` > (e) `generic-midi` fallback.
- The system ensures that the v1 priority order from PRD-0043 (user > bundled-device > generic) is preserved as a strict superset; the new `identifierHint`-matching tiers are *inserted* without reordering the existing fallback chain.
- The system ensures that PRD-0048's `MidiSettingsPanel` is extended with a per-device "Bind to This Specific USB Port" toggle. When enabled for a profile, the system writes/updates the user profile's `device.match.identifierHint` to the connected device's current `info.identifier`. When disabled, the field is removed.
- The system ensures that the Settings panel displays the current `info.identifier` (or a human-readable shortened form, e.g., last 12 chars + ellipsis) per connected device, so the user can visually distinguish two identical controllers.
- The system ensures that the Settings panel displays a "Swap" action when two identical devices are connected, which swaps the active profiles between the two physical ports atomically.
- The system ensures that bundled profiles are **never** automatically rewritten to add `identifierHint`. Binding bundled profiles to specific ports requires first duplicating the bundled profile to a user copy (PRD-0048 behaviour).
- The system ensures that hot-plug events (PRD-0040) correctly produce per-port distinct `deviceId` values: unplugging the left DDM4000 and plugging it back in to the same physical USB port yields the same `deviceId`; plugging into a different port yields a different `deviceId`.
- The system ensures that the active-mapping selection per `deviceId` is persisted in a small JSON state file `~/Library/Application Support/Sonik/MidiMappings/_device_state.json` mapping `deviceId -> activeMappingId`, so that two Behringer DDM4000 mixers in two specific ports each remember their last selected profile across launches.
- The system ensures that when the OS-reported `identifier` is empty or known-unstable on a platform (a runtime detection, not a compile-time switch), the system silently falls back to the v1 ordinal-based scheme and logs a one-time `DBG` warning on the Message thread.

## 1.3. User Flow

### 1.3.1. Two Behringer DDM4000 mixers Plugged In For the First Time

1. The user connects two Behringer DDM4000 mixers side-by-side to two USB ports on their MacBook. `MidiDeviceManager::pollHotPlug` detects both within 1 second.
2. For the first device, `computeDeviceId` reads `info.identifier = "Bluetooth Bus :: USB :: 0x1234567a"` (macOS-style) and produces `deviceId = D1`.
3. For the second device, `computeDeviceId` reads `info.identifier = "Bluetooth Bus :: USB :: 0x1234567b"` and produces `deviceId = D2`.
4. `MappingStore` resolves a profile for each:
   - D1 has no user override → resolves to the bundled `behringer-ddm4000.json` (no `identifierHint`, matches both).
   - D2 has no user override → also resolves to the bundled profile.
5. Both controllers now drive deck A/B identically. The user sees this in the Settings panel: two device entries listing identical profile names.
6. The user opens Settings → MIDI. The panel lists:
   - **Behringer DDM4000** — port: `…0x1234567a` — profile: *Behringer DDM4000 v1 (bundled)*
   - **Behringer DDM4000** — port: `…0x1234567b` — profile: *Behringer DDM4000 v1 (bundled)*
7. They click **Duplicate to User Profile** on the first device, name the copy "DDM4000 Left (A/B)". The profile is duplicated; the panel now shows this user profile as active for D1.
8. They click the new "Bind to This Specific USB Port" toggle. `MappingStore` updates the user profile's JSON to add `device.match.identifierHint: "Bluetooth Bus :: USB :: 0x1234567a"` and saves atomically.
9. They repeat: duplicate again for the second device, naming "DDM4000 Right (C/D)", and bind to the second port.
10. Now D1 resolves to "DDM4000 Left (A/B)" and D2 resolves to "DDM4000 Right (C/D)" — even though both profiles are derived from the same bundled template.
11. The user opens the "DDM4000 Right (C/D)" profile in the binding table and remaps every `deck.A.*` target to `deck.C.*` (and `deck.B.*` → `deck.D.*`) via the existing PRD-0048 inline editor.

### 1.3.2. Reboot Restores Per-Port Bindings

1. The user reboots their machine. Both Behringer DDM4000 mixers power up.
2. `MidiDeviceManager` enumerates and produces the same `deviceId` values D1 and D2 (because macOS's Core MIDI identifier is stable for the same physical port).
3. `MappingStore` reads `_device_state.json`, finds the persisted active-mapping selections, and resolves D1 → "DDM4000 Left (A/B)" and D2 → "DDM4000 Right (C/D)". The bundled-profile fallback never triggers because both user profiles have explicit `identifierHint` matches.
4. PRD-0047's `MidiFeedbackEngine` performs a boot dump for each device, lighting them correctly.

### 1.3.3. User Plugs One DDM4000 Into a Different Port

1. The user accidentally swaps the USB ports. The DDM4000 that was previously in port-A is now in port-B.
2. `MidiDeviceManager` enumerates: the controller previously known as D1 is now reporting `info.identifier = "…0x1234567b"` (port-B's identifier), so it gets `deviceId = D2`. The other gets D1.
3. The user-profile resolver: the "DDM4000 Left (A/B)" profile has `identifierHint: "…0x1234567a"` (port-A's identifier). The device now in port-A is the physical right-deck controller. The "DDM4000 Right (C/D)" profile has `identifierHint: "…0x1234567b"`, which now matches the physical left-deck controller in port-B.
4. Result: the user's physical-controller-to-software-deck mapping is swapped. The DDM4000 labelled "left" now controls C/D and vice versa. This is **correct behaviour**: the user told the software "this profile binds to this physical port"; if they move the controller to a different port, the binding follows the port, not the controller.
5. The user notices the mismatch. They open Settings → MIDI, see the device list, and click the new **Swap** button. The two profiles' `identifierHint` fields are swapped atomically.
6. The user could alternatively unplug and re-plug the controllers in the "correct" physical ports.

### 1.3.4. Single Controller Today (No Behaviour Change)

1. The user has one Behringer DDM4000 on one USB port (the EPIC-0005 baseline scenario).
2. `MidiDeviceManager::pollHotPlug` produces `deviceId = D1`, derived from the OS identifier.
3. `MappingStore` resolves to the bundled `behringer-ddm4000.json` (no `identifierHint` → matches any port).
4. The user never opens the new "Bind to This Specific USB Port" toggle. Their experience is identical to EPIC-0005.

### 1.3.5. Platform With Unstable Identifier

1. A future build runs on a hypothetical platform where `juce::MidiDeviceInfo::identifier` is empty or rotates per session.
2. `MidiDeviceManager` detects on the first enumeration that all sibling identifiers are empty/duplicated.
3. The system silently falls back to v1's `(manufacturer, product, ordinal)` scheme. A one-time `DBG` warning is emitted on the Message thread.
4. The "Bind to This Specific USB Port" toggle is disabled in the Settings panel with a tooltip: "Your operating system does not provide stable USB-port identifiers. Multi-instance disambiguation is unavailable on this platform."

## 1.4. Acceptance Criteria

- [ ] The system replaces `MidiDeviceManager::computeDeviceId(const juce::MidiDeviceInfo&)` (PRD-0040) with `computeDeviceId(const juce::MidiDeviceInfo&, const std::vector<juce::MidiDeviceInfo>& siblings)`.
- [ ] The system implements `computeDeviceId` to: (1) collect every `siblings` entry's `identifier`; (2) if the input's `identifier` is non-empty AND all sibling identifiers are non-empty AND no two siblings share the same identifier, compute `SHA-1(manufacturer | product | identifier)` truncated to 64 bits; (3) otherwise compute `SHA-1(manufacturer | product | ordinal)` where `ordinal` is the 0-based index of the input among siblings sharing the same `(manufacturer, product)`, sorted by `identifier` lexically then by `name`.
- [ ] The system emits a one-time `DBG` warning on the Message thread (guarded by a `std::atomic_flag` to fire at most once per process) when the identifier-based path is unavailable.
- [ ] The system extends the v1 mapping schema's `device.match` block with an optional `identifierHint` field accepting either a string (exact match against `info.identifier`) or `{ regex: string }` (regex match). `MappingParser::parse` (PRD-0042) is updated to parse this field into a new `Mapping::DeviceMatch::identifierHintRegex: std::optional<std::regex>` (compiled lazily on first match attempt and cached).
- [ ] The system extends the profile-resolution algorithm in `MappingStore::resolveProfileForDevice(deviceId, info) -> std::optional<MappingId>` (PRD-0043) to score every candidate user and bundled profile by: (a) `identifierHint` present and matches `info.identifier` → score 4 (user) or 2 (bundled); (b) no `identifierHint`, `device.match` other fields match → score 3 (user) or 1 (bundled); (c) `identifierHint` present and does NOT match → score 0 (rejected). Highest score wins; ties resolved by user > bundled > generic order.
- [ ] The system extends PRD-0048's `MidiSettingsPanel` with a per-device row showing the physical-port identifier in a monospace label (last 16 characters, ellipsised left if longer).
- [ ] The system adds a per-device "Bind to This Specific USB Port" toggle in the panel. Enabling for a user profile writes `device.match.identifierHint = info.identifier` to the JSON via `MappingStore::setIdentifierHint(mappingId, identifierHint)` (a new method added in this PRD), debounced and persisted atomically. Disabling removes the field.
- [ ] The system disables the toggle for bundled profiles with a tooltip directing the user to "Duplicate to User Profile" first.
- [ ] The system disables the toggle on platforms where the identifier-based path is unavailable, with the platform-tooltip described above.
- [ ] The system adds a top-level "Swap Profiles Between Ports" action visible when exactly two devices sharing `(manufacturer, product)` are connected. The action calls `MappingStore::swapIdentifierHints(deviceIdA, deviceIdB)` (new method) which atomically swaps the `identifierHint` fields between the two devices' currently active user profiles.
- [ ] The system disables the Swap action when one of the two profiles is bundled.
- [ ] The system persists active-mapping selections per `deviceId` to `~/Library/Application Support/Sonik/MidiMappings/_device_state.json` via `MappingStore::saveDeviceState() const`, called debounced after every `setActiveMapping`.
- [ ] The system loads `_device_state.json` on `MappingStore` construction and applies the persisted selections when devices are detected via `midiDeviceAdded`. Missing entries fall back to normal resolution.
- [ ] The system on `_device_state.json` parse failure logs the error via `DBG` on the Message thread, falls back to normal resolution, and never crashes.
- [ ] The system uses atomic `.tmp` + rename writes for `_device_state.json` (consistent with PRD-0043).
- [ ] The system preserves PRD-0040's existing `MidiDeviceManager` API surface for everything other than `computeDeviceId`'s signature change; consumers (`MidiInboundRouter`, `MidiFeedbackEngine`, `MidiSettingsPanel`) are updated to the new signature.
- [ ] The system asserts via `JUCE_ASSERT_MESSAGE_THREAD` on every new public method.
- [ ] The system is covered by `UsbPortDisambiguationTests.cpp` in `Tests/` verifying: (a) two synthetic `MidiDeviceInfo` siblings with distinct identifiers produce distinct `deviceId` values; (b) two siblings with empty identifiers fall back to ordinal-based ids that still differ from each other; (c) re-enumerating the same physical-port identifier reproduces the same `deviceId` byte-for-byte; (d) `identifierHint` exact-match resolution scores higher than no-hint resolution; (e) `identifierHint` regex matching works for partial patterns; (f) `_device_state.json` round-trip (save + reload) preserves selections; (g) the `Swap Profiles Between Ports` action correctly swaps `identifierHint` between two user profiles; (h) Swap is rejected when one profile is bundled; (i) the `Bind to This Specific USB Port` toggle is disabled in the panel for bundled profiles and on unsupported platforms (simulated via a runtime stub); (j) v1 profiles without `identifierHint` continue to match any port unmodified (full backward-compat verification).
