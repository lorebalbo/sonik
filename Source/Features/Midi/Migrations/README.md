# 1. MIDI Mapping Schema Migrations

## 1.1. Runtime Contract

`MigrationRegistry` owns pure JSON-to-JSON transforms that upgrade MIDI mapping files one schema version at a time. The registry is owned by `MappingStore`, starts empty in schema v1 builds, and is called only while loading mappings on the JUCE Message thread.

Migration functions must obey these rules:
- Take `const juce::var&` input and return a new `juce::var` tree.
- Perform no file I/O, no network I/O, no logging, and no UI work.
- Keep no static or thread-local mutable state.
- Never run on the audio thread or MIDI callback thread.
- Leave on-disk files unchanged unless `MappingStore::saveMigratedCopy()` is called by an explicit user action.

## 1.2. Adding A Migration

When a future PR bumps the mapping schema:
- Bump `kCurrentSchemaVersion` in `MigrationRegistry.h`.
- Register exactly one migration for each new adjacent step, such as v1 to v2.
- Give every step a concise description for the MIDI settings UI.
- Add tests in `Tests/MappingMigrationTests.cpp` using representative input and a golden output expectation.
- Document the schema change in a sibling file named `vN_to_vN+1.md`.

## 1.3. Failure Handling

Missing steps, newer-than-supported schemas, and exceptions become structured `MigrationError` values. If a migration completes but `MappingParser` rejects the migrated JSON, `MappingStore` records `MigrationProducedInvalidOutput` and refuses that user mapping so the bundled fallback can remain active.