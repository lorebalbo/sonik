---
status: Implemented
epic: EPIC-0012
depends-on:
  - PRD-0063
  - PRD-0087
---

# 1. PRD-0095: Session Schema & Serializer

## 1.1. Problem

EPIC-0008 through EPIC-0011 build a complete, non-destructive DAW timeline: the
`daw` `ValueTree` (PRD-0063) holds channel groups, lanes, and clips, each clip
carrying its source reference and crop window (`DawClip` fields: `clipId`,
`sourceFileId`, `sourceStartSample`, `sourceEndSample`, `timelineStartSample`,
`sourceLengthSamples`, `gainDb`, `laneId`); PRD-0087 adds automation lanes with
their breakpoint curves; the master grid / tempo reference and the timeline view
state (zoom, scroll) round out the live model. All of this lives only in memory.
The instant the DJ quits Sonik, every arrangement they recorded — every clip
crop, every automation breakpoint, every grid alignment — is gone.

Before any Save/Open UI (PRD-0096), source relocation (PRD-0097), import
(PRD-0098), or render/export (PRD-0099–0101) can exist, the project needs a
durable, well-defined on-disk representation: a file format that captures the
*entire* DAW model losslessly, plus a serializer that maps the live `daw`
`ValueTree` to that file and back. The format must reference audio **by stable
id**, never embed it, so projects stay tiny and bit-faithful to the original
FLAC/MP3 sources. It must carry a **schema version** and a **migration hook** so
that a session saved by today's build still opens in a build six months from now
after the `daw` schema has grown — mirroring the mapping schema-migration
framework (PRD-0040 in spirit). And it must write **atomically**, so a crash or
power loss mid-save can never corrupt a session the DJ spent hours building.

This PRD owns *only* the schema definition and the serializer/deserializer
round-trip. It does not own the file-picker UI, recent-files list, source-path
resolution, or the offline renderer; those are downstream PRDs that consume the
contract established here.

## 1.2. Objective

The system defines a versioned `.soniksession` document format and a serializer
such that:

- A `SessionSchema.h` header declares the canonical on-disk structure: a root
  `SONIK_SESSION` node carrying a `schemaVersion` integer attribute, a
  `projectSampleRate`, the master grid/tempo reference, the persisted view state
  (zoom, scroll, selection), and a child subtree that is the serialized `daw`
  `ValueTree` (PRD-0063) including every lane, every clip with all eight
  `DawClip` fields, and every automation lane/breakpoint (PRD-0087).
- A `SessionSerializer` exposes a `save(const juce::ValueTree& daw, …, const
  juce::File& target)` that writes the complete model to a `.soniksession` file,
  and a `load(const juce::File& source)` that reconstructs the in-memory model.
- Clips serialize their `sourceFileId` (the EPIC-0004 library track id / external
  file reference) verbatim and **never** embed audio sample data; the file is a
  pure description of *how to reassemble* the arrangement from external sources.
- The serializer round-trips losslessly: `load(save(x)) == x` for every field of
  every node in the `daw` tree, plus project metadata and view state, with no
  drift in sample-accurate positions, gain values, or breakpoint coordinates.
- The file carries a `schemaVersion` field, and `load` runs every persisted
  version through a **migration hook** (`SessionMigrator`) that upgrades older
  documents to the current schema before the model is handed back, refusing to
  load a *newer*-than-current version with a clear error.
- Unknown child nodes and unknown attributes encountered during load are
  **preserved**, not dropped, so a session written by a newer build degrades
  gracefully when (read-only) opened by an older build that supports the same
  major version.
- The write is **atomic**: the serializer writes to a temporary file in the
  destination directory and renames it over the target on success, so a partial
  write can never replace a good session file.
- No audio-thread code is touched: all serialization runs off the real-time
  thread, performs file I/O freely, and communicates with no `processBlock` path.

## 1.3. Developer / Integration Flow

1. A new header `Source/Features/Daw/Session/SessionSchema.h` declares the
   identifier constants for every on-disk node and property: the root
   `SONIK_SESSION` type, the `schemaVersion`, `projectSampleRate`,
   `appVersion`, and `savedAtUtc` attributes, the `MASTER_GRID` node (tempo
   reference, downbeat sample, time signature), the `VIEW_STATE` node
   (`zoomSamplesPerPixel`, `scrollStartSample`, `selectedClipId`), and the
   `DAW` child node that hosts the serialized PRD-0063 tree. A single
   `kCurrentSchemaVersion` constant is the source of truth for the writer.
2. `SessionSchema.h` documents the contract that the `DAW` child is a verbatim
   copy of the live `daw` `ValueTree` (PRD-0063) — the serializer does not
   re-shape it, so lanes, clips (all eight `DawClip` fields), and automation
   lanes/breakpoints (PRD-0087) are persisted by structural identity, not by a
   hand-written field-by-field projection. This keeps the serializer immune to
   additive `daw` schema growth.
3. `Source/Features/Daw/Session/SessionSerializer.h/.cpp` implements `save`:
   it builds a root `SONIK_SESSION` `ValueTree`, stamps `schemaVersion =
   kCurrentSchemaVersion`, attaches the project metadata, the `MASTER_GRID` and
   `VIEW_STATE` children, and a *copy* of the `daw` tree as the `DAW` child,
   then serializes the whole tree to the chosen on-disk encoding (see §1.5.1)
   and performs the atomic temp-write-and-rename (§1.5.5).
4. `SessionSerializer::load` reads the file, parses it back into a `ValueTree`,
   reads `schemaVersion`, and dispatches to `SessionMigrator::migrate(tree,
   fromVersion)` which applies the ordered chain of per-version upgrade steps
   until the tree matches `kCurrentSchemaVersion`. A version *newer* than
   `kCurrentSchemaVersion` is rejected with a typed `SessionLoadError`
   (`UnsupportedFutureVersion`); a malformed/truncated file is rejected with
   `CorruptFile`; a missing file with `FileNotFound`.
5. `SessionMigrator` is a free-standing, table-driven upgrader: each entry maps
   `version N → N+1` via a pure function `ValueTree(ValueTree)`. Version 1 (the
   schema this PRD ships) has no predecessor, so the migration table starts
   empty; the *framework* is delivered now so that PRD-era schema changes add a
   single step without touching `load`. This mirrors PRD-0040's mapping-migration
   pattern.
6. `load` returns a `SessionDocument` struct: the reconstructed `daw`
   `ValueTree`, the `projectSampleRate`, the master-grid reference, and the
   view-state values. Downstream PRD-0096 takes this struct, swaps it into the
   live model, and triggers the EPIC-0010 arrangement-snapshot recompile;
   PRD-0097 resolves the `sourceFileId`s. Neither concern lives in this PRD.
7. A new test file `Tests/SessionSerializerTests.cpp` builds a representative
   `daw` tree (multiple lanes, several clips with non-trivial crop windows,
   automation lanes with breakpoints), saves it, loads it back, and asserts a
   deep structural equality; it also covers the version-too-new rejection, the
   corrupt-file rejection, unknown-node preservation, and the atomic-write
   guarantee (a forced failure mid-write leaves the prior file intact).

```text
Source/Features/Daw/Session/
├─ SessionSchema.h          - node/property ids + kCurrentSchemaVersion
├─ SessionSerializer.h      - save() / load() / SessionDocument / errors
├─ SessionSerializer.cpp    - encode, decode, atomic write, dispatch
└─ SessionMigrator.h/.cpp   - table-driven version-upgrade chain
```

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Daw/Session/SessionSchema.h` declares a root
      `SONIK_SESSION` node identifier, a `schemaVersion` integer property, a
      `projectSampleRate` property, and child node identifiers for `MASTER_GRID`,
      `VIEW_STATE`, and `DAW`, plus a single `kCurrentSchemaVersion` constant
      that is the writer's source of truth.
- [ ] The serialized `DAW` child is a verbatim structural copy of the live `daw`
      `ValueTree` (PRD-0063); every lane, every clip, and every automation lane
      is persisted without a hand-written field projection.
- [ ] Every `DawClip` field — `clipId`, `sourceFileId`, `sourceStartSample`,
      `sourceEndSample`, `timelineStartSample`, `sourceLengthSamples`, `gainDb`,
      `laneId` — survives a save/load round-trip with bit-exact values (sample
      positions are integers, `gainDb` is a `double`, ids are strings).
- [ ] Every PRD-0087 automation lane and breakpoint (parameter id, breakpoint
      time in samples, value, and curve/interpolation kind) survives the
      round-trip with no coordinate drift.
- [ ] Clips serialize `sourceFileId` only; no audio sample data is ever embedded
      in the `.soniksession` file. A session referencing a multi-gigabyte FLAC is
      a small text/binary description, not a copy of the audio.
- [ ] `SessionSerializer::save(daw, metadata, target)` writes a `.soniksession`
      file containing the root node, the `schemaVersion`, the project sample
      rate, the master grid reference, the view state, and the `DAW` subtree.
- [ ] `SessionSerializer::load(source)` returns a `SessionDocument` with the
      reconstructed `daw` `ValueTree`, the project sample rate, the master-grid
      reference, and the view-state values, such that `load(save(x))` is deeply
      equal to `x` for all persisted fields.
- [ ] Persisted view state includes zoom (`zoomSamplesPerPixel`), scroll
      (`scrollStartSample`), and selection (`selectedClipId`); the exact set is
      resolved in §1.5.3.
- [ ] `load` reads `schemaVersion` and runs the document through
      `SessionMigrator::migrate` before returning; for a version equal to
      `kCurrentSchemaVersion` the migration is a no-op pass-through.
- [ ] A document whose `schemaVersion` is greater than `kCurrentSchemaVersion`
      is rejected with a typed `UnsupportedFutureVersion` error and the live
      model is left untouched.
- [ ] A malformed or truncated file is rejected with a typed `CorruptFile`
      error; a missing file with `FileNotFound`. No partial/garbage model is ever
      returned to the caller.
- [ ] Unknown child nodes and unknown attributes present in a loaded document are
      preserved through migration and re-emitted on the next save (forward
      compatibility, §1.5.6).
- [ ] `save` is atomic: it writes to a temporary file in the destination
      directory and renames it over the target only after a fully successful
      write; a simulated mid-write failure leaves any pre-existing session file
      intact and uncorrupted.
- [ ] `SessionMigrator` is a table-driven chain of `version N → N+1` pure upgrade
      functions; adding a future schema version requires adding exactly one table
      entry and bumping `kCurrentSchemaVersion`, with no change to `load`.
- [ ] The `.soniksession` extension is registered as the canonical project file
      type (§1.5.7); the serializer rejects a `save` target with a different
      extension or normalises it (resolution in §1.5.7).
- [ ] `Tests/SessionSerializerTests.cpp` covers: deep round-trip equality of a
      multi-lane / multi-clip / automation-bearing tree; future-version
      rejection; corrupt-file rejection; unknown-node preservation; and the
      atomic-write integrity guarantee.
- [ ] No audio-thread code is added or modified: all serialization performs file
      I/O off the real-time thread, allocates and uses `juce::String` /
      `juce::ValueTree` freely, and never touches a `processBlock` path.

## 1.5. Grey Areas

### 1.5.1. On-Disk Encoding: Human-Readable XML vs Compact Binary

A `juce::ValueTree` can serialize to XML (`ValueTreeToXml` / readable text) or to
JUCE's binary stream (`ValueTree::writeToStream` / compact). XML is
diff-friendly, git-friendly, and debuggable by eye; binary is smaller and faster
to parse for large trees.

**Resolution:** Ship **XML** as the on-disk encoding for v1. A DAW session — even
a dense one — is dominated by a few hundred clips and a few thousand automation
breakpoints, which is kilobytes of XML, not megabytes; the size advantage of
binary is irrelevant at this scale, while the debuggability, hand-inspectability,
and future scriptability of XML are concrete wins during a format's formative
period. Atomic-write and migration logic are encoding-agnostic, so a later switch
to binary (or an opt-in binary mode for very large sessions) is a one-line change
in the encode/decode helpers guarded by the same `schemaVersion`. The file is
gzip-compressible later if size ever matters; readability now is worth more than
bytes saved.

### 1.5.2. Schema Versioning & Migration Strategy

The format needs a versioning scheme and a migration policy: a single monotonic
integer vs semantic major/minor, and forward-only vs bidirectional migration.

**Resolution:** A single **monotonic integer** `schemaVersion`, starting at `1`,
with **forward-only** migration (`N → N+1` chain). This exactly mirrors the
mapping-migration framework (PRD-0040): every breaking or additive schema change
increments the integer and adds one ordered upgrade step. There is no
"downgrade" path — a session is always migrated *up* to the running build's
`kCurrentSchemaVersion`, never down. A document newer than the running build is
refused rather than guessed at (see §1.5.6 for how *additive* newer fields are
nonetheless tolerated read-only within the same major). The migration table is
delivered empty in v1 (no predecessors exist), but the dispatch framework ships
now so the *first* real schema change is a one-entry diff, not a refactor.

### 1.5.3. Which View State to Persist

The timeline view has several transient UI values: horizontal zoom, horizontal
scroll, vertical lane heights, the current selection, the playhead position, and
the loop region. Persisting too much couples the file to UI minutiae; too little
loses useful context on reopen.

**Resolution:** Persist **zoom**, **horizontal scroll**, and the **selected clip
id**; persist the **playhead position** and **loop region** as part of the
master-grid/transport metadata (they are arrangement state, not pure view
chrome). Do **not** persist per-lane pixel heights or window geometry — those are
app-display preferences that belong to global settings, not the portable project
file, and would make a session reopen "wrong" on a different screen size. The
`VIEW_STATE` node therefore holds `zoomSamplesPerPixel`, `scrollStartSample`, and
`selectedClipId`. All three are optional on load: a session missing them (or a
future session adding more) loads cleanly with sensible defaults, so view state
never blocks a successful model reconstruction.

### 1.5.4. Source References: Library Id vs Absolute Path vs Both

Clips reference audio by `sourceFileId`. For portability and relocation, the file
could store the library id alone (stable but meaningless on another machine
without the same library DB), an absolute path alone (portable across DB resets
but brittle to moves), or both.

**Resolution:** Store **both** — the `sourceFileId` is the authoritative
reference (already in `DawClip`), and the serializer additionally records a
**last-known absolute path** and the source's display name as *relocation hints*
in a sidecar `SOURCE_REFS` node keyed by `sourceFileId`. On open, PRD-0097
resolves the id through the library DB first; if the id is unknown (e.g. opening
a session on a fresh machine), the last-known path is offered as a relocation
starting point, reusing PRD-0039's flow. This PRD only *persists* both values;
the resolution logic is PRD-0097's. Storing the hint costs a short string per
unique source and dramatically improves cross-machine portability, which is the
whole point of a shareable project format.

### 1.5.5. Atomic Write & Corruption Safety

A naive `save` that truncates and rewrites the target in place can leave a
half-written, unloadable file if the process dies mid-write — destroying hours of
the DJ's work.

**Resolution:** Always write to a **temporary file in the same directory** as the
target (so the final rename is a same-volume atomic operation), flush and close
it fully, then **rename** it over the destination. JUCE's `File::replaceFileIn` /
a temp-then-move sequence provides this. A pre-existing session file is never
truncated until a complete, valid replacement exists on disk. On platforms where
rename-over-existing is not atomic, fall back to rename-old-aside →
rename-new-into-place → delete-old. A test forces a failure between temp-write
and rename and asserts the original file is byte-for-byte intact. This is
non-negotiable: the cost is one extra file and one rename; the benefit is that no
crash can ever corrupt a saved session.

### 1.5.6. Forward Compatibility of Unknown Nodes

A session written by a *newer* build may contain nodes/attributes an *older*
build does not understand (within the same major version). The older build must
not silently delete them on the next save, or a round-trip through an old build
would strip a newer build's data.

**Resolution:** Because the serializer persists the `daw` subtree by **structural
copy** rather than a hand-written field projection, unknown child nodes and
unknown attributes are *inherently* preserved — `ValueTree` carries them through
load, migration, and re-save untouched. The migrator's upgrade steps are written
to be additive and non-destructive (they add/rename known nodes, never blanket
strip unknown ones). The result: an older build can open, view, and re-save a
newer same-major session without data loss for the parts it does understand,
preserving the parts it does not. A *major* version bump (reserved for genuinely
incompatible restructures) is the only case allowed to drop data, and is gated by
the future-version rejection in §1.5.2.

### 1.5.7. File Extension & Type Registration

The project file needs a canonical extension and a way for the OS / file pickers
to recognise it. Options: enforce `.soniksession` strictly, accept any
extension, or normalise on save.

**Resolution:** `.soniksession` is the **canonical** extension. On `save`, the
serializer **normalises** the target: if the caller passes a path without an
extension or with a different one, the serializer appends/replaces it with
`.soniksession` rather than failing, so the file always ends up correctly typed.
On `load`, the extension is **not** enforced — content is authoritative (a
correctly structured `SONIK_SESSION` root loads regardless of extension), which
tolerates user renames and keeps round-trips robust. The associated MIME type
`application/vnd.sonik.session+xml` and the file-type registration with the OS
(for double-click-to-open) are PRD-0096's concern (it owns the UI and app
wiring); this PRD only fixes the extension string and the normalise-on-save
behaviour so downstream UI has a stable contract to register against.
