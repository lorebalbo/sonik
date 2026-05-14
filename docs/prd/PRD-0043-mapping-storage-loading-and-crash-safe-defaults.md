---
status: Implemented
epic: EPIC-0005
depends-on: [PRD-0040, PRD-0042]
---

# 1. PRD-0043: Mapping Storage, Loading and Crash-Safe Defaults

## 1.1. Problem

PRD-0042 defines the in-memory `Mapping` struct and its JSON parser. But mappings have to *come from somewhere*: a bundled default profile shipped with the binary, a user-edited file on disk, or a freshly authored binding from the MIDI Learn UI. Without a defined storage layer, every later PRD would invent its own filesystem access, file-format conventions, and error handling — guaranteeing inconsistency and silent data loss.

Three concrete dangers exist:

- **Crash on startup from a bad mapping file.** A single typo, truncated JSON, or future-version file in `~/Library/Application Support/Sonik/MidiMappings/` could throw inside `juce::JSON::parse` or trip a schema assertion deep in PRD-0042's resolver. If that crash takes down the whole application, the user is locked out of Sonik entirely, with no UI surface to fix the mapping. The application must launch even with completely garbage mapping files in the user folder.
- **No out-of-the-box experience.** A DJ plugs in a Reloop Contour Interface Edition and expects it to work immediately. Without a bundled default profile, every user must MIDI-Learn every control from scratch on first launch, defeating the purpose of advertising "supported controllers". The Generic MIDI fallback profile also needs to exist as a blank canvas for unknown devices.
- **No clear write path for MIDI Learn.** When PRD-0048's UI lets a user create or edit a binding, where does the result get written? Overwriting a bundled default in the binary is impossible; writing to a global location is unsafe across multi-user installs. Without a defined user-mapping directory and a clean overlay model, MIDI Learn cannot persist anything.

DJs using Sonik live are directly affected: the difference between "plug controller in, hit play" and "spend 20 minutes mapping 64 controls before the first track" is the difference between a usable tool and a non-starter. Equally, a single corrupted JSON file on disk must never prevent the app from launching, or a user's only recovery path is deleting files manually from a terminal.

## 1.2. Objective

The system must own all persistence concerns for MIDI mappings: locate, load, validate, and provide the active set of `Mapping` instances to downstream PRDs, and persist user edits durably without ever blocking application startup on malformed data. Specifically:

- The system ensures that two bundled default profiles ship inside the application binary: `reloop-contour-interface-edition.json` (a full curated mapping for the Reloop Contour CE with LED feedback) and `generic-midi.json` (an empty bindings profile used as a fallback for unknown devices).
- The system ensures that on application startup the user mapping directory is created if missing: `~/Library/Application Support/Sonik/MidiMappings/` on macOS, `%APPDATA%/Sonik/MidiMappings/` on Windows (resolved via `juce::File::getSpecialLocation(userApplicationDataDirectory)`).
- The system ensures that every JSON file in the user mapping directory is loaded, parsed via PRD-0042's `MappingParser::parse`, and validated, on a background thread, before the user-facing MIDI surface is wired up.
- The system ensures that any file that fails to load (file I/O error, JSON parse error, schema validation error, unknown future `schemaVersion`) is logged to the application log via `DBG` and excluded from the active set, without ever throwing an exception that propagates out of the loader. A malformed file never prevents application launch.
- The system ensures that for any connected device, the active mapping is resolved by the following priority order: (1) a user-saved file whose `device.match` regex matches the device, (2) the bundled default profile for that device if one exists, (3) the bundled Generic MIDI profile.
- The system ensures that the MappingStore exposes a thread-safe `getActiveMappingForDevice(uint64_t deviceId) const -> std::shared_ptr<const Mapping>` callable from any thread, returning a snapshot pointer that the caller may hold safely while the store is updated underneath (copy-on-write update pattern).
- The system ensures that the MappingStore exposes `saveUserMapping(const Mapping&, juce::StringRef filename) -> SaveResult`, callable on the Message thread, which atomically writes a JSON file to the user mapping directory using a `.tmp` + rename pattern so a power-loss or crash during write never produces a half-written file.
- The system ensures that the MappingStore notifies registered listeners (PRD-0048's MIDI Learn UI) when the active mapping for any device changes, via a `MappingStoreListener::activeMappingChanged(uint64_t deviceId)` callback on the Message thread.
- The system ensures that the bundled Reloop Contour Interface Edition profile in `reloop-contour-interface-edition.json` is curated from the reference `.tsi` in `midi_mappings/reloop_contour_ce/` and the Reloop manual, covering: left/right deck transport (PLAY, CUE, SYNC), pitch fader, gain, EQ (high/mid/low), 8 hot-cue pads per side, loop in/out/halve/double, jog wheel (touch / scratch / bend), beat-jump, SHIFT modifier, and outbound LED feedback declarations for every lit transport and pad button.
- The system ensures that the bundled Generic MIDI profile has `bindings: []` and `modifiers: []` but a valid `device.match.midiName: ".*"` so it matches any unknown device as a last-resort fallback.
- The system ensures that all storage code lives entirely under `Source/Features/Midi/` and depends on `Source/Features/Midi/` only.

## 1.3. User Flow

This PRD has no end-user UI directly; the user interacts with mapping files indirectly via MIDI Learn (PRD-0048) or by editing JSON files in a known folder. The "users" of this PRD are PRD-0048 (which writes) and PRD-0044 (which reads). The flow is expressed as a sequence of system events.

### 1.3.1. Application Startup: Bundled Profiles

1. `SonikApplication::initialise()` constructs `MappingStore` on the Message thread, after `MidiDeviceManager` (PRD-0040) and before any UI is shown.
2. The constructor reads the two bundled JSON profiles from `BinaryData::reloop_contour_interface_edition_json` and `BinaryData::generic_midi_json` (embedded by JUCE's `juce_add_binary_data` CMake helper).
3. Each bundled JSON is parsed via `MappingParser::parse` (PRD-0042). The bundled profiles must parse without errors; a failed parse of a bundled profile is a hard assertion in Debug builds and a silent fallback to an empty `Mapping` in Release builds (so a corrupted binary still launches, just without MIDI defaults).
4. The two resulting `Mapping` instances are stored in a `std::unordered_map<juce::String, std::shared_ptr<const Mapping>>` keyed by a stable bundled-profile id (`"reloop-contour-interface-edition"`, `"generic-midi"`).

### 1.3.2. Application Startup: User Profile Directory

1. The constructor resolves the user mapping directory: `juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Sonik/MidiMappings/")`.
2. If the directory does not exist, it is created via `createDirectory()`. If creation fails (permissions, disk full), the failure is logged via `DBG` and the store proceeds with bundled profiles only.
3. The constructor schedules a background `juce::ThreadPoolJob` to enumerate and load every `*.json` file in the directory. The Message thread does not block on this enumeration; the application UI is allowed to come up immediately with only bundled profiles active. Devices connecting before user-profile loading completes will resolve to bundled defaults.

### 1.3.3. Background Load of User Profiles

1. The background job iterates the directory's `*.json` files in alphabetical order (deterministic for diagnostics).
2. For each file: read its contents into a `juce::String` via `juce::File::loadFileAsString()`. Empty files and files larger than 1 MB (a paranoid upper bound; the Reloop reference profile is well under 100 KB) are skipped with a logged warning.
3. The file contents are passed to `juce::JSON::parse`. A parse failure logs the path and the JUCE-reported error, and continues to the next file.
4. The parsed `juce::var` is passed to `MappingParser::parse(root, sourcePath)`. The returned `ParseResult.errors` vector is logged per-entry; a `ParseResult.mapping` with at least one valid binding is accepted into the store regardless of whether other bindings failed (PRD-0042's partial-load contract).
5. The accepted `Mapping` is inserted into a separate `std::unordered_map<juce::String, std::shared_ptr<const Mapping>>` keyed by the file's stem name (e.g., `my-custom-reloop` for `my-custom-reloop.json`).
6. After every file in the directory has been processed, the background job calls `MessageManager::callAsync` to atomically swap the store's `userProfiles` map (under `std::shared_mutex` lock) and fire `MappingStoreListener::userProfilesLoaded()` on the Message thread.

### 1.3.4. Device Connection: Resolving the Active Mapping

1. A device is enumerated by `MidiDeviceManager` (either at startup or via hot-plug). The manager fires `midiDeviceAdded(deviceId)` to listeners; `MappingStore` is one such listener.
2. `MappingStore::midiDeviceAdded(deviceId)` reads the device's `MidiDeviceRecord` from the manager: `manufacturer`, `productName`.
3. The store iterates `userProfiles` first, computing for each profile whether its `deviceMatch` regexes match `(manufacturer, productName)`. The first user profile that matches wins.
4. If no user profile matches, the store iterates `bundledProfiles`. The Reloop Contour CE bundled profile matches manufacturer "Reloop" and product name regex matching "Contour Interface Edition.*".
5. If no bundled profile matches, the Generic MIDI profile is selected (its regex matches anything).
6. The resolved `std::shared_ptr<const Mapping>` is stored in `std::unordered_map<uint64_t, std::shared_ptr<const Mapping>> activeByDevice` under the store's `std::shared_mutex`. The store then fires `MappingStoreListener::activeMappingChanged(deviceId)` on the Message thread.

### 1.3.5. Reading the Active Mapping at MIDI Callback Time

1. PRD-0044's inbound router, on receiving `onMidiInbound(const MidiInboundEvent&)` on the MIDI callback thread, calls `MappingStore::getActiveMappingForDevice(event.deviceId)`.
2. The store acquires the `std::shared_mutex` in shared (read) mode, looks up the device, copies the `std::shared_ptr<const Mapping>`, releases the lock, and returns the copy. The lock is held for a few nanoseconds; under normal operation the shared lock is contention-free because writes are rare.
3. The router uses the returned pointer for the duration of the inbound event, then releases it. The `Mapping` itself is never mutated; if the store updates `activeByDevice` underneath, the router's existing shared_ptr keeps the old `Mapping` alive until the router drops it. No torn reads, no use-after-free.

### 1.3.6. MIDI Learn Persists a New Binding

1. PRD-0048's MIDI Learn UI, after the user binds a new control, constructs a new `Mapping` in memory (copy of the current active mapping for the device, with the new/edited binding inserted).
2. The UI calls `MappingStore::saveUserMapping(newMapping, "user-reloop-custom.json")` on the Message thread.
3. The store serialises the `Mapping` to a `juce::var` via `MappingSerializer::serialize(const Mapping&) -> juce::var` (a function defined in this PRD).
4. The store writes the serialised JSON to `~/Library/Application Support/Sonik/MidiMappings/user-reloop-custom.json.tmp` via `juce::File::replaceWithText`. On any I/O error, returns `SaveResult::IoFailure` with the underlying error.
5. The store calls `juce::File::moveFileTo` to rename `.tmp` → `.json` atomically. Most operating systems guarantee `rename(2)` is atomic on the same filesystem; this protects against half-written files on power loss or crash during write.
6. The store inserts the new `Mapping` into `userProfiles`, re-resolves the active mapping for the affected device, and fires `activeMappingChanged(deviceId)`.
7. The router (PRD-0044) hears the listener fire, calls `getActiveMappingForDevice` again, and the new binding is live without restarting the application.

### 1.3.7. Malformed File at Startup

1. The user has saved `~/Library/Application Support/Sonik/MidiMappings/broken.json` with truncated JSON (e.g., they edited it manually and forgot a closing brace).
2. `juce::JSON::parse` returns an empty `juce::var` and JUCE sets an error string.
3. The background loader logs `"MappingStore: failed to parse broken.json: <error>"` via `DBG` and skips the file. No exception is thrown, no other file is affected.
4. The application launches normally with the other valid user profiles and all bundled profiles active.
5. Later, the MIDI Learn UI (PRD-0048) reads `MappingStore::getLoadErrors() -> std::vector<MappingLoadError>` and displays a banner listing the broken file path and the parser error so the user can fix it.

### 1.3.8. Future Schema Version

1. A user installs Sonik v3.0 which introduces `schemaVersion: 2` with new binding fields.
2. The user then downgrades to Sonik v2.5 which only supports `schemaVersion: 1`.
3. PRD-0042's `MappingParser::parse` reads the file's `schemaVersion: 2` and returns a `ValidationError { kind: UnsupportedSchemaVersion, ... }` plus a `Mapping` containing zero bindings.
4. The store logs the warning, accepts the empty `Mapping` (which has the correct `deviceMatch`), and selects it for the device anyway. The device falls back to no bindings (effectively to the Generic MIDI profile's empty set). The user is informed via the MIDI Learn UI banner.

## 1.4. Acceptance Criteria

- [ ] The system constructs `MappingStore` on the Message thread after `MidiDeviceManager` and before any UI is shown.
- [ ] The system embeds `reloop-contour-interface-edition.json` and `generic-midi.json` in the application binary via JUCE's `juce_add_binary_data` CMake helper, with source files under `Resources/MidiMappings/`.
- [ ] The system parses both bundled profiles synchronously in the `MappingStore` constructor and stores them in a `std::unordered_map<juce::String, std::shared_ptr<const Mapping>>` keyed by a stable bundled-profile id.
- [ ] The system asserts via `jassert` in Debug builds that the bundled profile parse produced no `ValidationError` entries; in Release builds, parse failures of bundled profiles fall back to an empty `Mapping` (the application still launches).
- [ ] The system resolves the user mapping directory as `juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Sonik/MidiMappings/")` and creates it if missing.
- [ ] The system never blocks the Message thread on user-mapping enumeration; the enumeration runs as a `juce::ThreadPoolJob` on a `juce::ThreadPool` owned by the store.
- [ ] The system iterates `*.json` files in the user mapping directory in alphabetical order and parses each via `MappingParser::parse`.
- [ ] The system enforces a 1 MB per-file size cap; files exceeding the cap are skipped with a logged warning and a `MappingLoadError` recorded.
- [ ] The system handles every I/O error, JSON parse error, and `ValidationError` from `MappingParser::parse` by logging via `DBG` and continuing; no exception ever propagates out of the loader.
- [ ] The system records every load failure in a `std::vector<MappingLoadError>` accessible via `getLoadErrors() const -> std::vector<MappingLoadError>` (deep copy) for PRD-0048 to surface in the UI.
- [ ] The system materialises each successfully-parsed user profile as a `std::shared_ptr<const Mapping>` and inserts it into a `std::unordered_map<juce::String, std::shared_ptr<const Mapping>> userProfiles` keyed by the file stem.
- [ ] The system fires `MappingStoreListener::userProfilesLoaded()` on the Message thread (via `MessageManager::callAsync`) after the background load has completed.
- [ ] The system listens for `MidiDeviceManager::midiDeviceAdded(deviceId)` events and resolves the active mapping for that device using the priority order: user profiles (in `userProfiles` iteration order, first regex match wins) > bundled device-specific profiles > bundled Generic MIDI profile.
- [ ] The system stores the resolved active mapping per device in `std::unordered_map<uint64_t, std::shared_ptr<const Mapping>> activeByDevice` guarded by a `std::shared_mutex`.
- [ ] The system fires `MappingStoreListener::activeMappingChanged(deviceId)` on the Message thread after every change to `activeByDevice`.
- [ ] The system exposes `getActiveMappingForDevice(uint64_t deviceId) const -> std::shared_ptr<const Mapping>` callable from any thread, taking the shared_mutex in shared mode, returning a copy of the `shared_ptr` (or a `nullptr` if the device has no active mapping yet).
- [ ] The system's `getActiveMappingForDevice` holds the shared lock only long enough to copy the `shared_ptr` (microseconds), never holds it during user code execution.
- [ ] The system exposes `saveUserMapping(const Mapping&, juce::StringRef filename) -> SaveResult` callable on the Message thread, asserting via `JUCE_ASSERT_MESSAGE_THREAD` in Debug builds.
- [ ] The system serialises the `Mapping` to JSON via `MappingSerializer::serialize(const Mapping&) -> juce::var`, the inverse of `MappingParser::parse`, with round-trip equality verified by test fixtures.
- [ ] The system writes the serialised JSON to `<filename>.tmp` first, then atomically renames to `<filename>` via `juce::File::moveFileTo`, so a crash during write never produces a half-written file.
- [ ] The system rejects `saveUserMapping` filenames containing path separators, `..`, or null bytes, returning `SaveResult::InvalidFilename`.
- [ ] The system rejects `saveUserMapping` filenames that do not end in `.json`, returning `SaveResult::InvalidFilename`.
- [ ] The system, after a successful save, updates the in-memory `userProfiles` map, re-resolves `activeByDevice` for every connected device whose `deviceMatch` matches the saved mapping, and fires `activeMappingChanged(deviceId)` for each affected device.
- [ ] The system exposes a `MappingStoreListener` interface with methods `userProfilesLoaded()`, `activeMappingChanged(uint64_t deviceId)`, and `userMappingSaved(juce::String filename, SaveResult result)`, all invoked on the Message thread.
- [ ] The system bundled Reloop profile (`reloop-contour-interface-edition.json`) declares `device.match.manufacturer: "Reloop"` and `device.match.midiName: ".*Contour.*Interface.*Edition.*"` (regex tolerant of OS-version suffixes like " (MIDI)" or " Port 1").
- [ ] The system bundled Reloop profile contains at minimum these binding targets, covering both left-side (Deck A) and right-side (Deck B) sections: `deck.{A,B}.transport.play`, `deck.{A,B}.transport.cue`, `deck.{A,B}.transport.sync`, `deck.{A,B}.pitchFader` (with `transform: linear` — Reloop Contour CE pitch faders are 7-bit, not 14-bit), `deck.{A,B}.gain`, `deck.{A,B}.eq.high`, `deck.{A,B}.eq.mid`, `deck.{A,B}.eq.low`, `deck.{A,B}.jog.touch`, `deck.{A,B}.jog.scratch`, `deck.{A,B}.jog.bend`, `deck.{A,B}.loop.in`, `deck.{A,B}.loop.out`, `deck.{A,B}.loop.toggle`, `deck.{A,B}.loop.size.halve`, `deck.{A,B}.loop.size.double`, `deck.{A,B}.hotcue.{1..4}.trigger`, `mixer.crossfader`.
- [ ] The system bundled Reloop profile declares a `modifier: shift` driven by the Contour CE's SHIFT button, with at minimum these SHIFT-layered overloads: SHIFT + `hotcue.N.trigger` → `hotcue.N.delete`, SHIFT + `loop.size.halve/double` → `beatjump.minus1/plus1`.
- [ ] The system bundled Reloop profile declares an outbound `feedback` block for every LED-capable button: transport PLAY/CUE/SYNC, the four hot-cue pads per side, and the loop activation indicator. Each `feedback` block specifies the outbound MIDI message (channel, status, data1, onValue, offValue) and the `valueTree` key sourcing the on/off state (e.g., `deck.A.isPlaying`).
- [ ] The system bundled Generic MIDI profile (`generic-midi.json`) declares `device.match.manufacturer: ".*"`, `device.match.midiName: ".*"`, `bindings: []`, `modifiers: []`.
- [ ] The system lives entirely under `Source/Features/Midi/` and does not `#include` any header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, `Source/Features/Mixer/`, or `Source/Features/Library/`.
- [ ] The system depends only on PRD-0040 (`MidiDeviceManager`, `MidiDeviceRecord`) and PRD-0042 (`Mapping`, `MappingParser`, `ValidationError`) public headers.
- [ ] The system is covered by `MappingStoreTests.cpp` in `Tests/` that verifies: (a) bundled profiles parse without errors; (b) a malformed user JSON file does not prevent the store from constructing; (c) priority order user > bundled-device > generic resolves correctly for both matching and non-matching devices; (d) `saveUserMapping` produces a round-trippable JSON file and rejects invalid filenames; (e) `saveUserMapping`'s `.tmp` → rename pattern leaves no `.tmp` file behind on success; (f) concurrent `getActiveMappingForDevice` reads from multiple threads while a save is in progress never produce torn `Mapping` reads (verified by a TSan stress test); (g) the bundled Reloop profile exercises every documented target ID and resolves correctly under SHIFT modifier.
