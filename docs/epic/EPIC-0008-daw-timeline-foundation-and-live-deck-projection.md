---
name: "EPIC-0008: DAW Timeline Foundation & Live Deck Projection"
status: Implemented
---

# 1. EPIC-0008: DAW Timeline Foundation & Live Deck Projection

## 1.1. Goal and Vision

Introduce the foundational layer of Sonik's in-app Digital Audio Workstation
(DAW): a non-destructive arrangement timeline docked at the top of the
application that visualises, in real time, what the DJ is doing on the decks
and mixer. This Epic builds the *canvas* — the data model, the master grid,
the per-deck channel groups, the non-destructive clip abstraction, and the
always-on "live projection" that draws a deck's audio onto its timeline lane
as the deck plays — but deliberately stops short of recording, playback of the
arrangement, automation, editing, and export, which are the subjects of the
four Epics that follow (EPIC-0009 … EPIC-0012).

The defining concept of Sonik's DAW is that it is **event-/arrangement-based,
not audio-capture-based**. Nothing in the timeline is a recording of the DAC
output. Every visual block ("clip") on the timeline is a *non-destructive
reference* — a crop window `[sourceStartSample, sourceEndSample]` — into the
original FLAC/MP3/WAV file loaded on a deck. This guarantees the timeline always
reflects the highest-quality source audio (no double-conversion, no stem
re-mux artifacts) and makes every block losslessly extendable later (EPIC-0010).

The end-user experience this Epic delivers: the DJ loads a track on Deck A.
The DAW shows that deck's **channel group** — three stacked lanes (Original,
Instrumental, Vocal) — initially empty. The moment the deck starts playing, a
clip begins growing on the active lane(s), in lockstep with playback, starting
from the source position the playhead is at and lengthening sample-for-sample
as the track plays. The clip is drawn against a master-tempo grid (bars/beats)
shared with the decks. The DJ can zoom, scroll, and read the arrangement, but
this Epic does not yet let them press the DAW's own Record or Play.

### 1.1.1. Industry Context

| Software | In-app arrangement view | Source model |
|---|---|---|
| Ableton Live (as MIDI host) | Arrangement timeline | Clips reference source files non-destructively |
| Traktor / Serato / rekordbox | None (audio recorder only) | Flat stereo capture |
| VirtualDJ | Sampler / loop recorder only | Captured audio |

No mainstream DJ application offers a true non-destructive arrangement DAW that
reconstructs a set from source files. Sonik's approach is closer to a DAW's
arrangement page (Ableton/Logic) hosting the decks as "instruments", which is
why the clip-as-source-crop model is foundational and non-negotiable.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- A **DAW panel docked at the top of the application**, above the decks/mixer,
  collapsible/expandable, fully compliant with `DESIGN.md`.
- A **time ruler** showing bars and beats (musical time), driven by the master
  tempo, with a fixed grid the DJ can read against.
- **Per-deck channel groups**: for each active deck (1–4) a group header plus
  **three horizontal lanes** — `Original`, `Instrumental`, `Vocal` — that map
  one-to-one to the deck's playable sources (the original file and the two
  stems from EPIC-0002).
- **Live deck projection**: while a deck is playing, a clip is rendered on the
  lane(s) corresponding to that deck's currently-active source mode, growing in
  real time from the current source position. The clip renders the source
  waveform (reusing the analysis from PRD-0006).
- **Grid-relative placement of the live projection**: the clip's first sample
  is anchored to the live playhead position on the timeline; the grid lines
  shown under it derive from the master tempo (visual alignment is finalised
  by the capture rules in EPIC-0009 — this Epic only renders the live, ongoing
  block).
- **Horizontal zoom** (musical: bars-per-screen) and **horizontal scroll**.
- **Vertical scroll / lane collapse** when more decks/lanes exist than fit.
- A **live playhead/now-line** indicating the current timeline position.

Foundational systems (non-user-facing):

- A `Source/Features/Daw/` feature slice owning the DAW's state, model, and UI.
- The **arrangement data model** as a dedicated `juce::ValueTree` branch
  (`daw`), with sub-trees for tracks (channel groups), lanes, and clips, plus
  every clip storing a non-destructive source reference
  (`sourceFileId`, `sourceStartSample`, `sourceEndSample`, `timelineStartSample`).
- A **master-grid service** that reconciles the DAW's musical grid with the
  existing `MasterClockManager` (EPIC-0003): the DAW grid does not invent a
  second tempo; it reads the authoritative master BPM and phase origin.
- A **live-projection bridge**: a lock-free, message-thread-side observer that
  reads each deck's published playhead/source position and source-mode and
  extends the corresponding live clip in the model at UI refresh rate.
- **Timeline clip waveform rendering** that reuses the per-track waveform
  analysis cache from PRD-0006 (no new analysis pass).
- A **coordinate/transform layer** mapping samples ↔ musical time ↔ screen
  pixels for the ruler, grid, clips, and playhead.

### 1.2.2. Out of Scope (owned by later Epics)

- **The DAW's own Record button and event capture** → EPIC-0009.
- **Translating hot cues, beat jumps, and loops into clip splits/repeats** →
  EPIC-0009.
- **Playing back the arrangement (listening to the recorded mix) and the DAW's
  own Play/Pause transport** → EPIC-0010.
- **Non-destructive clip editing** (move, trim, split, uncrop/extend, delete) →
  EPIC-0010.
- **Automation lanes** (tempo, filter, EQ, gain, key-lock, pitch-stretch,
  key-stepper) → EPIC-0011.
- **Project save/load and audio export** → EPIC-0012.
- **The deck-side "play original vs. play separated stems" toggle** that feeds
  the three-lane channel-group model → handled as a PRD under EPIC-0002 (Stem
  Separation); this Epic consumes the resulting source-mode signal but does not
  build the toggle.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Non-Destructive Clip Model (the core abstraction)

Every clip is a value object referencing a source, never copied audio:

```text
DawClip {
  clipId            : uuid
  laneId            : uuid          // Original | Instrumental | Vocal lane
  sourceFileId      : library track / stem-cache reference
  sourceStartSample : int64         // crop start into the source
  sourceEndSample   : int64         // crop end into the source
  timelineStartSample : int64       // position on the timeline (samples @ project SR)
  gainDb            : float          // clip-level trim (default 0)
}
```

- The clip length on the timeline equals `sourceEndSample - sourceStartSample`
  (1:1 in this Epic; time-stretching of clips is out of scope until alignment
  is introduced in EPIC-0009 and resampling in playback EPIC-0010).
- "Uncrop/extend" (EPIC-0010) is a pure mutation of `sourceStartSample` /
  `sourceEndSample` within `[0, sourceLengthSamples]` — the abstraction is
  designed now so that later editing is trivial and lossless.
- Clips never hold audio buffers. Rendering (visual now, audible in EPIC-0010)
  always reads from the source file / stem cache by id.

### 1.3.2. Master Grid ↔ Master Clock Reconciliation

The DAW must **not** create a second source of tempo truth. EPIC-0003 already
defines `MasterClockManager` and a coherent `MasterClockSnapshot`
(`masterBPM`, `masterPhaseOriginSample`, `masterIsPlaying`) published via a
SeqLock. This Epic adds a **read-only grid service** on the message thread that:

- Derives bar/beat grid lines for the ruler from `masterBPM` and the project
  sample rate.
- Treats the master clock's phase origin as the grid's beat-0 reference so the
  DAW grid and deck beatgrids share one phase.
- Exposes the project sample rate and a samples↔beats conversion used by every
  coordinate transform.

The DAW does not yet *drive* the master tempo (that is master-tempo automation,
EPIC-0011). It only reads it. Any future writer (automation) will go through
`MasterClockManager`, preserving the single-source-of-truth invariant.

### 1.3.3. Channel-Group / Three-Lane Source Mapping

Each deck projects onto a **group of three lanes** because EPIC-0002 makes three
distinct sources playable per deck:

- `Original` — the untouched source file (highest quality; no separation
  artifacts).
- `Instrumental` — the summed instrumental stem.
- `Vocal` — the vocal stem.

The deck's *active source mode* (which of the three is producing audio) is
provided by EPIC-0002 + the new stem-source toggle PRD. This Epic renders the
live projection onto exactly the lane(s) that are audible:

- Original mode → clip on `Original` lane only.
- Stems mode with both stems → clips on `Instrumental` and `Vocal` lanes.
- Stems mode with one stem muted → clip only on the audible stem lane.

The mapping is read from deck state; this Epic does not decide playback policy.

### 1.3.4. Live-Projection Bridge (thread safety)

The live projection must update smoothly without ever touching the audio thread
unsafely. Each `DeckAudioSource` already publishes its playhead/source position
via `std::atomic` (used by waveforms and sync). The DAW adds a message-thread
`LiveProjectionTimer` (UI refresh rate, ~30–60 Hz) that:

- Reads each deck's atomic source position + active source mode.
- Extends the "live clip" for that deck's group in the `ValueTree` model
  (purely on the message thread — model mutation never happens on the audio
  thread).
- Detects play→stop transitions (deck stopped) to finalise the live clip's
  `sourceEndSample`.

No audio-thread allocation, locks, or I/O are introduced. Per `AGENTS.md`, all
model mutation and rendering happen on the message/UI thread.

### 1.3.5. State Architecture

- A new top-level `daw` branch is added to the central `juce::ValueTree`,
  parallel to `decks`/`mixer`, with sub-trees:
  `daw.tracks[]` → `daw.tracks[i].lanes[]` → `daw.tracks[i].lanes[j].clips[]`.
- The DAW UI observes the tree via JUCE Listeners (Observer pattern); it never
  polls the audio thread.
- Source references are stable ids (library track id / stem-cache key from
  EPIC-0002 + EPIC-0004), not raw paths, so relocation (PRD-0039) keeps clips
  valid.

### 1.3.6. File Structure (Feature-Sliced + Atomic Design)

```text
Source/Features/Daw/
├─ State/                  // ValueTree schema, ids, sample-rate constants
│  ├─ DawState.h
│  └─ DawClipModel.h
├─ Model/                  // pure model helpers (no JUCE UI)
│  ├─ DawClip.h
│  ├─ ChannelGroup.h
│  └─ MasterGridService.h/.cpp
├─ Projection/            // live deck → timeline bridge
│  ├─ LiveProjectionTimer.h/.cpp
│  └─ SourceModeReader.h
├─ Transform/             // samples ↔ beats ↔ pixels
│  └─ TimelineTransform.h/.cpp
└─ Ui/
   ├─ Atoms/              // ClipBlock, GridLine, RulerTick, Playhead
   ├─ Molecules/          // LaneView, ChannelGroupHeader, TimeRuler
   └─ Organisms/          // DawPanel (the docked top-of-app timeline)
```

`Source/Features/Daw/` may only `#include` public contracts of other feature
slices (deck source-position/source-mode accessors, master-clock snapshot,
waveform cache, library/stem ids). It must not reach into another slice's
`internal/`.

### 1.3.7. UI Design Language

All DAW UI complies with `DESIGN.md`: strict monochrome palette
(`#2d2d2d` / `#fdfdfd`), `Space Mono Regular`, 2-px solid borders, zero
border-radius, dithered patterns instead of gradients, pixel-art icons, tonal
layering for lane depth. Clip waveforms and grid lines use tonal layering, not
colour, to distinguish lanes and bar/beat divisions. Component specs
(timeline ruler, clip blocks, lane headers, playhead) must be checked against
`DESIGN.md` before each implementing PRD.

### 1.3.8. Audio-Thread Safety

This Epic adds **no DSP** and does not render audio. All work is message/UI
thread: model mutation, waveform drawing, coordinate math. The only contact
with the audio thread is **reading** existing per-deck atomics and the master
clock SeqLock snapshot — no new audio-thread code paths.

## 1.4. PRD Roadmap

Sequenced so each PRD compiles, runs, and is testable without the later ones.
Numbers are assigned by `generate_doc.sh` at drafting time; `TBD` until then.

- [x] PRD-0063: DAW State Schema & Non-Destructive Clip Model (`daw` ValueTree branch, `DawClip`, channel-group/lane structure)
- [x] PRD-0064: Master Grid Service & Master-Clock Reconciliation (read-only bars/beats grid from `MasterClockSnapshot`)
- [x] PRD-0065: Timeline Coordinate Transform (samples ↔ beats ↔ pixels) with zoom & scroll math
- [x] PRD-0066: DAW Panel Shell & Time Ruler (docked top-of-app organism, DESIGN.md-compliant, collapse/expand)
- [x] PRD-0067: Channel-Group & Three-Lane Layout (Original / Instrumental / Vocal lane headers and views)
- [x] PRD-0068: Clip Block Atom & Timeline Waveform Rendering (reuse PRD-0006 analysis cache)
- [x] PRD-0069: Live Deck Projection Bridge (LiveProjectionTimer growing live clips from deck source position + source mode)
- [x] PRD-0070: Live Playhead / Now-Line & Zoom/Scroll Interaction Polish
