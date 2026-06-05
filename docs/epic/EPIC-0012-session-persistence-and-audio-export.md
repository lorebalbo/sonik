---
name: "EPIC-0012: Session Persistence & Audio Export"
status: In Progress
---

# 1. EPIC-0012: Session Persistence & Audio Export

## 1.1. Goal and Vision

Close the DAW loop: let the DJ **save**, **reopen**, and **export** the
arrangements they record. EPIC-0008–0011 build a non-destructive, automatable,
playable timeline; this Epic makes it durable and shareable. The DJ can save a
session to a native project file, reopen it later exactly as it was (clips,
automation, grid, per-clip edits), and **export the finished mix to an audio
file** in their format of choice (FLAC, MP3, WAV) via an offline render that
reuses the EPIC-0010 timeline engine and EPIC-0011 automation applier.

The experience: after building a set, the DJ hits **Save** and picks a location
for a `.soniksession` project. Reopening it restores the full arrangement and
automation, with source audio re-linked by stable ids (relocatable via PRD-0039
if files moved). When ready to share, the DJ chooses **Export**, selects a
format and quality, and Sonik renders the arrangement offline — reading the
original FLAC/MP3 sources and applying the recorded automation — to a single
high-quality audio file. Because the timeline is reconstructed from source files
(not a DAC capture), the export is as clean as the originals.

This Epic owns the **project file format**, **save/load**, and **audio
export/bounce**. It builds on every prior DAW Epic and introduces no new
arrangement, recording, automation, or editing concepts.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- **Save / Save As / Open** for DAW sessions, plus recent-sessions access.
- A **native project format** (`.soniksession`) capturing the full `daw` model:
  channel groups/lanes, every clip (source ref + crop + timeline position +
  gain), all automation lanes (continuous + boolean), the master grid/tempo
  reference, project sample rate, and zoom/scroll view state.
- **Source re-linking on open**: clips reference stable library/stem ids
  (EPIC-0004 / EPIC-0002); on open, missing sources are flagged and offered for
  relocation, reusing PRD-0039's relocation flow.
- **Audio export / bounce** of the arrangement to a file:
  - Formats: **WAV** (lossless PCM), **FLAC** (lossless compressed), **MP3**
    (lossy, selectable bitrate).
  - Selectable sample rate / bit depth (where applicable) and export range
    (whole arrangement or a selected region/loop).
  - **Offline (faster-than-real-time) render** using the EPIC-0010 engine and
    EPIC-0011 automation applier in non-real-time mode.
- **Export progress + cancel** UI (mirroring the stem-separation progress
  pattern from EPIC-0002).
- **Load existing audio files into the DAW**: import an external audio file as a
  clip on a lane (so the DAW can also be used to assemble/listen, not only to
  record from decks).

Foundational systems (non-user-facing):

- A **serializer/deserializer** mapping the `daw` `ValueTree` ⇄ `.soniksession`
  on disk, with a **schema version** and a migration hook (mirroring the mapping
  schema-migration framework, PRD-0049, in spirit).
- An **offline render driver** that runs the timeline engine and automation
  applier deterministically without the real-time audio device, writing to an
  `juce::AudioFormatWriter` (WAV/FLAC/MP3 encoders).
- A **source-id resolution layer** binding project clip ids to current file
  paths via the library DB (EPIC-0004) and stem cache (EPIC-0002).

### 1.2.2. Out of Scope

- **Stems/multitrack export** (exporting each lane or channel group as separate
  files) — possible future enhancement; this Epic exports the summed mix (with a
  possible per-region selection). The model supports it later without migration.
- **Cloud sync / sharing services** — local files only.
- **Interchange formats** (Ableton `.als`, stem `.stem.mp4`, rekordbox XML) —
  future enhancement.
- **Recording, automation, editing, and playback mechanics** — owned by
  EPIC-0009 / EPIC-0011 / EPIC-0010 respectively; this Epic only persists and
  renders them.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Project File Format

- `.soniksession` is a versioned, structured document (JSON or JUCE `ValueTree`
  XML/binary) containing the entire `daw` branch plus project metadata
  (sample rate, master-tempo reference/automation, view state, format version).
- It stores **source references by stable id**, never embedded audio, keeping
  projects tiny and lossless. Optionally records the last-known path per source
  for relocation hints.
- A **schema version field** plus a forward-migration hook ensures older
  sessions open in newer builds (same philosophy as PRD-0049 mapping
  migrations).

### 1.3.2. Save / Load Reconciliation With the Live Model

- Loading a session reconstructs the `daw` `ValueTree` and triggers the
  EPIC-0010 arrangement-snapshot recompile so playback/edit work immediately.
- Source ids resolve through the library DB (EPIC-0004); unresolved ids mark the
  affected clips as "missing source" and surface the PRD-0039 relocation flow.
  Unresolved clips are preserved (never silently dropped) so relinking restores
  them fully.

### 1.3.3. Offline Render / Export Engine

- Export reuses the **timeline render engine** (EPIC-0010) and **automation
  applier** (EPIC-0011) in a **non-real-time driver** that advances the playhead
  block-by-block as fast as source reads allow, summing to an output buffer
  written via `juce::AudioFormatWriter`.
- The render path must be **deterministic** and **bit-faithful to the sources**:
  no added dither beyond what the chosen format requires, no resampling beyond
  project-rate reconciliation already defined in EPIC-0010.
- Encoders: WAV (PCM), FLAC, and MP3 (via JUCE's available format writers; MP3
  encoding availability/licensing verified during the implementing PRD).
- Runs on a background thread with progress + cancel, never blocking the UI
  (consistent with EPIC-0002's separation thread pattern).

### 1.3.4. Audio-Thread Safety

- Save/load and export run entirely **off the real-time audio thread**.
- The offline render driver does **not** use the live audio device; it pulls the
  same engine in a non-real-time loop, so `processBlock`'s `CLAUDE.md`
  guarantees are irrelevant to export yet the engine's no-alloc/no-lock inner
  loop is preserved for code reuse.
- During *live* (real-time) listening, all prior Epics' audio-thread rules
  continue to apply unchanged.

### 1.3.5. File Structure, Design, Reuse

```text
Source/Features/Daw/
├─ Persistence/
│  ├─ SessionSerializer.h/.cpp      // ValueTree ⇄ .soniksession
│  ├─ SessionSchema.h               // version + migration hook
│  └─ SourceIdResolver.h/.cpp       // clip ids ⇄ library/stem paths
└─ Export/
   ├─ OfflineRenderDriver.h/.cpp    // non-real-time engine driver
   ├─ AudioExporter.h/.cpp          // WAV / FLAC / MP3 writers
   └─ Ui/ (export dialog, progress)
```

- Reuses EPIC-0010 render engine, EPIC-0011 automation applier, EPIC-0004
  library DB, EPIC-0002 stem cache, PRD-0003 decoders, and PRD-0039 relocation.
- All UI complies with `DESIGN.md` (monochrome export dialog, dithered progress
  indicator, pixel-art icons).

## 1.4. PRD Roadmap

- [x] PRD-0095: Session Schema & Serializer (`.soniksession` format, versioning + migration hook)
- [ ] PRD-0096: Save / Save As / Open & Recent Sessions (model ⇄ file, view-state restore)
- [ ] PRD-0097: Source-Id Resolution & Missing-Source Relocation on Open (reuse PRD-0039)
- [ ] PRD-0098: Import External Audio File as Clip (load files into a lane)
- [ ] PRD-0099: Offline Render Driver (non-real-time timeline + automation evaluation)
- [ ] PRD-0100: Audio Exporter — WAV / FLAC / MP3 (format/quality/range options)
- [ ] PRD-0101: Export Dialog, Progress & Cancel (background thread, DESIGN.md-compliant)
