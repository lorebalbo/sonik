---
name: "EPIC-0010: Timeline Playback Engine & Non-Destructive Clip Editing"
status: Implemented
---

# 1. EPIC-0010: Timeline Playback Engine & Non-Destructive Clip Editing

## 1.1. Goal and Vision

Make the recorded arrangement **audible and editable**. EPIC-0008 built the
timeline; EPIC-0009 recorded a set onto it as non-destructive clips. This Epic
adds the DAW's own **Play / Pause / Stop** transport that renders those clips
back to the speakers by reading the original source files, plus the full suite
of **non-destructive clip editing** every DAW user expects — move, trim, split,
delete, and above all **uncrop/extend** (revealing more of the source than was
originally captured).

The experience: after recording, the DJ presses **Play** in the DAW and hears
the mix they performed, reconstructed sample-accurately from the FLAC/MP3
sources at full quality. They can scrub, loop a region for review, and then
*edit*: drag a clip so the track now enters at 0:30 instead of 1:00, grab a
clip edge and drag it outward to **uncrop** — extending the audio to reveal
parts of the source song that were not played live (a pure adjustment of the
crop window, never a stretch), trim the other edge, split a clip, or delete it.
Because every clip is a reference into the source (EPIC-0008 §1.3.1), all of
this is lossless and reversible.

This Epic owns the **timeline render engine** (turning clips → audio) and
**clip editing**. It does **not** own automation (EPIC-0011) or project
persistence/export (EPIC-0012), though its render engine is the substrate both
later Epics build on.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- **DAW transport**: Play, Pause, Stop for the arrangement, independent of the
  decks' own transports; a transport playhead that scrubs and follows playback.
- **Region/loop review**: select a timeline range and loop it for listening.
- **Audible reconstruction**: the engine reads each clip's source crop
  (Original / Instrumental / Vocal) from the original files / stem cache and
  sums the active lanes into the master output, click-free.
- **Non-destructive clip editing**:
  - **Move**: drag a clip along the timeline (changes `timelineStartSample`).
  - **Trim**: drag either edge to shorten the visible/audible crop.
  - **Uncrop / Extend**: drag an edge outward to lengthen the crop within the
    source bounds `[0, sourceLengthSamples]` — reveal more of the song (the
    "start at 0:30 instead of 1:00" / "extend the intro" workflow).
  - **Split**: cut a clip into two contiguous clips at the playhead/cursor.
  - **Delete**: remove a clip.
  - **Snap to grid** toggle for all edits, using the master grid (EPIC-0008).
- **Per-clip gain trim** applied during playback (already modelled in EPIC-0008).
- **Undo / redo** for all editing operations.

Foundational systems (non-user-facing):

- A **timeline render engine**: a real-time-safe player that, per audio block,
  determines which clips on which lanes overlap the current playhead, reads the
  corresponding source samples, applies clip gain, and sums them to the output —
  with click-free clip-boundary handling.
- A **streaming source-read layer** that serves clip audio without disk I/O on
  the audio thread (pre-buffering / background reader threads feeding lock-free
  ring buffers), reusing the decode infrastructure (PRD-0003) and stem cache
  (EPIC-0002).
- A **sample-rate reconciliation** step so sources at differing sample rates are
  resampled to the project rate for playback.
- An **edit command layer** (command pattern) over the `daw` `ValueTree`
  enabling undo/redo and keeping all mutations on the message thread.

### 1.2.2. Out of Scope

- **Automation lanes and their playback** (tempo/filter/EQ/gain/key-lock/etc.)
  → EPIC-0011. The render engine here plays clips at their captured/edited crop
  and clip gain only; automated parameter motion is layered on in EPIC-0011.
- **Project save/load and audio file export/bounce** → EPIC-0012 (export reuses
  this engine in an offline render mode).
- **Recording / capturing new performance** → EPIC-0009.
- **Time-stretching clips to a new tempo** — editing here is crop/move/trim,
  not stretch. Tempo-stretch of clips is a possible future enhancement.
- **Crossfades between overlapping clips on the same lane** beyond the short
  anti-click ramp (full user-drawn crossfades are a future enhancement).

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Timeline Render Engine (audio-thread safety)

The render engine runs inside the `processBlock` chain and must obey `AGENTS.md`
without exception:

- **No disk I/O on the audio thread.** Clip audio is streamed by background
  reader threads into pre-allocated lock-free ring buffers; the audio thread
  only copies from those buffers.
- **No allocation / no locks** in `processBlock`. The set of "active clips for
  this block" is resolved from a pre-built, lock-free-published schedule
  (e.g., a SeqLock-published snapshot of the arrangement, mirroring EPIC-0003's
  pattern), so the audio thread never walks the `ValueTree`.
- **Click-free boundaries**: short (e.g., 64-sample) ramps at clip starts/ends
  and at edit-induced discontinuities, consistent with EPIC-0002's crossfade
  approach.

### 1.3.2. Arrangement Snapshot for the Audio Thread

The editable model is the `daw` `ValueTree` (message thread). The audio thread
cannot read it directly. A **message-thread compiler** transforms the tree into
a compact, immutable **playback schedule** (sorted clip events per lane with
source-read handles) and publishes it to the audio thread via a SeqLock /
double-buffer. Every edit recompiles and republishes; the audio thread always
reads a coherent snapshot. This mirrors the master-clock publication pattern
already proven in EPIC-0003.

### 1.3.3. Streaming Source Reads

- Each active clip is backed by a **background streamer** that pre-rolls source
  audio (Original file via PRD-0003 decoders, or Instrumental/Vocal from the
  EPIC-0002 stem cache) into a ring buffer ahead of the playhead.
- Seeks/scrubs/edits trigger a re-prime of the relevant streamers off the audio
  thread.
- Streamers are pooled and pre-allocated; starting/stopping playback does not
  allocate on the audio thread.

### 1.3.4. Non-Destructive Editing Semantics

All edits are pure mutations of the clip value object (EPIC-0008 §1.3.1):

- **Move** → `timelineStartSample`.
- **Trim start** → raise `sourceStartSample` (+ shift `timelineStartSample`).
- **Trim end** → lower `sourceEndSample`.
- **Uncrop start** → lower `sourceStartSample` toward 0 (reveal earlier source).
- **Uncrop end** → raise `sourceEndSample` toward `sourceLengthSamples`.
- **Split** → replace one clip with two sharing the source, partitioned at the
  cut sample.
- **Delete** → remove the clip node.

Edits are clamped to `[0, sourceLengthSamples]`; no edit ever fabricates or
loses audio data. All run through the command layer for undo/redo.

### 1.3.5. Edit Command Layer & Undo/Redo

A command-pattern layer wraps every mutation (`MoveClip`, `TrimClip`,
`UncropClip`, `SplitClip`, `DeleteClip`, `SetClipGain`). Commands execute on the
message thread, push onto an undo stack, and trigger a schedule recompile
(§1.3.2). This keeps editing, recording (EPIC-0009), and future automation edits
(EPIC-0011) on one consistent, reversible mutation path.

### 1.3.6. Sample-Rate Reconciliation

Sources may differ from the project sample rate. Streamers resample to the
project rate during the background read (high-quality resampler), so the audio
thread always receives project-rate samples. The project rate is fixed per
session and shared with the master-grid service (EPIC-0008).

### 1.3.7. File Structure, Design, Reuse

```text
Source/Features/Daw/
├─ Playback/
│  ├─ TimelineRenderer.h/.cpp        // audio-thread block renderer
│  ├─ ArrangementSnapshot.h          // SeqLock-published schedule
│  ├─ ClipStreamer.h/.cpp            // background source pre-roll
│  └─ DawTransport.h/.cpp            // play/pause/stop/loop-region
├─ Editing/
│  ├─ EditCommands.h/.cpp            // move/trim/uncrop/split/delete
│  └─ UndoStack.h/.cpp
└─ Ui/ (additions)                   // transport bar, clip drag/trim handles
```

- Reuses PRD-0003 decoding, EPIC-0002 stem cache, PRD-0006 waveforms, and the
  `daw` model from EPIC-0008.
- All UI complies with `DESIGN.md`; clip drag/trim/uncrop handles and the
  transport bar follow the monochrome component language.
- The render engine feeds the existing master output stage (EPIC-0007) so the
  arrangement plays through the same output path as the decks.

## 1.4. PRD Roadmap

- [x] PRD-0079: Arrangement Snapshot Compiler & SeqLock Publication (message-thread `ValueTree` → audio-thread schedule)
- [x] PRD-0080: Clip Streaming Source-Read Layer (background pre-roll, ring buffers, sample-rate reconciliation)
- [x] PRD-0081: Timeline Render Engine (audio-thread clip summing, click-free boundaries) into the master output
- [x] PRD-0082: DAW Transport (Play / Pause / Stop, playhead, region loop for review)
- [x] PRD-0083: Edit Command Layer & Undo/Redo over the `daw` model
- [x] PRD-0084: Clip Move & Trim Interactions (grid-snap drag handles)
- [x] PRD-0085: Clip Uncrop / Extend & Split (lossless crop-window editing)
- [x] PRD-0086: Clip Delete, Per-Clip Gain & Editing Validation
