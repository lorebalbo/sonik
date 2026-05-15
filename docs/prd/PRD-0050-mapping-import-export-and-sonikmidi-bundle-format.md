---
status: Not Implemented
epic: EPIC-0006
depends-on: [PRD-0043, PRD-0048, PRD-0049]
---

# 1. PRD-0050: Mapping Import / Export and `.sonikmidi.json` Bundle Format

## 1.1. Problem

After EPIC-0005, the only way a Sonik user can move a custom MIDI mapping between machines (or share it with the DJ community) is to manually locate `~/Library/Application Support/Sonik/MidiMappings/`, find the right JSON file by guessing at its name, copy it onto a USB stick or email it, and trust the recipient to drop it into the same folder on the other side. That is a path filled with footguns:

- **Wrong destination folder.** New users have no idea this folder exists. A profile dropped into Downloads does nothing.
- **Filename collision.** Two community profiles named `behringer-ddm4000.json` overwrite each other silently.
- **Corruption in transit.** A truncated download or a copy-paste through a forum text editor that mangles UTF-8 produces malformed JSON that silently falls back to the bundled default — with no error visible to the user.
- **Schema version mismatch.** A profile exported from Sonik 3.0 (schema v3) crashes silently when dropped onto a Sonik 2.5 install. Without the import pipeline running PRD-0049's migration framework, the user just sees "this mapping doesn't load."
- **No preview.** The user has no way to see *what is inside* a `.json` file before committing — how many bindings, what device it targets, what modifiers it declares — short of opening it in a text editor.

The professional answer — used by every product with portable user-config (Traktor `.tsi`, Logic `.cst`, Ableton `.ablpreset`, VS Code `.code-profile`) — is a **first-class portable bundle format** with a dedicated import/export UI. The user picks "Export" in Settings, gets a save dialog, picks a destination, ends up with a single self-contained file. The recipient picks "Import" in Settings, gets a file picker, sees a *preview* before committing (device match, binding count, modifier list, schema version, app version it was made on), and the import either succeeds atomically or fails with a clear human-readable reason — never silently.

## 1.2. Objective

The system must implement an import/export UI on top of `MappingStore` and a portable, self-contained, integrity-checked bundle format `.sonikmidi.json` that wraps a mapping with a manifest header. Specifically:

- The system ensures that the `.sonikmidi.json` bundle is a single JSON file with two top-level objects: `manifest` and `mapping`.
- The system ensures that `manifest` records `{ appVersion: string, sonikSchemaVersionAtExport: int, exportedAt: ISO8601 string, sha256: string, exporterDeviceName?: string }` — `sha256` is the lower-case hex SHA-256 of the JSON-serialised `mapping` block computed with sorted keys.
- The system ensures that `mapping` is the exact same shape as a v1 user mapping file in `~/Library/Application Support/Sonik/MidiMappings/`, with no semantic difference: a `mapping` block extracted from a bundle is a valid input to `MappingParser::parse` after PRD-0049 migration.
- The system ensures that export emits the active mapping for a chosen device (or a chosen named user mapping) as a `.sonikmidi.json` file via `juce::FileChooser::launchAsync` with the `Save File` mode, default filename `<sanitised-device-name>__<sanitised-mapping-name>.sonikmidi.json`.
- The system ensures that import opens `juce::FileChooser` in `Open File` mode with a `.sonikmidi.json` filter and (on success) runs the bundle through a validation pipeline before writing anything to disk.
- The system ensures that the validation pipeline performs, in order: (1) JSON syntax parse, (2) manifest presence and required-field check, (3) SHA-256 verification of the `mapping` block against `manifest.sha256`, (4) schema migration via PRD-0049's `MigrationRegistry::apply`, (5) `MappingParser::parse` on the migrated mapping, (6) device-match conflict detection against existing user mappings, (7) target-id existence check against `ControlTargetRegistry`.
- The system ensures that each validation stage produces a structured error; the dialog displays the failing stage and the reason, and never proceeds to file-system write on any failure.
- The system ensures that the import dialog shows a **preview pane** populated by stages (1)–(5): device match string, target binding count, modifier count, schema version, app version at export, exported-at timestamp, and exporter device name (if recorded).
- The system ensures that conflict resolution (stage 6) presents a modal with three choices: **Rename** (user provides a new mapping name, file written under the new name), **Replace** (existing user mapping is overwritten atomically via PRD-0043's `.tmp` + rename), **Cancel** (no file system change). Bundled mappings are read-only and cannot be replaced.
- The system ensures that on successful import, the new mapping is written atomically to the user mapping directory, registered with `MappingStore`, and (if the user chooses in the dialog) immediately activated for the matching device.
- The system ensures that the export and import flows are invoked from a new toolbar in PRD-0048's `MidiSettingsPanel` with two buttons: `Import…` and `Export…`. The buttons obey `DESIGN.md` (2px borders, monochrome, Space Mono).
- The system ensures that import and export both happen on a background thread (`juce::ThreadPoolJob`) so the UI never blocks on file I/O or hash computation.
- The system ensures that the `.sonikmidi.json` MIME-type-equivalent and file extension are documented in the Sonik docs so forum users and community curators know what to expect.

## 1.3. User Flow

### 1.3.1. Exporting a Customised DDM4000 Profile

1. The user has been customising a duplicated copy of the bundled DDM4000 profile under the name "DDM4000 (my mix)". They open Settings → MIDI.
2. They select the "DDM4000 (my mix)" entry in the active-profile dropdown for the Behringer DDM4000 device, then click the new toolbar **Export…** button.
3. A native save dialog opens with the default filename `behringer_ddm4000__ddm4000_my_mix.sonikmidi.json`. They navigate to Desktop and click Save.
4. A background `juce::ThreadPoolJob` runs: it serialises the mapping's JSON with sorted keys, computes the SHA-256, assembles the manifest, writes the combined bundle atomically via `.tmp` + rename, and posts a success toast back to the Message thread.
5. The user sees a brief toast: "Exported to Desktop/behringer_ddm4000__ddm4000_my_mix.sonikmidi.json".
6. They email the file to a friend.

### 1.3.2. Importing a Friend's Profile (Happy Path)

1. The friend opens the email attachment and saves it to Downloads. They open Settings → MIDI, click the toolbar **Import…** button.
2. A native open dialog filters to `.sonikmidi.json`. They pick the file.
3. The dialog transitions to a preview pane showing:
   - **Device:** Behringer DDM4000 (MIDI)
   - **Mapping name:** DDM4000 (my mix)
   - **Schema:** v1
   - **Sonik app version at export:** 1.2.3
   - **Exported:** 2026-05-12 14:30 (Friend's MacBook)
   - **Bindings:** 124
   - **Modifiers:** 1 (shift)
4. SHA-256 verification passes silently in the background. No conflict because the friend has no existing user mapping under that name.
5. The user clicks **Import**. The bundle is written to `~/Library/Application Support/Sonik/MidiMappings/ddm4000_my_mix.json` atomically. `MappingStore` fires `mappingAdded`. The Settings panel's profile dropdown for the Behringer DDM4000 now lists "DDM4000 (my mix) (imported)" alongside the bundled profiles.
6. The import dialog offers a final option: "Activate now for Behringer DDM4000?" Yes → `MappingStore::setActiveMapping` fires. The DDM4000 LEDs perform their boot-dump for the new profile (PRD-0047). Within a second, the user's controller is configured.

### 1.3.3. Importing With a Filename Conflict

1. The user has already imported a "DDM4000 (my mix)" mapping. A second friend sends them a different mapping with the same name.
2. The user runs Import…, picks the new file. The preview pane shows the bundle. Validation stages 1–5 pass.
3. Stage 6 detects the conflict: an existing user mapping under the same id targets the same device.
4. A conflict modal appears: "A mapping named 'DDM4000 (my mix)' already exists for this device. Rename, Replace, or Cancel?"
5. They click **Rename** and type "DDM4000 (friend 2)". The bundle is written to `ddm4000_friend_2.json` atomically. Both profiles now appear in the dropdown.

### 1.3.4. Importing a Corrupted File

1. The user downloads a `.sonikmidi.json` through a flaky network. The download was truncated.
2. They run Import…, pick the file. The dialog runs stage (1) JSON parse → fails: "Unexpected end of input at line 47."
3. The preview pane shows an error banner with the failing stage, the parser's reported line/offset, and a "Show File" button (reveals in Finder). The **Import** button is disabled.
4. No file system write occurs. The user re-downloads.

### 1.3.5. Importing a Newer-Schema File

1. The user is on a build where `kCurrentSchemaVersion = 2`. They receive a `.sonikmidi.json` exported from Sonik 4.0 where `schemaVersion = 4`.
2. They run Import…, pick the file. Stages (1)–(3) pass. Stage (4) runs PRD-0049's `MigrationRegistry::apply(json, 4, 2)` → `MigrationError { reason: "schema version newer than supported" }`.
3. The preview pane shows: "This mapping was created with Sonik 4.0 (schema v4). Your version supports up to v2. Please upgrade Sonik to import this mapping."
4. The **Import** button is disabled. No file system write.

### 1.3.6. Importing an Older-Schema File With Migration Needed

1. The user is on a future build where `kCurrentSchemaVersion = 3`. They receive a `.sonikmidi.json` from a friend still on v1.
2. They run Import…, pick the file. Stages (1)–(3) pass. Stage (4) runs `MigrationRegistry::apply(json, 1, 3)`, which chains `v1 -> v2` then `v2 -> v3`.
3. Stage (5) parses the migrated mapping successfully. The preview pane shows: "**Migrated from v1 to v3 during import** (2 migration steps applied)" alongside the regular preview info.
4. Stages (6)–(7) pass. The user clicks **Import**. The **migrated** mapping is written to disk. The original `.sonikmidi.json` is untouched.

### 1.3.7. Importing With an Unknown Control Target

1. The user receives a `.sonikmidi.json` that was authored for a future Sonik build with a target `deck.A.beatfx.bloom` that does not exist in the current build's `ControlTargetRegistry`.
2. Stages (1)–(6) pass. Stage (7) detects 1 unknown target and produces a non-fatal warning.
3. The preview pane shows: "1 of 124 bindings references an unknown control target (`deck.A.beatfx.bloom`). It will be skipped on load."
4. The user can still click **Import**; the mapping is written, and on load `MappingParser::parse` drops the unknown binding (existing v1 behaviour from PRD-0042 partial-load).

## 1.4. Acceptance Criteria

- [ ] The system defines `SonikMidiBundle` as a POD struct `{ BundleManifest manifest; juce::var mappingJson; }`.
- [ ] The system defines `BundleManifest` as `{ juce::String appVersion; int sonikSchemaVersionAtExport; juce::String exportedAtIso8601; juce::String sha256; juce::String exporterDeviceName; }`.
- [ ] The system defines `SonikMidiBundleSerializer::toJson(const SonikMidiBundle&) -> juce::var` and `SonikMidiBundleSerializer::fromJson(const juce::var&) -> Result<SonikMidiBundle, BundleParseError>`.
- [ ] The system implements `sha256OfSortedJson(const juce::var&) -> juce::String` returning a 64-char lower-case hex hash, computed by serialising the input with deterministic key ordering (deep recursive sort) and feeding the bytes to a JUCE-bundled or third-party SHA-256 implementation.
- [ ] The system implements `MappingExportService::exportActiveMapping(uint64_t deviceId, const juce::String& mappingId, juce::File destination) -> juce::ThreadPoolJob` that, on a background thread, reads the active mapping JSON from `MappingStore`, computes the SHA-256, assembles the bundle, writes `destination.tmp` and renames to `destination`, and posts a completion callback to the Message thread via `juce::MessageManager::callAsync`.
- [ ] The system implements `MappingImportService::importBundle(juce::File source, ImportCallbacks) -> juce::ThreadPoolJob` that runs the 7-stage validation pipeline on a background thread and posts progressive updates (preview info, stage failures) to the Message thread via callbacks.
- [ ] The system defines `ImportStage` as an enum: `JsonParse`, `ManifestExtract`, `Sha256Verify`, `SchemaMigrate`, `MappingParse`, `ConflictDetect`, `TargetIdValidate`.
- [ ] The system defines `ImportError` as a tagged union over `ImportStage` carrying stage-specific structured detail (`{stage, reason, sourceLine?, sourceOffset?, parserError?, migrationError?, conflictExistingMappingId?, unknownTargetIds?}`).
- [ ] The system's import pipeline halts on the first fatal error (stages 1–6 are fatal). Stage 7 (unknown targets) is non-fatal and produces a warning that the user may accept.
- [ ] The system extends PRD-0048's `MidiSettingsPanel` with a top toolbar containing `Import…` and `Export…` buttons styled per `DESIGN.md`.
- [ ] The system implements the `Import…` flow as: button click → `juce::FileChooser` open dialog filtered to `*.sonikmidi.json` → on selection, launch `MappingImportService::importBundle` → display preview pane on success of stage 5 → user clicks `Import` to commit (or `Cancel`).
- [ ] The system implements the `Export…` flow as: dropdown to pick a mapping to export (defaults to the active mapping of the currently selected device) → `juce::FileChooser` save dialog → launch `MappingExportService::exportActiveMapping` → show success toast on completion.
- [ ] The system implements the conflict modal (stage 6 fatal collision) as a JUCE `AlertWindow` with three actions: `Rename` (presents a text field for the new mapping id, sanitised for filesystem-safe characters), `Replace` (only enabled if the existing mapping is a user mapping; disabled for bundled mappings), `Cancel`.
- [ ] The system, on successful import, writes the migrated bundle's `mapping` block atomically to `~/Library/Application Support/Sonik/MidiMappings/<sanitised-mapping-id>.json` (the bundled file outside the bundle is **not** preserved on disk; only the migrated form is stored).
- [ ] The system, on successful import, registers the new mapping with `MappingStore::registerImportedMapping(mappingJson, mappingId, deviceMatchInfo)` (a new method added in this PRD) and fires `MappingStoreListener::mappingAdded`.
- [ ] The system offers a final post-import dialog: "Activate now for <device name>?" with Yes / No buttons; on Yes, calls `MappingStore::setActiveMapping`.
- [ ] The system performs all hashing, JSON parsing, and file I/O on background threads (`juce::ThreadPoolJob`); the Message thread is never blocked.
- [ ] The system asserts `JUCE_ASSERT_MESSAGE_THREAD` on every UI-touching callback and `! juce::MessageManager::getInstance()->isThisTheMessageThread()` (or equivalent) on every background-thread entry.
- [ ] The system never produces a partially-written file on import error (atomic `.tmp` + rename pattern; on any failure, the `.tmp` is deleted).
- [ ] The system is covered by `MappingImportExportTests.cpp` in `Tests/` verifying: (a) round-trip export → import produces an identical mapping; (b) SHA-256 verification rejects a tampered `mapping` block; (c) missing manifest fields produce stage-2 errors; (d) a v1 bundle in a v2 build is migrated and imported successfully; (e) a vN+1 bundle in a vN build produces `UnsupportedSchemaVersion`; (f) a bundle with a binding referencing an unknown target produces a non-fatal stage-7 warning and the import proceeds with the binding dropped; (g) conflict detection identifies an existing user-mapping collision and surfaces it as a fatal stage-6 error; (h) Rename and Replace conflict-resolution paths each write the correct file; (i) atomic write semantics: a synthetic failure during the file write leaves no partial file.
