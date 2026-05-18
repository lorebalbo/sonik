---
name: "EPIC-0006: MIDI Controller System v2 — Mapping Tooling & Multi-Device"
status: Open
---

# 1. EPIC-0006: MIDI Controller System v2 — Mapping Tooling & Multi-Device

## 1.1. Goal and Vision

Build on the v1 MIDI foundation laid by [EPIC-0005](EPIC-0005-midi-controller-system.md) to make Sonik's MIDI subsystem **portable** (mappings move easily between machines and users), **forward-compatible** (the versioned mapping schema can evolve without breaking existing files), and **truly multi-device** (two physical instances of the same controller — e.g., two Behringer DDM4000 mixers side-by-side — are addressed independently and unambiguously).

EPIC-0005 deliberately stopped at "one of each device, no migration UI, files on disk in a known folder." That ships a working DJ rig with the Behringer DDM4000 out of the box. EPIC-0006 is the second-tour-of-duty Epic that turns the MIDI subsystem from "works for one user with one controller" into "works for the community with any controller in any USB port."

The guiding principle is that this Epic **does not change the v1 mapping schema's wire format** for `schemaVersion: 1`. It adds (a) a UI shell on top of `MappingStore` for importing and exporting profile files, (b) an extensible migration framework that can move a `schemaVersion: 1` file forward to a future `schemaVersion: 2` without user intervention or data loss, and (c) a UI for picking *which* of several identical hardware instances a profile binds to. No v1 file ever needs to be rewritten unless the user explicitly chooses to.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

- **Mapping import/export UI** — file picker exposing native OS dialogs (`juce::FileChooser`), a preview pane showing the to-be-imported profile's device match, binding count, modifier count, and `schemaVersion`, validation against the registered control-target catalogue before commit, conflict resolution when the imported profile's `device.match` collides with an existing user profile (rename / replace / cancel)
- **Export of any user or bundled profile** to a single self-contained `.sonikmidi.json` file (the bundled file plus a manifest header carrying app version, export timestamp, and a SHA-256 integrity hash so corrupted files are caught on import)
- **Schema migration framework** — a versioned chain of pure migration functions `migrate_v1_to_v2(json) -> json`, `migrate_v2_to_v3(json) -> json`, etc., invoked on load when an imported or stored file's `schemaVersion` is older than the current. Migrated files are not silently rewritten on disk; the user is prompted to "Save Migrated Copy" or "Keep Original."
- **USB-port disambiguation** for multiple identical devices — extends the v1 device-ID resolver (PRD-0040) to populate the `ordinal` field from the connection order; surfaces a "Bind Profile To This Specific Port" toggle in the Settings UI per device; persists per-port profile-active selections so reconnecting two Behringer DDM4000 mixers into different ports produces a stable mapping each time
- **Per-port deviceId fingerprinting** that survives reboots: the resolver caches `(manufacturer, product, USB location ID on macOS / endpoint path on Windows)` and recognises the same physical USB port across sessions, so port-A and port-B can each hold a distinct profile selection independently of plug order
- **Conflict UX** when two identical devices boot simultaneously and the user has only ever configured one profile: prompt to either duplicate the profile (one per port) or share the same profile across both (with a clear warning that LED feedback may be ambiguous)
- **Backwards compatibility guarantee** — every `schemaVersion: 1` file produced by EPIC-0005 loads unmodified under EPIC-0006; the v2 UI never auto-rewrites v1 files on disk
- **MIDI Learn cross-device** — when two identical devices are connected, MIDI Learn's transient subscriber identifies which physical device produced the captured event (via the v1 deviceId), so a binding learned on port-A does not accidentally bind to port-B

### 1.2.2. Out of Scope

- **HID controllers** (Native Instruments Traktor S-series, Pioneer DDJ HID-mode, NI Maschine pad-only mode). These require fundamentally different OS-level APIs (IOHIDManager on macOS, HID.dll on Windows) and a separate end-to-end stack (no MIDI parser, no JUCE `MidiInput`); they belong in a dedicated future EPIC-0007 (HID Controller System).
- **Multi-port logical devices** (controllers that expose 2+ MIDI ports as one logical controller — e.g., NI Maschine, Traktor X1 in some firmware revisions). Solving this requires a logical-device-aggregation layer above the v1 device-ID resolver and a more elaborate mapping schema (per-port binding sub-sections). Deferred to a future v3 Epic if the user buys such a device.
- **Mapping marketplace / cloud sync** of community profiles. The export format defined here is the *enabler* for community sharing (drop the `.sonikmidi.json` into a forum post), but a built-in browse-and-install marketplace is its own product surface and is out of scope.
- **Vendor mapping import** (Rekordbox `.xml`, Serato `.midimap`, Traktor `.tsi`). Each vendor's format would be its own reverse-engineering effort. The user is not asking for this and the v1 Generic-MIDI fallback + MIDI Learn is the migration path.
- **Profile signing / cryptographic verification.** The SHA-256 hash in the export manifest is for *integrity* (detect bit-rot in transit), not *authenticity*. A signing PKI is out of scope.
- **Per-binding profile fragments / overlay mappings.** A v1 profile is a single self-contained file; this Epic does not introduce composition where multiple files are stacked.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Backward Compatibility With v1

Every artefact produced by EPIC-0005 must continue to work unmodified after EPIC-0006 ships:

- `schemaVersion: 1` files in `~/Library/Application Support/Sonik/MidiMappings/` load and run as before.
- Bundled profiles (`behringer-ddm4000.json`, `generic-midi.json`) remain `schemaVersion: 1` until a v2 schema is introduced.
- The v1 `MappingStore` API (`getActiveMapping`, `save`, `createUserCopy`, `deleteUserMapping`, listener events) gains *additions* but no breaking changes.
- The v1 `MidiSettingsPanel` (PRD-0048) gains a new toolbar with "Import…" / "Export…" / "Bind to Port" controls; the existing rows and editing UI remain.

### 1.3.2. Schema Migration Framework

The migration framework is a registry of pure functions keyed by `(fromVersion, toVersion)`. On load of a file with `schemaVersion: N` where `N < currentVersion`, the framework chains migrations `N → N+1 → … → currentVersion`. Each migration is a pure transformation over `juce::var` (the parsed JSON tree) and is unit-tested in isolation.

The framework requires that future schema bumps:

- Add fields with safe defaults so v1 readers degrade gracefully (or are explicitly version-gated)
- Document the migration in a header alongside the migration function (`Source/Features/Midi/Migrations/v1_to_v2.md`)
- Never silently rewrite the user's on-disk file; a migrated profile lives in memory until the user chooses "Save Migrated Copy"

For v1-only operation (today), the framework is registered but has zero migrations. The plumbing exists so the *next* schema bump is a single-PR addition.

### 1.3.3. USB Port Identity

JUCE exposes `juce::MidiDeviceInfo` with `name` and `identifier`. The `identifier` is platform-specific:

- **macOS:** Core MIDI returns a unique-per-endpoint string that survives across reboots for the same `(USB location ID, vendor ID, product ID)` triple. Two identical devices in different USB ports produce different identifiers.
- **Windows:** WinMM / WinRT MIDI returns a path-like identifier that encodes the USB device interface path; identical devices in different ports also produce different identifiers, but the encoding is OS-version-dependent.

The v1 resolver (PRD-0040) computes `deviceId = SHA-1(manufacturer | product | ordinal)` and uses `ordinal=0` only. EPIC-0006 extends this to `deviceId = SHA-1(manufacturer | product | identifier)`, falling back to the v1 scheme when `identifier` is empty or unstable on the platform. The mapping schema's `device.match` block gains an optional `identifierHint` (a substring or regex) that, when present, restricts the profile to a specific physical port.

This is a **non-breaking extension** of the v1 schema: profiles without `identifierHint` continue to match any port (v1 behaviour); profiles with `identifierHint` get port-specific binding.

### 1.3.4. Import Validation Pipeline

Imported `.sonikmidi.json` files run through a strict validation pipeline before any file system write:

1. JSON parse (rejects malformed syntax).
2. Manifest extraction (verifies the file has a `manifest` block with `appVersion`, `exportedAt`, `sha256`).
3. SHA-256 verification of the `mapping` block against the manifest hash (rejects corrupted files).
4. Schema version check (rejects `schemaVersion > currentMaxSupportedVersion`; runs migration for `<`).
5. Mapping parse via the existing v1 `MappingParser::parse` (rejects unresolved control targets, malformed modifiers, conflict errors).
6. Conflict detection against existing user profiles (rejects collisions unless the user picks rename/replace).

A file passing all six stages is written atomically via the v1 `.tmp` + rename pattern. A file failing any stage produces a structured error surfaced in the import dialog with line/offset details where applicable. No partial state is ever written.

### 1.3.5. Module Boundary

EPIC-0006 PRDs continue to live under `Source/Features/Midi/`. The migration framework lives in `Source/Features/Midi/Migrations/`. The import/export UI lives in `Source/Features/Midi/UI/`. No cross-module includes beyond what EPIC-0005 already established. The export format (`.sonikmidi.json`) is consumed only by `MappingStore`; no other Feature module needs to know it exists.

## 1.4. PRD Roadmap

- [x] PRD-0049: Mapping Schema Migration Framework
- [x] PRD-0050: Mapping Import / Export and `.sonikmidi.json` Bundle Format
- [x] PRD-0051: USB-Port Disambiguation & Multi-Instance Device Binding

## 1.5. Future Successor Epics

- **EPIC-0007 (planned):** HID Controller System — Native Instruments Traktor S-series, Pioneer DDJ HID-mode, NI Maschine HID. Different OS APIs, separate end-to-end stack; intentionally **not** folded into the MIDI Epic line.
- **EPIC-0008 (planned, conditional):** Multi-Port Logical Device Aggregation — only if/when the user adopts hardware exposing 2+ MIDI ports as one logical controller.
- **EPIC-0009 (planned, conditional):** Mapping Marketplace & Cloud Sync — depends on EPIC-0006's portable bundle format as its foundation.
