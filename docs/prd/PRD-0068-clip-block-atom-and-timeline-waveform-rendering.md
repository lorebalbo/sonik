---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0006
  - PRD-0065
  - PRD-0067
---

# 1. PRD-0068: Clip Block Atom & Timeline Waveform Rendering

## 1.1. Problem

The DAW timeline foundation is, by the time this PRD is reached, a canvas with everything except the thing the DJ actually wants to look at: the audio. The state schema and non-destructive clip model (PRD-0063) define a `DawClip` as a crop window `[sourceStartSample, sourceEndSample]` into a source, anchored on the timeline at `timelineStartSample`. The coordinate transform (PRD-0065) converts samples ↔ beats ↔ pixels with the active zoom and scroll applied. The channel-group three-lane layout (PRD-0067) draws the `Original`, `Instrumental`, and `Vocal` lanes with their headers and lane backgrounds. But no clip is ever drawn. A lane is an empty tonal rectangle, and a `DawClip` in the `daw` ValueTree branch has no visual representation at all.

The clip is the atomic visual unit of the arrangement. It must render as a discrete rectangular block placed and sized by the `TimelineTransform`, and inside that block it must show the source waveform for exactly the cropped sub-range the clip references — not the whole track, and not a freshly analysed copy of it. EPIC-0008 is explicit (§1.2.1) that timeline clip waveform rendering must **reuse the per-track waveform analysis cache from PRD-0006** with no new analysis pass. Re-analysing audio for the timeline would duplicate the 256-spp mipmap pipeline, double memory and CPU, and risk the timeline waveform disagreeing with the deck waveform for the same file. The DJ would see two different pictures of the same audio.

There is also a source-resolution problem unique to the timeline. The deck-side waveform system (PRD-0006) keys its in-memory cache by **deck id** (`WaveformManager::getWaveformData (deckId)`) and its SQLite cache by **content hash**. A `DawClip` references a `sourceFileId` (a library track id or a stem-cache key from EPIC-0002/EPIC-0004), not a deck and not a deck's currently-loaded file. A clip can outlive the deck that created it, reference a stem the deck is not currently playing, or sit on the `Instrumental` / `Vocal` lane while the deck plays the original. The clip renderer therefore needs to resolve `sourceFileId` → content hash → cached `WaveformData` independently of which deck (if any) currently holds that file. And it must degrade gracefully when that resolution yields nothing yet — a clip whose source has not been analysed must still draw as a recognisable block, not vanish.

Finally, `DESIGN.md` has no clip specification. It specifies waveforms (pattern density / dithering, never frequency colour), buttons (2-px solid `#2d2d2d` border, zero radius, fill inversion), and tonal layering for depth — but it never describes a timeline clip. This PRD must derive the clip's visual contract from those existing rules rather than inventing a new colour or shape language.

## 1.2. Objective

The system provides a `ClipBlock` UI atom that renders a single `DawClip` on its lane such that:

- The block is a rectangle spanning the timeline range `[timelineStartSample, timelineStartSample + (sourceEndSample - sourceStartSample)]`, with its left edge and width computed exclusively through `TimelineTransform` (PRD-0065) so it tracks zoom and scroll identically to the grid, ruler, and (future) playhead. The clip length is 1:1 with the crop length (no time-stretch in this Epic, per EPIC-0008 §1.3.1).
- Inside the block, the source waveform for the crop window `[sourceStartSample, sourceEndSample]` is rendered by **reusing the existing PRD-0006 analysis cache** — the `WaveformData` mipmap (`levels[]` at 256-spp base, six reduction tiers) resolved by `sourceFileId`. No new analysis pass, no second analyzer instance, no re-decode of the source file.
- The waveform is drawn as a **sub-range** of the cached full-track analysis: the renderer maps the crop window to mipmap point indices (offsetting by `sourceStartSample`) and draws only those points, scaled to the block's pixel width.
- The renderer selects the appropriate PRD-0006 mipmap tier (via `WaveformData::getBestLevel`) for the current samples-per-pixel so that zoomed-out clips read from coarser tiers and never over-draw (consistent with PRD-0006's max-4-points-per-pixel rule).
- The waveform honours `DESIGN.md`: strictly monochrome (`#2d2d2d` on `#fdfdfd`), drawn as a peak/RMS envelope using **pattern density / dithering**, never the three-band frequency colouring that PRD-0006 stores (the energy fields exist in `WaveformPoint` but are deliberately ignored here).
- The `Instrumental` and `Vocal` lanes draw the corresponding **stem** waveform (resolved from the stem-cache analysis) when available; the `Original` lane draws the original-file waveform. Lane-to-source mapping is read from the clip's `laneId` / `sourceFileId`, not decided here.
- A clip whose source `WaveformData` is not yet available (analysis pending, stem not yet separated, cache miss) renders as a placeholder: the bordered block still occupies its correct timeline position and width, filled with a neutral dithered pattern, so the arrangement structure is legible before audio is.
- The block has a `2-px` solid `#2d2d2d` border, zero border-radius, and uses tonal layering so an individual clip is visually distinct from its lane background and from an adjacent clip on the same lane.
- All rendering happens on the message/UI thread. No DSP, no new analysis, no audio-thread contact; the renderer only **reads** cached `WaveformData` and the clip's ValueTree properties.

## 1.3. Developer / Integration Flow

1. A new atom `Source/Features/Daw/Ui/Atoms/ClipBlock.h/.cpp` is added, a `juce::Component` subclass. It is constructed with (a) a reference to the clip's `juce::ValueTree` node (or a lightweight `DawClip` value resolved from it), (b) the `TimelineTransform` (PRD-0065), and (c) a waveform-source accessor that resolves a `sourceFileId` to a cached `WaveformData::Ptr` (see step 3).
2. On `resized()` / transform change, `ClipBlock` computes its bounds from the transform: left `x = transform.sampleToPixel (timelineStartSample)`, width `= transform.sampleToPixel (timelineStartSample + cropLength) - x`, where `cropLength = sourceEndSample - sourceStartSample`. The owning `LaneView` (PRD-0067) positions the block vertically within the lane; `ClipBlock` only owns horizontal placement and its own painting.
3. A small read-only accessor — `ClipWaveformSource` (a thin interface, or a `std::function<WaveformData::Ptr (const juce::String& sourceFileId)>` injected by constructor) — resolves `sourceFileId` to a cached `WaveformData::Ptr`. For original-file sources it resolves the library track's content hash and returns the PRD-0006 cached data (SQLite hash-keyed cache, deserialised via `WaveformData::deserialize`); for stem sources it resolves the EPIC-0002/EPIC-0004 stem-cache key to its analysed data. This accessor reuses PRD-0006's cache; it does **not** trigger `WaveformAnalyzer::analyze`. A cache miss returns `nullptr`.
4. In `paint()`, `ClipBlock` first fills the block background (tonal layer distinct from the lane) and strokes the `2-px` `#2d2d2d` border with zero radius. If `WaveformData` is available, it computes the samples-per-pixel from the transform, picks the mipmap tier via `getBestLevel`, maps the crop window `[sourceStartSample, sourceEndSample]` to that tier's point-index range (offset by `sourceStartSample`), and draws the peak/RMS envelope for that sub-range using monochrome dithering/pattern density. If `WaveformData` is `nullptr`, it draws the pending placeholder fill instead.
5. The waveform is drawn mono-summed (peak = max of L/R, RMS = quadratic mean), matching PRD-0006's display contract. The `energyLow/Mid/High` fields on `WaveformPoint` are not read; no colour is applied beyond the monochrome ink.
6. `ClipBlock` observes the clip's ValueTree node (Observer pattern, JUCE Listener) so that property changes to `sourceStartSample` / `sourceEndSample` / `timelineStartSample` / `gainDb` repaint the block. (Editing those values is a later Epic; observing them now keeps the atom correct when the live-projection bridge of PRD-0069 mutates them.)
7. A new test file `Tests/ClipBlockTests.cpp` constructs a `ClipBlock` against a synthetic `TimelineTransform` and a stub `ClipWaveformSource`, and asserts: correct bounds for a given crop/placement; correct mipmap-tier selection and crop-window point-index mapping at several zoom levels; placeholder rendering path taken on cache miss; and that no analysis is triggered (the stub records calls and expects zero analyzer invocations).

## 1.4. Acceptance Criteria

- [ ] A new atom exists at `Source/Features/Daw/Ui/Atoms/ClipBlock.h/.cpp` as a `juce::Component` subclass, residing entirely within the `Source/Features/Daw/` slice.
- [ ] `ClipBlock` computes its left edge and width exclusively via the PRD-0065 `TimelineTransform`; changing the timeline zoom or scroll moves and resizes the block consistently with the grid and ruler.
- [ ] The block spans `[timelineStartSample, timelineStartSample + (sourceEndSample - sourceStartSample)]`; the on-timeline length equals the crop length 1:1 (no time-stretch).
- [ ] The clip waveform is rendered by reusing the existing PRD-0006 `WaveformData` cache resolved by `sourceFileId`; no new `WaveformAnalyzer` analysis pass is triggered by this PRD (verified by a test stub asserting zero analyzer invocations).
- [ ] The waveform drawn inside the block is the **sub-range** of the cached full-track analysis corresponding to `[sourceStartSample, sourceEndSample]`, offset by `sourceStartSample`, not the whole-track waveform.
- [ ] The renderer selects the mipmap tier via `WaveformData::getBestLevel` for the current samples-per-pixel and never over-draws (≤ 4 points per pixel), consistent with PRD-0006.
- [ ] The waveform is strictly monochrome (`#2d2d2d` ink on `#fdfdfd`) drawn as a peak/RMS envelope using dithering / pattern density; the `WaveformPoint` `energyLow/Mid/High` frequency fields are not used and no frequency colouring is applied.
- [ ] The block has a `2-px` solid `#2d2d2d` border and zero border-radius, per `DESIGN.md`.
- [ ] Tonal layering distinguishes the clip from its lane background and from an adjacent clip on the same lane.
- [ ] On the `Original` lane the original-file waveform is drawn; on the `Instrumental` and `Vocal` lanes the corresponding stem waveform is drawn when available, resolved from the stem-cache analysis.
- [ ] When the source `WaveformData` is unavailable (analysis pending, stem not yet separated, or cache miss), the block still renders at its correct position and width with a neutral dithered placeholder fill; it never disappears or throws.
- [ ] `ClipBlock` observes its clip ValueTree node and repaints on changes to `sourceStartSample`, `sourceEndSample`, `timelineStartSample`, and `gainDb`.
- [ ] All rendering occurs on the message/UI thread. This PRD adds no DSP and no audio-thread code; it only reads cached `WaveformData` and ValueTree properties, performs no allocations on the audio thread, takes no locks visible to the audio thread, and performs no I/O on the audio thread.
- [ ] At least one test in `Tests/ClipBlockTests.cpp` verifies bounds computation, mipmap-tier selection, crop-window point-index mapping at multiple zoom levels, and the placeholder path on cache miss.
- [ ] Clip selection, hover, and drag affordances are NOT implemented (deferred to EPIC-0010); the atom exposes no edit interaction in this PRD.

## 1.5. Grey Areas

### 1.5.1. Waveform Source for the Stem Lanes

The `Original` lane has an obvious source: the library track's file, whose `WaveformData` PRD-0006 already analyses and caches by content hash. The `Instrumental` and `Vocal` lanes are less obvious — does a per-stem `WaveformData` analysis already exist, and if so under what key?

**Resolution:** The stem lanes draw from per-stem `WaveformData` resolved by the stem-cache key (EPIC-0002 separation output + EPIC-0004 stem cache), reusing the same PRD-0006 mipmap format. If, at the time this PRD is implemented, the stem-separation pipeline does not yet publish per-stem `WaveformData` into the shared cache, the stem lanes fall back to the placeholder path (§1.5.4) rather than re-analysing the stem here. Triggering stem analysis is out of scope: this PRD only *consumes* a cache. A follow-up under EPIC-0002 (or PRD-0069's projection wiring) is responsible for ensuring stem waveforms are analysed and cached under a key the `ClipWaveformSource` accessor can resolve. This keeps the single-analysis-pipeline invariant intact and avoids a second, divergent stem-waveform analyzer.

### 1.5.2. Rendering Performance at High Zoom-Out

At maximum zoom-out a multi-minute clip may occupy only a few hundred pixels, while its base-tier analysis holds tens of thousands of points. Naively drawing every base point would over-draw catastrophically and stutter the timeline.

**Resolution:** Reuse PRD-0006's downsampled mipmap tiers exactly as the deck detail waveform does. `ClipBlock` computes samples-per-pixel from the `TimelineTransform`, calls `WaveformData::getBestLevel`, and draws from that coarser tier so the number of drawn points is bounded (≤ 4 per pixel). No new downsampling is implemented — the six-tier mipmap PRD-0006 already generates is the performance mechanism. This is the same discipline PRD-0006 applies to the deck waveform and is why reusing its cache (rather than a bespoke timeline analysis) is mandated by the Epic.

### 1.5.3. Crop-Window Mapping into the Cached Full-Track Analysis

The cached `WaveformData` describes the whole source file, but the clip shows only `[sourceStartSample, sourceEndSample]`. The renderer must map that sample window into the chosen tier's point array.

**Resolution:** For the chosen mipmap `level`, the tier's samples-per-point is `baseSamplesPerPoint * 2^level` (256 × 2^level). The crop window maps to point indices `[sourceStartSample / spp, sourceEndSample / spp]`; the renderer draws only that slice, scaled across the block's pixel width. This is a pure offset-and-slice into existing data — no resampling and no new analysis. Edge points are clamped to the tier's valid range; a crop that exceeds the analysed length (e.g. a source still being analysed progressively) draws the available prefix and placeholder-fills the remainder.

### 1.5.4. Placeholder When Analysis Is Pending

A clip can exist before its source waveform is cached — the original is mid-analysis, or a stem has not been separated yet. The block must remain structurally present.

**Resolution:** When `ClipWaveformSource` returns `nullptr`, `ClipBlock` draws the full bordered block at its correct timeline position and width, filled with a neutral monochrome dithered pattern (visually distinct from a real waveform — it reads as "pending", not as silence). No progress text or spinner is drawn (consistent with PRD-0006, which shows no progress indicator on cache hit and renders progressively otherwise). If PRD-0006's progressive-analysis ValueTree signal is observable for the source, `ClipBlock` may repaint as points become available; if not, it repaints when the completed `WaveformData` first resolves. The placeholder is never an empty or transparent region — the arrangement structure is always legible.

### 1.5.5. Clip Selection and Visual Affordance

A timeline clip in mature DAWs has selection highlight, hover state, and drag handles. None of those belong to this PRD.

**Resolution:** Selection, hover, drag handles, trim handles, and any edit affordance are explicitly deferred to EPIC-0010 (non-destructive clip editing). This PRD renders a static, read-only block. The atom is designed so a later Epic can add an interaction/selection layer on top without changing the rendering contract — but no `mouseDown`, selection state, or highlight styling is introduced here. Stating this prevents scope creep into editing.

### 1.5.6. Tonal Treatment to Separate Adjacent Clips on the Same Lane

Two clips abutting on the same lane (which becomes common once PRD-0069's live projection and EPIC-0009's capture split a take into segments) must read as two blocks, not one. `DESIGN.md` forbids colour and gradients.

**Resolution:** Use tonal layering plus the mandatory `2-px` `#2d2d2d` border to delimit each clip. Each clip is a slightly distinct tonal layer above the lane background, and the always-present 2-px border on every block guarantees a visible seam between adjacent clips even when their fills are identical. Where two clips are immediately adjacent, their touching borders form a clear `4-px`-reading divider (two 2-px borders). No alternating colour, no gradient, no rounded corner — the separation is achieved entirely with border and tonal depth, fully within `DESIGN.md`.

### 1.5.7. DESIGN.md Has No Clip Specification

`DESIGN.md` specifies waveforms, buttons, knobs, faders, and tonal layering, but never a timeline clip block. There is no authoritative spec to copy.

**Resolution:** Derive the clip's visual contract from the two existing rules it most resembles: the waveform spec (monochrome, dithering / pattern density, no frequency colour) governs the clip's interior, and the button spec (`2-px` solid `#2d2d2d` border, zero radius, fill inversion, tonal layering) governs the clip's frame and ground. The clip is, visually, "a bordered button-style container whose fill is a cropped waveform." No new colour, font, or radius is introduced. This is stated explicitly so a future `DESIGN.md` revision can formalise a clip section without contradicting what is built here.
