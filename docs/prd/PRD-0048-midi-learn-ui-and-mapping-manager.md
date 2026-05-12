---
status: Not Implemented
epic: EPIC-0005
depends-on: [PRD-0040, PRD-0042, PRD-0043, PRD-0044, PRD-0045, PRD-0046, PRD-0047]
---

# 1. PRD-0048: MIDI Learn UI and Mapping Manager

## 1.1. Problem

PRDs 0040–0047 implement the full MIDI controller backend: device detection, real-time-safe inbound dispatch, a versioned mapping JSON schema, soft-takeover, modifier layers, and LED feedback. But everything is **headless**. The user has no way to:

- See which MIDI devices Sonik has detected.
- See which mapping profile is currently active per device.
- Switch between bundled and user mapping profiles per device.
- Inspect and edit individual bindings (which physical control is mapped to which app target).
- Add a new binding via "MIDI Learn" — click a target in the UI, then wiggle the desired physical control on the hardware.
- See validation errors from `MappingStore::getLoadErrors()` when a custom mapping fails to parse.
- See the current soft-takeover state (which continuous controls are `Disengaged`) and force-engage any of them.
- See the current modifier mask (which SHIFTs / ALTs are held).
- Duplicate a bundled profile to a user profile to start customising.
- Reset to bundled defaults if their custom mapping breaks.

Without this UI, the entire MIDI subsystem is invisible to end users. The Reloop Contour CE works out of the box (because the profile is bundled), but every other controller in the world requires the user to hand-edit a JSON file in `~/Library/Application Support/Sonik/MidiMappings/`. That is not acceptable for a consumer product comparable to Traktor or Serato.

## 1.2. Objective

The system must implement a "MIDI Settings" panel inside the main application's existing settings/preferences area, exposing every MIDI subsystem capability to the user through a graphical interface conforming to `DESIGN.md`. Specifically:

- The system ensures that a "MIDI" tab in Settings displays every detected device with its current connection state, manufacturer, product, deviceId, and active profile name.
- The system ensures that the user can switch the active profile per device from a dropdown listing every available profile for the device (bundled and user-saved).
- The system ensures that the user can duplicate a bundled profile to a user profile via a "Duplicate to User Profile" action, after which all subsequent edits target the user copy (bundled profiles are read-only).
- The system ensures that the user can delete a user profile via a "Delete" action with confirmation.
- The system ensures that the user can reset a device's active profile to the bundled default via a "Reset to Defaults" action with confirmation.
- The system ensures that the panel displays the full binding table for the active profile, with one row per binding showing: target (e.g., `deck.A.transport.play`), physical MIDI key (e.g., `Ch 1 Note 18`), modifier requirement (e.g., `(none)` or `SHIFT`), transform (e.g., `Momentary`), soft-takeover policy (e.g., `Pickup`), and feedback summary (e.g., `Binary LED`).
- The system ensures that the user can edit any field of a user-profile binding via inline controls (dropdowns for target/transform/policy, free-form numeric inputs for MIDI key, modifier picker).
- The system ensures that the user can initiate "MIDI Learn" for any binding row via a button: the row enters a "Learning…" state for up to 10 seconds; the next inbound MIDI event from the device replaces the binding's MIDI key.
- The system ensures that the user can add a new binding via an "Add Binding" button, which inserts a blank row and immediately enters MIDI Learn mode for the new row.
- The system ensures that the user can delete a binding via a per-row "Delete" button with confirmation.
- The system ensures that MIDI Learn detects conflicts with existing bindings (same MIDI key + same modifier mask) and prompts the user to either replace the existing binding, change the new binding's modifier, or cancel.
- The system ensures that validation errors from `MappingStore::getLoadErrors()` are surfaced in a banner at the top of the panel with line/offset details for each error, with a "Reload" action to retry.
- The system ensures that the panel displays a real-time visualiser per device showing the current modifier mask (PRD-0046 `getModifierMask`) as a list of held modifier names, updated at ≤ 30 Hz.
- The system ensures that the panel displays a real-time list of `Disengaged` soft-takeover bindings (PRD-0045 `getState`) per device, with a "Force Engage" button per row.
- The system ensures that every edit to a user profile is debounced and persisted atomically via `MappingStore::save` (PRD-0043) after 500 ms of inactivity.
- The system ensures that the panel performs all operations on the Message thread and conforms strictly to `DESIGN.md` (monochrome `#2d2d2d` / `#fdfdfd`, Space Mono, 2px `#2d2d2d` borders, no gradients, no border-radius, dithered patterns, pixel-art icons).

## 1.3. User Flow

### 1.3.1. Opening the MIDI Settings Panel With a Connected Reloop Contour CE

1. The user opens Settings → MIDI.
2. The panel lists one device: "Reloop Contour Interface Edition (Connected, Profile: Reloop Contour Interface Edition v1)". The active-profile dropdown has two options: "Reloop Contour Interface Edition v1 (bundled, read-only)" and "Generic MIDI (bundled, read-only)".
3. Below the device header, a table lists every binding: 124 rows for the bundled Reloop profile. The user scrolls to inspect the SHIFT-layered hot-cue delete bindings.
4. Every row is read-only (greyed) because the active profile is bundled.
5. A banner at the bottom shows: "Modifiers held: (none)". A second list shows "Disengaged: deck.A.pitchFader, deck.B.pitchFader" (the user hasn't touched the pitch faders yet since startup).
6. The user holds SHIFT; the "Modifiers held" banner updates to "Modifiers held: shift" within ≤ 50 ms.

### 1.3.2. Duplicating to a User Profile and Editing

1. The user clicks "Duplicate to User Profile". A dialog prompts for the new profile name; the user types "Reloop CE (my mix)" and confirms.
2. `MappingStore` writes `~/Library/Application Support/Sonik/MidiMappings/reloop-ce-my-mix.json` (atomic via `.tmp` + rename) and emits `mappingAdded` + `activeMappingChanged`.
3. The panel switches the active profile dropdown to the new user profile. Every binding row is now editable.
4. The user clicks "MIDI Learn" on the PLAY button binding for Deck A. The row enters the "Learning…" state with a countdown.
5. The user presses a different physical button on the Contour CE (Note On, ch 1, note 50). The row's MIDI key updates to "Ch 1 Note 50". The Learning state dismisses.
6. After 500 ms of inactivity, the user profile is saved atomically.

### 1.3.3. Conflict Detection During MIDI Learn

1. The user clicks "MIDI Learn" on the CUE button binding for Deck A. The user accidentally presses the same physical button they just remapped to PLAY.
2. The panel detects the conflict: another binding in the active profile already uses `(Ch 1, Note 50, modifier mask 0)`.
3. A modal dialog appears: "This MIDI key is already bound to `deck.A.transport.play`. Replace, change modifier, or cancel?"
4. The user picks "Change modifier" → "SHIFT". The new binding is created with `requiredModifierMask = bit-for-shift`. No conflict because the PLAY binding has no modifier requirement.
5. The user profile is saved.

### 1.3.4. Force-Engaging a Soft-Takeover Binding

1. The user has just loaded a track. The pitch fader is `Disengaged`. The settings panel shows it in the "Disengaged" list.
2. The user clicks "Force Engage" next to `deck.A.pitchFader`.
3. The panel reads the current hardware value (last cached by PRD-0045) and the current software value, calls `SoftTakeoverManager::forceEngage(deviceId, target, hwValue, swValue)`.
4. The pitch fader's binding transitions to `Engaged`. The Disengaged list updates. Subsequent fader moves pass through immediately.

### 1.3.5. Surfacing a Validation Error

1. The user manually edits a user profile JSON outside the application and accidentally introduces a `ModifierTargetConflict`.
2. On next launch, `MappingStore` parses the file, surfaces a `ValidationError` via `getLoadErrors()`, and falls back to the bundled profile.
3. The panel displays a red banner: "Failed to load `reloop-ce-my-mix.json`: line 47, offset 12 — modifier 'shift' and binding 'deck.A.transport.play' both bound to Ch 1 Note 24."
4. The banner offers "Open in Editor" (reveals in Finder) and "Reset to Defaults" (overwrites the broken file with a fresh copy from the bundled profile).

### 1.3.6. Adding a Custom Generic-MIDI Mapping

1. The user connects a future Korg nanoKONTROL2. `MidiDeviceManager` detects it; `MappingStore` resolves to `Generic MIDI` (no bundled Korg profile yet).
2. The panel lists the new device. The user duplicates Generic MIDI to "Korg nanoKONTROL2 (mine)" and clicks "Add Binding".
3. The new row enters Learning mode. The user wiggles the first fader; the panel captures CC 0 on channel 1 and proposes the target `deck.A.pitchFader`.
4. The user accepts. The binding is created.
5. The user repeats for the remaining controls.

## 1.4. Acceptance Criteria

- [ ] The system adds a new "MIDI" tab to the existing Settings panel (or creates the Settings panel if it does not yet exist) using `juce::TabbedComponent`-style navigation consistent with `DESIGN.md`.
- [ ] The system's MIDI panel root component lives under `Source/Features/Midi/UI/MidiSettingsPanel.{h,cpp}`, conforming to the Atomic Design layering — atoms (`DevicePill`, `BindingRow`, `LearnButton`) → molecules (`DeviceHeader`, `BindingTable`) → organism (`MidiSettingsPanel`).
- [ ] The system uses the existing strict monochrome palette (`#2d2d2d` and `#fdfdfd`), Space Mono Regular font, 2px solid `#2d2d2d` borders, zero `border-radius`, dithered patterns instead of gradients, and pixel-art icons throughout.
- [ ] The system subscribes the panel to `MidiDeviceManager` connect/disconnect events and updates the device list on the Message thread.
- [ ] The system subscribes the panel to `MappingStoreListener::activeMappingChanged`, `mappingAdded`, `mappingRemoved` events and refreshes the binding table accordingly.
- [ ] The system subscribes the panel to `SoftTakeoverManagerListener::stateChanged` events and updates the "Disengaged" list.
- [ ] The system polls `MidiInboundRouter::getModifierMask(deviceId)` at 30 Hz via a `juce::Timer` and updates the "Modifiers held" banner.
- [ ] The system displays one device row per connected device with: device name (manufacturer + product), connection status pill (Connected/Disconnected), deviceId in monospace, active-profile dropdown, "Duplicate to User Profile" button, "Reset to Defaults" button.
- [ ] The system disables editing controls for read-only bundled profiles, indicating the read-only state via an explicit "(bundled, read-only)" suffix on the profile name.
- [ ] The system displays the binding table with columns: Target, MIDI Key, Modifier, Transform, Soft-Takeover, Feedback, Actions.
- [ ] The system exposes per-row Edit, MIDI Learn, and Delete actions for user-profile bindings.
- [ ] The system implements MIDI Learn as a 10-second countdown during which the next inbound `MidiInboundEvent` for the bound device is captured. The capture is implemented by registering the panel as a transient `MidiInputSubscriber` (PRD-0040) for the targeted device; the subscription is torn down on capture or timeout.
- [ ] The system detects conflicts during MIDI Learn by scanning the active mapping's bindings for any binding sharing the same `(channel, status, data1, requiredModifierMask)` triple, and prompts the user via a modal dialog with options Replace / Change Modifier / Cancel.
- [ ] The system implements "Add Binding" by appending a row with default-empty fields and immediately entering MIDI Learn mode, then prompting the user to pick a target via a searchable target picker.
- [ ] The system implements the target picker as a tree-structured combo box scoped to the registered targets in `ControlTargetRegistry` (PRD-0042), grouped by feature (deck.A, deck.B, mixer, library, …) and supporting full-text search by target name.
- [ ] The system implements debounced persistence: every edit to a user profile schedules a `juce::Timer::startTimer(500)` that, on fire, calls `MappingStore::save(deviceId, mappingId)` on a background thread.
- [ ] The system reads `MappingStore::getLoadErrors()` (PRD-0043) on panel mount and on `mappingLoadFailed` events; displays each error as a banner row with `path : line : offset` and a structured human-readable description.
- [ ] The system offers "Open in Editor" (using `juce::File::revealToUser`) and "Reset to Defaults" (overwrite the user file with the bundled default for the same device, then reload) in the error banner.
- [ ] The system displays the per-device "Disengaged" soft-takeover list with one row per disengaged binding: target, last hardware value, current software value, "Force Engage" button.
- [ ] The system's "Force Engage" button calls `SoftTakeoverManager::forceEngage(deviceId, target, lastHardwareValue, currentSoftwareValue)` on the Message thread.
- [ ] The system displays the per-device modifier banner as a comma-separated list of currently-held modifier ids resolved via `MidiInboundRouter::getModifierBitName(mapping, bit)` (PRD-0046).
- [ ] The system's "Duplicate to User Profile" prompts for a non-empty unique name, sanitises it for filesystem-safe characters, and writes a deep copy of the bundled mapping JSON under `~/Library/Application Support/Sonik/MidiMappings/` via `MappingStore::createUserCopy(deviceId, bundledMappingId, newName)`.
- [ ] The system's "Delete" action on a user profile triggers a confirmation dialog before calling `MappingStore::deleteUserMapping(mappingId)`.
- [ ] The system's "Reset to Defaults" action on a device triggers a confirmation dialog before calling `MappingStore::setActiveMapping(deviceId, bundledMappingId)` and removing any user override.
- [ ] The system performs every panel operation on the Message thread; no audio-thread interaction.
- [ ] The system implements a UI smoke test in `MidiSettingsPanelTests.cpp` (`Tests/`) verifying: (a) connected devices appear in the list; (b) profile dropdown lists bundled + user profiles; (c) duplicate creates a writable user copy; (d) MIDI Learn capture replaces the row's MIDI key when a transient subscriber receives an inbound event; (e) conflict detection identifies an existing binding sharing the same MIDI key + modifier mask; (f) debounced save fires `MappingStore::save` 500 ms after the last edit; (g) load errors render in the banner; (h) Force Engage calls `SoftTakeoverManager::forceEngage`; (i) the modifier banner updates within 50 ms of a modifier bit change.
