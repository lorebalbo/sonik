---
status: Not Implemented
epic: EPIC-0008
depends-on:
  - PRD-0001
  - PRD-0026
  - PRD-0062
---

# 1. PRD-0063: DAW State Schema & Non-Destructive Clip Model

## 1.1. Problem

EPIC-0008 introduces an in-app DAW arrangement timeline whose defining principle is that nothing on it is a recording of the DAC output: every block is a non-destructive reference into a source file already loaded on a deck (EPIC-0008 §1.1, §1.3.1). Before any of the visible machinery — the ruler, the lanes, the live projection, the clip waveforms — can exist, the application needs a single, authoritative, observable data structure to hold the arrangement. Today the central `juce::ValueTree` carries `decks` and `mixer` branches but has no notion of a timeline, no concept of a per-deck channel group, no lane abstraction, and no clip value object. Without that schema, every downstream PRD in this Epic (the master-grid service, the live-projection bridge, the coordinate transform, the UI organisms) would have to invent its own ad-hoc storage, and the four Epics that follow (recording in EPIC-0009, playback/editing in EPIC-0010, automation in EPIC-0011, persistence in EPIC-0012) would have nothing stable to attach to.

The clip abstraction in particular is foundational and non-negotiable (EPIC-0008 §1.1.1): a clip must be a pure value object — `clipId`, `sourceFileId`, `sourceStartSample`, `sourceEndSample`, `timelineStartSample`, `gainDb`, `laneId` — that *references* audio by stable id and never holds a buffer. If the schema is shaped incorrectly now, the "uncrop/extend" operation that EPIC-0010 promises (a pure mutation of `sourceStartSample`/`sourceEndSample` within `[0, sourceLengthSamples]`) becomes impossible without a lossy migration, and the stable-id relocation contract from PRD-0039 breaks. The schema must also be perfectly decoupled from the audio thread: it lives on the message thread, is mutated only there, and is observed via JUCE Listeners (EPIC-0008 §1.3.4, §1.3.5).

This PRD defines that schema and that clip value object — and nothing else. It builds no recording, no playback, no automation, no persistence, and no UI. It is the load-bearing foundation the rest of EPIC-0008 stands on.

## 1.2. Objective

The system defines a new top-level `daw` branch of the central `juce::ValueTree`, parallel to `decks` and `mixer`, together with a non-destructive `DawClip` value object, such that:

- The `daw` branch has a deterministic three-level sub-tree shape: `daw.tracks[]` (one channel group per deck) → `daw.tracks[i].lanes[]` (exactly three lanes: `Original`, `Instrumental`, `Vocal`) → `daw.tracks[i].lanes[j].clips[]`.
- A `DawClip` is a pure value object with the fields `clipId` (a `juce::Uuid`), `laneId` (the owning lane's stable id), `sourceFileId` (a stable id — library track id for the `Original` lane, stem-cache key for the stem lanes), `sourceStartSample` (`int64`), `sourceEndSample` (`int64`), `timelineStartSample` (`int64`), `sourceLengthSamples` (`int64`, the total length of the referenced source), and `gainDb` (`float`, default `0.0`).
- A clip's timeline length is exactly `sourceEndSample - sourceStartSample` (1:1 in this Epic; no stretch).
- Clips never hold audio buffers; they carry only ids and sample indices. Audio is resolved later, by id, against the library / stem cache.
- The schema exposes a single project sample-rate constant that every coordinate transform and sample index in the `daw` branch is expressed against.
- The tree is mutated only on the message thread and observed via JUCE Listeners (Observer pattern); no field is read or written from the audio thread.
- Source references are stable ids (not raw paths) so that relocation (PRD-0039) keeps clips valid.
- The schema is shaped so that future uncrop/extend (EPIC-0010) is a pure mutation of `sourceStartSample`/`sourceEndSample` within `[0, sourceLengthSamples]`, and future stretch (EPIC-0010), automation (EPIC-0011), and serialization (EPIC-0012) attach without restructuring the existing fields.

## 1.3. Developer / Integration Flow

1. A new feature slice `Source/Features/Daw/State/` is created with `DawState.h` and `DawClipModel.h`. `DawState.h` declares the `daw` branch identifiers (the tree type, the `tracks`/`lanes`/`clips` child types) as `juce::Identifier` constants, plus the project sample-rate constant (`kProjectSampleRate`, see §1.5.4). `DawClipModel.h` declares the property `juce::Identifier`s for every `DawClip` field and the helper that serialises a `DawClip` to / from a `juce::ValueTree` child node.

2. A new `Source/Features/Daw/Model/` directory is created with `DawClip.h` (the pure value object) and `ChannelGroup.h` (a pure model helper describing a per-deck group of three lanes). Neither file includes any JUCE UI module; they depend only on `juce_core` / `juce_data_structures` for `juce::Uuid` and `juce::ValueTree`.

3. `DawClip.h` defines the `DawClip` struct with the eight fields from §1.2, a defaulted `gainDb = 0.0f`, and free functions (or static members) `toValueTree(const DawClip&) -> juce::ValueTree` and `fromValueTree(const juce::ValueTree&) -> DawClip`. A `timelineLengthSamples()` accessor returns `sourceEndSample - sourceStartSample`. The struct is copyable and holds no buffers, no pointers, and no JUCE component references.

4. `ChannelGroup.h` defines a `ChannelGroup` model helper and a `LaneId` enum (`Original`, `Instrumental`, `Vocal`). It provides a factory that, given a deck index, builds the canonical three-lane sub-tree (each lane carrying a stable `laneId` `juce::Uuid` and a `laneKind` property) and helpers to look up a lane node by `LaneId`. Lanes are always pre-created, three per group (see §1.5.5).

5. The application's root `juce::ValueTree` construction (owned by the central state, parallel to where `decks` and `mixer` are attached) gains a `daw` child built by a new `DawState::createDawBranch()` factory in `DawState.h`. On creation the branch is empty of clips but may be empty of tracks too; tracks are added when a deck acquires a source (the live-projection PRD-0065 drives that, not this PRD — see §1.5.3).

6. A `DawState::ensureTrackForDeck(juce::ValueTree dawBranch, int deckIndex)` helper (declared here, exercised by later PRDs) idempotently creates the channel-group track for a deck if it does not already exist, pre-populating its three lanes via the `ChannelGroup` factory. Calling it twice for the same deck is a no-op.

7. Unit tests under `Tests/` (`DawStateSchemaTests.cpp` and `DawClipModelTests.cpp`, registered in `TestRunner.cpp`) verify the tree shape, the round-trip `DawClip` ↔ `juce::ValueTree` fidelity, the `timelineLengthSamples()` identity, lane pre-creation, the idempotency of `ensureTrackForDeck`, and the stable-id contract (ids survive a serialise / deserialise cycle).

8. No audio-thread code is added. No DSP is added. No UI is added. The branch is mutated only by message-thread helpers and observed via `juce::ValueTree::Listener`.

## 1.4. Acceptance Criteria

- [ ] A new top-level `daw` child exists on the central `juce::ValueTree`, parallel to `decks` and `mixer`, created by `DawState::createDawBranch()`.
- [ ] The branch has the shape `daw` → `daw.tracks[]` → `daw.tracks[i].lanes[]` → `daw.tracks[i].lanes[j].clips[]`, with every tree / child type declared as a `juce::Identifier` constant in `DawState.h`.
- [ ] Each track created by `ensureTrackForDeck` carries a stable deck-index property and exactly three lane children with `laneKind` ∈ { `Original`, `Instrumental`, `Vocal` }, each with a stable `laneId` `juce::Uuid`.
- [ ] `DawClip` (in `Source/Features/Daw/Model/DawClip.h`) is a copyable value object with the fields `clipId` (`juce::Uuid`), `laneId` (`juce::Uuid`), `sourceFileId` (stable id string), `sourceStartSample` (`int64`), `sourceEndSample` (`int64`), `timelineStartSample` (`int64`), `sourceLengthSamples` (`int64`), and `gainDb` (`float`, default `0.0f`).
- [ ] `DawClip` holds no audio buffers, no raw pointers to audio, and no JUCE UI references. Verified by inspection and by the fact that the type compiles in a translation unit that links only `juce_core` and `juce_data_structures`.
- [ ] `DawClip::timelineLengthSamples()` returns `sourceEndSample - sourceStartSample` for every clip.
- [ ] `toValueTree` / `fromValueTree` round-trip a `DawClip` with bit-exact fidelity for all eight fields, including the `juce::Uuid` ids and the `int64` sample indices (no precision loss, no path stored).
- [ ] `sourceFileId` is a stable id (library track id for the `Original` lane, stem-cache key for the `Instrumental` / `Vocal` lanes), never a filesystem path. A clip remains valid (resolvable by id) after a simulated relocation that changes the underlying file path (PRD-0039 contract).
- [ ] `ensureTrackForDeck` is idempotent: calling it twice for the same deck index produces exactly one track with exactly three lanes and does not duplicate or reset existing clips.
- [ ] A single project sample-rate constant (`kProjectSampleRate`) is declared in `DawState.h` and is the unit for every sample index stored in the `daw` branch.
- [ ] Mutating any clip's `sourceStartSample` or `sourceEndSample` to any value within `[0, sourceLengthSamples]` leaves the schema valid (no field needs renaming or restructuring), demonstrating the EPIC-0010 uncrop/extend path is a pure mutation.
- [ ] Unit tests in `Tests/DawStateSchemaTests.cpp` and `Tests/DawClipModelTests.cpp` are registered in `TestRunner.cpp` and cover tree shape, clip round-trip, length identity, lane pre-creation, `ensureTrackForDeck` idempotency, and id stability across a serialise / deserialise cycle.
- [ ] No audio-thread code is added by this PRD. The `daw` branch is mutated only on the message thread; no field is read or written from `processBlock` or anything it calls. Verified by inspection: the new files contain no `std::atomic` audio-thread access, no locks, and no `new` / `delete` on an audio path.
- [ ] No DSP block is added or modified by this PRD.
- [ ] No UI atom, molecule, or organism is added by this PRD; rendering is a later EPIC-0008 PRD.

## 1.5. Grey Areas

### 1.5.1. Clip Id Generation and Stability

`clipId` could be a monotonic counter, a hash of the clip's fields, or a random UUID. A counter is compact but collides after serialise / merge and is not stable under reordering; a content hash changes the moment the clip is uncropped (which would break every reference the moment EPIC-0010 edits the clip).

**Resolution:** Use `juce::Uuid`, generated once at clip creation and never derived from clip content. A UUID is globally unique, survives serialisation (EPIC-0012) and project merge, and — crucially — is invariant under every future mutation (uncrop, move, gain change), so references from automation lanes (EPIC-0011) and the UI selection model remain valid across edits. The `juce::Uuid` is stored as its canonical string form in the `juce::ValueTree` property and rehydrated on load. Lane ids follow the same rule: a `juce::Uuid` minted once when the lane is created, never derived from the lane kind, so two decks' `Original` lanes are distinguishable.

### 1.5.2. `sourceFileId` for Original vs Stem Lanes

The `Original` lane references the untouched library file, but the `Instrumental` / `Vocal` lanes reference stem audio that lives in the EPIC-0002 stem cache, which is keyed differently from the library track id. A single `sourceFileId` field must address both without the schema knowing which subsystem owns the id.

**Resolution:** `sourceFileId` is an opaque stable id string whose namespace is determined by the owning lane's `laneKind`, not by parsing the id. For the `Original` lane it holds the library track id (PRD-0026 / PRD-0001); for the `Instrumental` and `Vocal` lanes it holds the stem-cache key (EPIC-0002 / EPIC-0004). The schema stores it verbatim and never interprets it; resolution to actual audio is a later concern (EPIC-0010) that dispatches on `laneKind`. This keeps the clip a pure value object, keeps the two id namespaces decoupled, and means a future fourth source type (e.g. a bass stem) only needs a new `laneKind`, not a new clip field.

### 1.5.3. Empty Channel Groups for Decks With No Track

A deck with no loaded track has no audio to project. The schema could (a) always pre-create four tracks (one per possible deck) at startup, or (b) create a track lazily only when a deck acquires a source.

**Resolution:** Create tracks lazily via `ensureTrackForDeck`, called by the live-projection PRD (PRD-0065) when a deck first acquires a source. This PRD declares the helper and guarantees its idempotency but does not itself populate tracks. Pre-creating four empty groups at startup would clutter the timeline with three empty lanes per silent deck and force the UI to special-case "group with no clips ever". Lazy creation keeps the tree minimal and means the presence of a track is itself the signal that a deck has been used in this session. Within a created track, however, the three lanes *are* pre-created eagerly (see §1.5.5) — laziness applies at the track level, not the lane level.

### 1.5.4. Sample-Rate Ownership: Project Rate vs Per-Source Rate

Sources can have different native sample rates (a 44.1 kHz FLAC, a 48 kHz WAV, a stem cache that may resample). Every sample index in the `daw` branch (`sourceStartSample`, `timelineStartSample`, …) needs a well-defined unit, but reconciling a source's native rate with the timeline's rate is genuinely a playback / resampling problem.

**Resolution:** This PRD declares a single project sample-rate constant `kProjectSampleRate` and expresses every `*Sample` field in the `daw` branch against it. The reconciliation of a source's *native* rate to the project rate is explicitly deferred to EPIC-0010 (playback / resampling), where it belongs. For this Epic the schema simply guarantees one consistent unit so the coordinate transform (PRD-0064) and the live projection (PRD-0065) share one ruler. `sourceStartSample` / `sourceEndSample` are therefore project-rate sample indices into a conceptually project-rate view of the source; the per-source-rate mapping is a documented EPIC-0010 obligation, recorded here so no later PRD is surprised by it.

### 1.5.5. Lanes Pre-Created (Three Per Group) vs Lazily

Within a channel group, the three lanes could be created lazily (only when the deck actually plays in that source mode) or eagerly (all three the moment the track is created).

**Resolution:** Pre-create all three lanes eagerly when a track is created. The three-lane structure (`Original`, `Instrumental`, `Vocal`) is fixed and universal per EPIC-0008 §1.3.3; a deck always *could* switch source mode at any time, so all three lanes are semantically present even before any of them carries a clip. Eager creation gives the UI a stable, predictable layout (the lane headers exist immediately), avoids a race where the live-projection timer must create a lane mid-extend, and makes the channel-group factory a single deterministic operation. The cost — three empty lane nodes per used deck — is negligible. Laziness is reserved for the track level (§1.5.3), where the cost (a phantom group for an unused deck) is user-visible.

### 1.5.6. Storing `sourceLengthSamples` Redundantly vs Deriving It

`sourceLengthSamples` (the total length of the referenced source) is, in principle, derivable by resolving `sourceFileId` and querying the library / stem cache. Storing it on the clip is redundant data that could drift from the source.

**Resolution:** Store `sourceLengthSamples` on the clip. The EPIC-0010 uncrop/extend operation is defined as a clamp of `sourceStartSample` / `sourceEndSample` into `[0, sourceLengthSamples]`; making that bound available on the clip itself means the edit operation (and any validation of it) needs no synchronous library lookup, keeps the clip self-describing for serialization (EPIC-0012), and lets the model validate invariants without a cross-slice dependency. The drift risk is bounded: the referenced source's total length does not change (the original file is immutable; a re-analysed stem produces a new cache key, hence a new `sourceFileId`, hence a new clip). If a future workflow ever does mutate a source in place, the owning PRD will be responsible for refreshing `sourceLengthSamples` — but that is not a case this Epic creates.

### 1.5.7. `gainDb` Units and Default

`gainDb` is a per-clip trim. It could be stored as a linear gain factor (`1.0` = unity) or as decibels (`0.0` = unity), and its default must be the neutral value.

**Resolution:** Store `gainDb` in decibels with a default of `0.0` (unity). Decibels match the rest of Sonik's gain-staging vocabulary (channel gain, master gain in the mixer Epic are all dB-facing to the user) and make a clip's trim directly comparable to those controls. `0.0 dB` is exactly unity, so a freshly captured clip is bit-transparent by default — essential for the non-destructive guarantee (a clip that is never trimmed must reproduce the source exactly). Conversion to a linear multiplier is a playback concern (EPIC-0010); the schema stores only the dB value. No clamping range is imposed by the schema beyond what serialization tolerates; sensible UI limits are a later-PRD concern.
