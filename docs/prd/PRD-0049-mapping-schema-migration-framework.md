---
status: Not Implemented
epic: EPIC-0006
depends-on: [PRD-0042, PRD-0043]
---

# 1. PRD-0049: Mapping Schema Migration Framework

## 1.1. Problem

EPIC-0005 ships every mapping file with `schemaVersion: 1`. PRD-0042 hard-codes the parser to that version: `MappingParser::parse` rejects any file with `schemaVersion != 1` as `UnsupportedSchemaVersion`. This is correct for v1, but it locks the format. Any future improvement — adding a per-binding `velocityCurve`, splitting `modifiers` into typed sub-blocks, introducing per-port `identifierHint` (PRD-0051) — requires bumping the version, and the *moment* that happens, every user's existing v1 file becomes unreadable.

A DJ who spent hours customising the DDM4000 profile on their machine cannot afford to lose their mappings just because Sonik updated. Worse: a user who exports a v1 profile and shares it on a forum will see it stop working for every recipient who has upgraded past v1. The "community sharing" promise of PRD-0050 dies on the day of the first schema bump.

Two non-solutions are tempting and wrong:

- **Never bump the schema.** The v1 format already has known gaps (no `velocityCurve`, no `feedbackBlinkHz` at the binding level, no port-specific `identifierHint`). Refusing to evolve traps the product.
- **Silently rewrite the file on load.** Tempting, but destroys the user's authored content if a migration has a bug, and breaks the workflow of users who maintain mapping JSON in a git repo or share `.sonikmidi.json` exports.

The professional answer — used by every long-lived JSON-config product (VS Code, Logic, Reaper, Ableton) — is a **chained migration framework**: a registry of pure transformations `migrate_vN_to_vN+1(json) -> json`, applied in sequence on load. The framework exists *before* the first schema bump so that, when v2 arrives, adding migration support is a one-line registration, not an architectural rewrite. And critically, the framework runs migrations **in memory only** unless the user explicitly opts in to overwriting their on-disk file.

EPIC-0005 leaves this gap because it would have been over-engineering at v1 (zero migrations to write). EPIC-0006 closes it: register the framework now, populate it with zero migrations today, and the first PR that bumps to v2 just adds one function and a unit test.

## 1.2. Objective

The system must implement a schema migration framework integrated into `MappingStore`'s load path that chains pure migration functions on parsed JSON before handing it to `MappingParser::parse`. Specifically:

- The system ensures that a global constant `kCurrentSchemaVersion` (initially `1`) defines the highest schema version this build supports.
- The system ensures that a `MigrationRegistry` stores migration functions keyed by `fromVersion`, each producing a JSON tree at `fromVersion + 1`. The registry exposes `apply(juce::var input, int fromVersion, int toVersion) -> MigrationResult`.
- The system ensures that `MigrationResult` is a struct `{ juce::var migratedJson; std::vector<MigrationStep> stepsApplied; std::optional<MigrationError> error; }`.
- The system ensures that `MigrationStep` records `{ int fromVersion; int toVersion; std::string description; }` for diagnostic display in PRD-0048's UI.
- The system ensures that `MigrationError` records `{ int atVersion; std::string reason; std::string sourcePath; }`.
- The system ensures that `MappingStore::loadMappingFromJson(const juce::var& json, const juce::String& sourcePath) -> LoadResult` reads the file's top-level `schemaVersion` integer, computes `target = kCurrentSchemaVersion`, and invokes `MigrationRegistry::apply(json, fromVersion, target)`.
- The system ensures that on `fromVersion == target`, no migrations are applied and the input is passed through unchanged.
- The system ensures that on `fromVersion < target`, every intermediate migration is applied in order; a missing migration (gap in the chain) produces a structured `MigrationError { reason: "no migration registered from vN to vN+1" }`.
- The system ensures that on `fromVersion > target`, the loader produces a structured error `UnsupportedSchemaVersion { fromVersion, maxSupported: target }` and refuses to load the file. The user is shown a clear "this mapping was made with a newer Sonik; please upgrade" message in PRD-0048's UI.
- The system ensures that migration functions are pure: they take a `juce::var` by const reference, return a new `juce::var`, perform zero I/O, zero allocations on file system, no logging side effects, and have no static or thread-local state.
- The system ensures that the migrated JSON is **never** written back to disk automatically. The user is offered, via PRD-0048's UI, a "Save Migrated Copy" action that writes the migrated form to a new file (the original is preserved). The active in-memory mapping uses the migrated form regardless.
- The system ensures that migration is a separate concern from validation: the migrated JSON is then fed to `MappingParser::parse` (PRD-0042), which produces the structured `ParseResult` exactly as today.
- The system ensures that every migration function ships with a unit test in `Tests/MappingMigrationTests.cpp` exercising at least one representative v(N) input and asserting the v(N+1) output is byte-identical to a recorded golden expectation.
- The system ensures that the framework is integrated but **empty** in this PRD's deliverable: the registry has zero entries because `kCurrentSchemaVersion == 1`. The first migration is added by a future PR alongside the v2 schema bump.

## 1.3. User Flow

This PRD has no end-user UI directly. The user interacts indirectly through PRD-0048's MIDI Learn UI when a v2-or-newer file is loaded.

### 1.3.1. v1 File Loads After Framework Lands (Today, Zero Migrations)

1. User launches Sonik. `MappingStore::loadAllMappings` iterates the user mapping directory and finds `ddm4000-my-mix.json` with `schemaVersion: 1`.
2. The store reads `schemaVersion: 1`, computes target `kCurrentSchemaVersion = 1`. `fromVersion == target`.
3. The store skips the migration registry entirely and passes the JSON straight to `MappingParser::parse`.
4. Result: identical behaviour to EPIC-0005. The framework is a no-op for v1 files in v1 builds. Zero user-visible change.

### 1.3.2. v1 File Loads After v2 Schema Bump (Future Build)

1. A future PR bumps `kCurrentSchemaVersion = 2` and registers `migrate_v1_to_v2`. The migration, for instance, splits the v1 string-or-array `modifier` field into a typed `modifierIds: [...]` block.
2. The user upgrades Sonik. They launch the new build. `MappingStore::loadAllMappings` finds the same `ddm4000-my-mix.json` still at `schemaVersion: 1`.
3. The store reads `schemaVersion: 1`, target `2`. `fromVersion < target`. The store invokes `MigrationRegistry::apply(json, 1, 2)`.
4. The registry runs `migrate_v1_to_v2(json)`. The output is a new `juce::var` with `schemaVersion: 2` and the transformed `modifierIds` block.
5. The store hands the migrated JSON to `MappingParser::parse` (v2-aware). Parsing succeeds. The mapping is loaded into memory and active.
6. PRD-0048's UI displays a non-intrusive banner: "1 mapping was migrated from schema v1 to v2 in memory. Save migrated copy?". The user can dismiss or accept.
7. If the user clicks "Save Migrated Copy", `MappingStore` writes the migrated JSON atomically to `ddm4000-my-mix.json` (replacing the v1 content). If the user dismisses, the next launch will re-migrate transparently — no user data is lost.

### 1.3.3. v3 File Loads in a v2 Build

1. A friend exports a `.sonikmidi.json` from Sonik 3.0 (which is at `schemaVersion: 3`). The user is on Sonik 2.5 (which is at `kCurrentSchemaVersion: 2`).
2. The user imports the file via PRD-0050's import dialog. The store reads `schemaVersion: 3`, target `2`. `fromVersion > target`.
3. The store produces `UnsupportedSchemaVersion { fromVersion: 3, maxSupported: 2 }`. The import is rejected.
4. PRD-0050's import dialog shows: "This mapping was created with Sonik 3.0 (schema v3). Your version supports up to v2. Please upgrade Sonik to import this file."
5. No file system write occurs. The user's existing mappings are untouched.

### 1.3.4. Broken Migration Function

1. A future migration `migrate_v2_to_v3` has a bug and produces a JSON tree that fails `MappingParser::parse` with a `ValidationError`.
2. The store records the failure as a `MappingLoadError { sourcePath, kind: MigrationProducedInvalidOutput, migrationDescription, parserError }`.
3. The active mapping falls back to the bundled default for the affected device (PRD-0043 behaviour).
4. PRD-0048's UI surfaces the error in the banner with a "Report Bug" button.

## 1.4. Acceptance Criteria

- [ ] The system defines `kCurrentSchemaVersion` as a `constexpr int` in `Source/Features/Midi/Migrations/MigrationRegistry.h`, set to `1` in this PRD's deliverable.
- [ ] The system defines `MigrationFunction` as a `using MigrationFunction = std::function<juce::var(const juce::var&)>` (heap-backed because migrations may close over registered helpers; called only on the Message thread, never on the audio or MIDI callback thread).
- [ ] The system defines `MigrationRegistry` as a class with `registerMigration(int fromVersion, juce::String description, MigrationFunction fn)` and `apply(juce::var input, int fromVersion, int toVersion) const -> MigrationResult`. The registry is owned by `MappingStore` and constructed empty in this PRD.
- [ ] The system defines `MigrationResult` as `{ juce::var migratedJson; std::vector<MigrationStep> stepsApplied; std::optional<MigrationError> error; }`.
- [ ] The system defines `MigrationStep` as `{ int fromVersion; int toVersion; juce::String description; }`.
- [ ] The system defines `MigrationError` as `{ int atVersion; juce::String reason; juce::String sourcePath; }`.
- [ ] The system's `MigrationRegistry::apply` iterates from `fromVersion` up to `toVersion - 1`, looking up each step. A missing step produces `MigrationError { atVersion: i, reason: "no migration registered from vN to vN+1" }` and aborts the chain.
- [ ] The system's `MigrationRegistry::apply`, when `fromVersion == toVersion`, returns the input unchanged with `stepsApplied.empty()` and no error.
- [ ] The system's `MigrationRegistry::apply`, when `fromVersion > toVersion`, produces `MigrationError { atVersion: fromVersion, reason: "schema version newer than supported" }`.
- [ ] The system extends `MappingStore::loadMappingFromJson` (PRD-0043) to: (1) read `schemaVersion` from the input; (2) call `MigrationRegistry::apply` to bring it forward; (3) on migration error, return a `MappingLoadError { kind: MigrationFailed | UnsupportedSchemaVersion, ... }` and abort the load for this file; (4) on success, pass the migrated JSON to `MappingParser::parse`; (5) on parser failure, return a `MappingLoadError { kind: MigrationProducedInvalidOutput, migrationDescription: lastStep.description, parserError: ... }`.
- [ ] The system extends `MappingLoadError` (PRD-0043) with new variants: `MigrationFailed`, `UnsupportedSchemaVersion`, `MigrationProducedInvalidOutput`.
- [ ] The system extends `MappingStore` with `getMigratedMappings() const -> std::vector<MigratedMappingInfo>` where `MigratedMappingInfo = { uint64_t deviceId; juce::String mappingId; juce::String sourcePath; std::vector<MigrationStep> stepsApplied; }` for PRD-0048's UI to surface the "Save Migrated Copy" banner.
- [ ] The system extends `MappingStore` with `saveMigratedCopy(const juce::String& mappingId) -> bool` that writes the in-memory migrated JSON to the original source path atomically via `.tmp` + rename, then removes the mapping from `getMigratedMappings()`.
- [ ] The system performs every migration call on the Message thread (asserted via `JUCE_ASSERT_MESSAGE_THREAD` at `MigrationRegistry::apply` entry); no audio-thread or MIDI-callback-thread interaction.
- [ ] The system disallows migration functions from performing file I/O, logging, or holding any state across calls. This is documented as a contract; violations are caught by code review and by the unit tests' input/output golden-equality check.
- [ ] The system delivers the framework with zero registered migrations (consistent with `kCurrentSchemaVersion == 1`).
- [ ] The system is covered by `MappingMigrationTests.cpp` in `Tests/` verifying: (a) `fromVersion == toVersion` path returns input unchanged and applies zero steps; (b) `fromVersion > toVersion` produces `UnsupportedSchemaVersion`; (c) `fromVersion < toVersion` with no registered step produces `MigrationFailed`; (d) a synthetic test-only registered migration `v1 -> v2` is chained correctly and the result is byte-identical to a golden expectation; (e) a synthetic faulty `v1 -> v2` that produces invalid output is correctly surfaced as `MigrationProducedInvalidOutput`; (f) `saveMigratedCopy` writes atomically and removes the entry from `getMigratedMappings`; (g) migration functions called outside the Message thread trigger `JUCE_ASSERT_MESSAGE_THREAD` in Debug.
- [ ] The system documents the contract for adding a new migration in `Source/Features/Midi/Migrations/README.md`: register in `MigrationRegistry`, add unit test with golden input/output, bump `kCurrentSchemaVersion`, document the change in a sibling `vN_to_vN+1.md` file.
