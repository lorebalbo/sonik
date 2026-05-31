---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0003
  - PRD-0073
  - PRD-0083
  - PRD-0095
---

# 1. PRD-0098: Import External Audio File as Clip

## 1.1. Problem

The DAW timeline (EPIC-0008 through EPIC-0011) is built around clips that
originate from the live decks: the DJ records a performance and the captured
audio becomes a `DawClip` referencing a recorded source (EPIC-0009). But a DAW
is also an assembly surface. A DJ preparing a set, building a mashup, or laying
down a backing bed frequently has audio that never touched a deck — an acapella
they downloaded, a one-shot they exported from another tool, a loop a friend
sent, a finished stem bounce. Today the only path to get that audio onto a lane
is to load it into a live deck and record it back in, which is wasteful (a
real-time pass for audio that already exists as a file), lossy in spirit (it
goes through the live signal path rather than the original samples), and
disconnected from the offline, sample-accurate nature of the timeline.

What is missing is a direct **file → clip** path: the DJ drags an audio file
from Finder/Explorer onto a specific lane at a specific time, or picks it from a
File menu, and Sonik decodes it (reusing the PRD-0003 decoders), registers a
stable source id for it (so the resulting clip persists and re-resolves through
PRD-0095/PRD-0097 exactly like a recorded clip), generates its waveform
(PRD-0006), and places a `DawClip` on the target lane via the PRD-0073 placement
engine — spanning the full file length, with the crop window initialised to the
entire file. From that moment the clip is indistinguishable from a recorded one:
it moves, trims, and un-crops under EPIC-0010's editing model, and every import
is a single undoable action under PRD-0083.

Without this, the DAW can only ever contain what was performed live, which
defeats half the purpose of having a timeline at all.

## 1.2. Objective

The system imports an external audio file directly onto a DAW lane as a new clip
such that:

- The DJ can initiate an import by (a) dragging one or more audio files from the
  OS file system onto a specific lane at a specific timeline position, or (b)
  using a File menu / lane context-menu "Import Audio File…" action that opens a
  native file chooser and places the result at the playhead (or a sensible
  default position, see §1.5.7).
- The file is decoded via the PRD-0003 decoding layer, reusing its supported
  formats (MP3, FLAC, WAV, AIFF), its validation (unsupported / corrupt files
  are rejected with a clear transient error and no clip is created), and its
  stereo-interleaved-float normalisation.
- A **stable source id** is assigned for the imported file and registered in the
  source-id resolution layer (EPIC-0012 §1.3.1 / PRD-0095), so the clip
  references the file by id, not by raw path, and re-resolves on session reload
  (PRD-0095) and relocation (PRD-0097).
- A waveform is generated for the imported source via PRD-0006 and is available
  to the lane renderer before or shortly after the clip appears (a placeholder
  is acceptable until the waveform is ready, consistent with PRD-0003's
  progressive-readiness pattern).
- A `DawClip` is created on the **target lane** at the **drop position** (or
  playhead) via the PRD-0073 placement engine, with `sourceLengthSamples` equal
  to the decoded file length and the **crop window initialised to the full file**
  (`cropStart = 0`, `cropEnd = sourceLength`).
- The imported file's audio is **reconciled to the session sample rate**: the
  decoded buffer is resampled to the project rate (consistent with PRD-0080's
  streaming/playback rate contract and PRD-0003 step 13), so the clip plays at
  correct pitch/duration on the timeline regardless of the file's native rate.
- The created clip is a **first-class timeline clip**: it is editable exactly
  like a recorded clip under EPIC-0010 (move, trim, un-crop, gain) with no
  special-casing, and re-resolves/persists identically.
- The entire import (source registration + clip placement) is a **single
  undoable transaction** under PRD-0083: one undo removes the clip and unwinds
  the placement; one redo restores it. Source-id registration is reference-
  counted so undo does not strand or prematurely evict the source (see §1.5.2).
- The operation runs **entirely off the real-time audio thread**: decoding,
  resampling, waveform generation, and source registration happen on a
  background thread; the clip becomes audible only once its source buffer is
  atomically published into the engine (reusing PRD-0003's swap pattern).

## 1.3. User Flow

1. The DJ drags an audio file (e.g. `acapella.flac`) from Finder onto Lane 3 of
   the arrangement at roughly bar 17. The lane shows a drop-target highlight
   under the cursor while the file hovers.
2. On drop, Sonik validates the file via the PRD-0003 layer. If the extension /
   contents are unsupported or corrupt, a transient error appears on the lane
   ("Unsupported file format" / "File could not be decoded") and no clip is
   created. The flow ends here on failure.
3. On a valid file, Sonik begins background decoding. A lightweight placeholder
   clip (or a "decoding…" affordance) appears at the drop position on Lane 3 so
   the DJ sees that the import is in progress, while every other lane and live
   playback stay fully responsive.
4. Decoding completes. Sonik resamples the buffer to the session rate (if the
   file's native rate differs), assigns and registers a stable source id, and
   kicks off waveform generation (PRD-0006).
5. The placement engine (PRD-0073) creates a `DawClip` on Lane 3 starting at the
   drop position (snapped to the grid per §1.5.4), with length equal to the
   decoded file and the crop window covering the whole file. The placeholder is
   replaced by the real clip; the waveform fills in as it becomes ready.
6. The DJ can immediately drag the clip to bar 19, trim its right edge to keep
   only the first 8 bars, then later drag that same edge back out (un-crop) to
   reveal the rest of the file — all under the standard EPIC-0010 editing model.
7. The DJ presses Cmd-Z. The whole import (clip + placement) is undone in one
   step; the lane returns to empty at that position. Cmd-Shift-Z re-imports
   without re-decoding (the source remains registered/reference-counted).
8. Alternatively, the DJ chooses **File ▸ Import Audio File…**, picks a WAV in
   the native chooser, and the clip is placed at the playhead on the currently
   focused lane following the same decode → register → place pipeline.

## 1.4. Acceptance Criteria

- [ ] A file dragged from the OS file system onto a specific lane creates, on
      drop, exactly one `DawClip` on that lane at the drop position (subject to
      grid snapping, §1.5.4), spanning the full decoded file length.
- [ ] A **File menu / context-menu "Import Audio File…"** action opens a native
      `juce::FileChooser` filtered to supported extensions and places the
      resulting clip at the playhead on the focused/target lane.
- [ ] Decoding reuses the PRD-0003 decoding layer: supported formats are MP3,
      FLAC, WAV, and AIFF; unsupported (AAC, OGG, WMA, DRM) and corrupt files are
      rejected at validation with a clear transient error and **no clip is
      created**.
- [ ] The decoded audio is reconciled to the **session sample rate** (resampled
      if the file's native rate differs), so the clip plays at correct pitch and
      duration on the timeline; the original file's rate is retained as metadata.
- [ ] A multi-channel or mono source is normalised to stereo-interleaved float
      per PRD-0003 (mono → dual-mono, multi-channel → stereo downmix) before clip
      creation.
- [ ] A **stable source id** is assigned and registered in the source-id
      resolution layer for the imported file; the created `DawClip` references
      the source by id, never by raw path, and the last-known path is recorded as
      a relocation hint (consistent with EPIC-0012 §1.3.1).
- [ ] The clip's crop window is initialised to the **full file**
      (`cropStart = 0`, `cropEnd = sourceLengthSamples`), and `sourceLength`
      equals the decoded file length at the session rate.
- [ ] A waveform is generated for the imported source via PRD-0006 and rendered
      on the lane; a placeholder/progressive state is acceptable until it is
      ready, with no blocking of UI or audio.
- [ ] The created clip is editable identically to a recorded clip under
      EPIC-0010: it can be moved, trimmed (both edges), un-cropped to reveal
      material beyond the initial crop, and gain-adjusted, with no code path that
      special-cases "imported" vs "recorded" clips downstream of creation.
- [ ] The import is a **single undoable transaction** under PRD-0083: one undo
      removes the clip and unwinds its placement; one redo restores it without
      re-decoding the file.
- [ ] Source-id registration is **reference-counted**: undoing an import does not
      evict a source still referenced by another clip, and redo reattaches to the
      existing registered source rather than re-registering.
- [ ] Decoding, resampling, waveform generation, and source registration run on a
      **background thread**; the clip's source buffer is published to the engine
      via an atomic pointer swap (PRD-0003 pattern). No allocation, lock, or I/O
      is introduced on the real-time audio thread by this PRD.
- [ ] Dropping **multiple files** at once places them as sequential clips on the
      target lane (or as a documented single-file-only behaviour, see §1.5.7);
      the chosen behaviour is covered by a test.
- [ ] A very large file is handled per §1.5.6 (load-with-confirmation reusing
      PRD-0003's large-file guard) without freezing the UI or audio.
- [ ] Tests under `Tests/` cover: a successful drop-to-clip placement at a known
      position; sample-rate reconciliation (a 96 kHz file on a 44.1 kHz session
      produces a clip of the correct timeline length); crop-window initialisation
      to full file; rejection of an unsupported/corrupt file with no clip
      created; undo/redo of an import (single transaction, ref-counted source);
      and grid snapping of the drop position.

## 1.5. Grey Areas

### 1.5.1. Supported Formats and Validation Reuse

Import could define its own format whitelist or delegate entirely to PRD-0003.

**Resolution:** Delegate entirely to PRD-0003's `AudioFormatManager`-backed
validation and decoding. The supported set is exactly MP3, FLAC, WAV, AIFF (and
whatever PRD-0003 supports at the time), and the rejection messages are reused
verbatim. There is no second source of truth for "what Sonik can decode": the
import path is a new *entry point* to the existing decoder, not a new decoder.
This guarantees that any format PRD-0003 gains later is importable for free, and
that import never accepts a file a deck would reject (or vice versa).

### 1.5.2. Source-Id Assignment: Hash vs Path vs Library Id

An external file has no library id yet. The source id could be derived from the
absolute path, from a content hash of the decoded/raw bytes, or from a freshly
minted opaque id, and it must coexist with EPIC-0004 library ids and EPIC-0002
stem ids in the same resolution layer.

**Resolution:** Mint an opaque, stable source id for the import and register it
in the source-id resolution layer (PRD-0095) with the file's **content hash**
recorded as the relocation/identity key and the absolute path recorded as a
relocation hint. Path alone is too fragile (the file moves and the project
breaks with no way to re-link by content); a raw hash as the *id itself* couples
the project format to a hashing scheme. An opaque id keyed by content hash gives
the best of both: identical files imported twice can be de-duplicated to one
source (optional, see below), and PRD-0097 relocation can re-link by content
when the path goes stale. If the imported file is *also* added to the library
(§1.5.3), the resolver maps the source id to the library id so the two views
agree. De-duplication of byte-identical re-imports is permitted but not required
by this PRD.

### 1.5.3. Add-to-Library vs Session-Only Reference

The imported file could be silently added to the EPIC-0004 library (so it is
searchable/analysable like deck tracks) or kept as a session-only reference that
exists only inside the `.soniksession`.

**Resolution:** Session-only reference by default, with an **optional**
"Add to library" affordance deferred as non-blocking. Importing a one-shot or a
friend's loop onto a lane should not silently populate the DJ's curated library
with transient assets — that pollutes search and analysis queues. The clip works
fully (plays, edits, persists, relocates) purely via the source-id resolution
layer without a library row. A future enhancement (or a checkbox in the import
dialog) may offer "also add to library," at which point the resolver maps the
source id to the new library id (§1.5.2); until then, import stays library-
neutral. This PRD only requires the session-only path; the add-to-library toggle
is explicitly out of scope and noted for EPIC-0004 follow-up.

### 1.5.4. Drop Position Snapping

The drop X-coordinate maps to a timeline position that is almost never exactly on
a beat. The clip start could land sample-accurately under the cursor or snap to
the grid (PRD-0074).

**Resolution:** Snap the clip start to the active grid resolution via PRD-0074,
respecting the same snap-enabled / snap-disabled toggle that governs clip drags
in EPIC-0010 (hold the snap-override modifier to drop free). DJs assembling on a
timeline expect imported material to land on the grid by default exactly as a
moved clip does; making import the one operation that ignores snap would be
surprising. Reusing PRD-0074's snap means import inherits the same resolution,
modifier, and visual feedback as every other placement, with zero new snapping
logic.

### 1.5.5. Sample-Rate and Channel-Count Reconciliation

The file may be 96 kHz/mono while the session is 44.1 kHz/stereo. Reconciliation
could happen at import (bake a resampled buffer) or lazily at playback.

**Resolution:** Reconcile **at import**, baking a session-rate, stereo-
interleaved-float buffer as the clip's source, consistent with PRD-0080's
streaming contract and PRD-0003 step 13. Channel normalisation follows PRD-0003
(mono → dual-mono, multi-channel → stereo downmix). Baking at import keeps the
timeline engine's inner loop free of per-clip resampling and guarantees the
clip's `sourceLengthSamples` is already in session-rate terms, so crop/trim math
in EPIC-0010 is uniform across recorded and imported clips. The original native
rate/channel count is preserved as display metadata only. (If a future Epic
introduces project-rate change with re-bake, imported sources re-reconcile via
the same path as recorded sources — no special case.)

### 1.5.6. Very Large File Handling: Stream vs Load

A 700 MB WAV fully decoded into RAM is heavy; the timeline could stream it from
disk instead.

**Resolution:** Fully decode into a memory-resident buffer (matching recorded
clips and PRD-0003's model), gated by PRD-0003's existing **large-file
confirmation** guard ("This file is very large… Continue?"). Streaming imported
clips from disk would diverge the timeline's clip-source model into two kinds
(in-memory recorded vs streamed imported), complicating crop/trim, undo, and the
offline render driver (PRD-0099+). Keeping every clip source in memory preserves
a single, uniform clip model; the large-file guard prevents accidental memory
blow-ups. A future streaming-clip enhancement, if needed, would apply uniformly
to all clip kinds rather than being import-specific, and is out of scope here.

### 1.5.7. Lane Targeting and Multi-File Drops

A drop has an unambiguous target lane (the lane under the cursor), but a menu
import has no cursor; and a drag may carry several files at once.

**Resolution:** For **drag-drop**, the target lane is the lane under the cursor
and the position is the cursor's timeline position (snapped per §1.5.4). For
**menu import**, the target is the currently focused/selected lane (falling back
to the first lane if none is focused) and the position is the playhead. For
**multi-file drops**, place the files as sequential, non-overlapping clips on the
target lane in the OS-provided order, each starting where the previous ends
(snapped); if no contiguous space is available, append after the last clip on the
lane. This keeps single-file (the common case) trivial and gives multi-file a
predictable, non-destructive layout rather than stacking overlaps. A future
enhancement may offer "spread across lanes" or a drop-preview; this PRD commits
only to the sequential-on-one-lane behaviour and tests it.
