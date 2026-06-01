---
status: Implemented
epic: EPIC-0009
depends-on:
  - PRD-0008
  - PRD-0027
  - PRD-0028
  - PRD-0064
  - PRD-0073
---

# 1. PRD-0074: Grid Alignment Resolver

## 1.1. Problem

The Clip Placement Engine (PRD-0073) opens a `DawClip` the instant a deck starts producing audio during recording, stamping it with `timelineStartSample = currentTimelinePosition`. That raw playhead value is musically naive: it lands wherever real time happened to be when the DJ pressed Play, which is almost never exactly on a grid line. If clips are written at raw positions, the captured arrangement is unquantised — every clip sits a few milliseconds off the bar, the channel grid lines drawn by EPIC-0008 no longer coincide with the audible beats, and any later edit (EPIC-0010) or export (EPIC-0012) inherits that drift.

The correct placement depends on whether the deck is actually locked to the master tempo. When the deck's BPM matches the master and it is phase-aligned (the DJ has SYNC engaged and the phase-lock controller has converged), the clip can be snapped hard to the channel grid — every beat in the clip will coincide with a grid line, producing a clean, quantised arrangement. But when the deck is playing at a foreign tempo (no sync, or a track whose BPM simply differs), force-snapping every beat to the grid would be a lie: the beats are not on the grid and never will be without time-stretching, which is explicitly out of scope for this Epic. In that case only the clip's *first captured downbeat* should be anchored to the nearest grid line, with the remainder of the clip placed free at its natural tempo.

EPIC-0009 §1.3.5 specifies this branch as pseudocode but no component owns it. Without a dedicated resolver, the Clip Placement Engine would have to inline tempo-comparison and phase logic it has no business knowing about, and — worse — it would risk inventing a *competing* definition of "in sync" that diverges from the one EPIC-0003 (PRD-0027 / PRD-0028) already established and tested. The decision must live in one small, pure, well-tested place that strictly reuses the existing sync vocabulary.

## 1.2. Objective

The system provides an `AlignmentResolver` such that:

- At clip **open** (and only at open), the Clip Placement Engine (PRD-0073) calls the resolver with the deck's current state and the master grid, and receives back a single `timelineStartSample` and an `AlignmentMode` (`GridAligned` or `FirstBeatAnchored`).
- When the deck's BPM matches the master tempo within tolerance **and** the deck is phase-aligned, the resolver returns `GridAligned`: `timelineStartSample = snap(playhead, gridLine)`, and the contract guarantees that grid lines under the clip coincide with the deck's beats.
- Otherwise the resolver returns `FirstBeatAnchored`: it derives the clip's first captured downbeat from the deck beatgrid (PRD-0008) and the capture source position, finds the nearest master grid line to that downbeat's timeline position, and returns that grid line as `timelineStartSample`. The remainder of the clip is placed free — no per-beat snapping is applied to the growing clip.
- The "tolerance" and "phase-aligned" predicates **reuse EPIC-0003's existing definitions verbatim** (PRD-0027's SYNC-latch / ratio-normalisation band and PRD-0028's `convergenceThreshold`). The resolver introduces no new sync notion, no new threshold constant, and no competing measure of alignment.
- The resolver is a pure message-thread function: given identical inputs it returns identical outputs, performs no audio-thread work, allocates nothing on any hot path, and reads only values already projected by EPIC-0008 / EPIC-0003 infrastructure.
- The alignment decision for a clip is computed **once, at open, and is immutable for the lifetime of that clip** — later BPM or phase changes never retro-align an already-open clip.

Time-stretching a clip to force a non-matching BPM onto the grid is explicitly **out of scope** (EPIC-0009 §1.2.2): non-matching tracks are first-beat-anchored and play at their own tempo.

## 1.3. Developer / Integration Flow

1. `AlignmentResolver` is added under `Source/Features/Daw/Recording/AlignmentResolver.h` / `AlignmentResolver.cpp`. It exposes a single pure entry point, e.g. `AlignmentResult resolve(const AlignmentInputs& in) const;`, where `AlignmentResult { int64 timelineStartSample; AlignmentMode mode; }` and `AlignmentMode { GridAligned, FirstBeatAnchored }`.
2. `AlignmentInputs` is a small POD assembled by the Clip Placement Engine (PRD-0073) at clip open from values already available on the message thread: `playheadSample` (current record playhead from the MasterGridService, PRD-0064), `deckBPM` and `masterBPM`, `isSynced` (PRD-0027's `IDs::isSynced` projection), `phaseOffsetBeats` (PRD-0028's published `phaseOffset` atomic, read once at open), the deck beatgrid `beatgridAnchor` / `beatgridInterval` (PRD-0008, also exposed as `DeckAudioState::beatgridAnchor` / `beatgridInterval`), the capture `sourceStartSample`, and the master grid origin + interval from the MasterGridService.
3. The resolver evaluates the EPIC-0009 §1.3.5 branch. The "match" predicate is `isSynced == true` **and** the BPM ratio `masterBPM / deckBPM` lies inside PRD-0027's normalisation band `[0.667, 1.5]` after folding (i.e. the deck is tempo-locked, not half/double-time mismatched) **and** the phase predicate `std::abs(phaseOffsetBeats) < convergenceThreshold` holds, where `convergenceThreshold` is imported from PRD-0028's named constant (`0.02` beats) — not redefined here.
4. **GridAligned path.** `snap(playhead, gridLine)` is computed against the MasterGridService grid as `gridOrigin + round((playhead − gridOrigin) / gridInterval) × gridInterval`. Because the deck is tempo-matched and phase-aligned, snapping the start to the grid is sufficient for *every* subsequent beat of the growing clip to coincide with a grid line; the resolver asserts (in a debug-only check) but does not enforce per-beat snapping at runtime.
5. **FirstBeatAnchored path.** The resolver derives the clip's first captured downbeat in source samples from the deck beatgrid: the first beat at or after `sourceStartSample` is `beatgridAnchor + ceil((sourceStartSample − beatgridAnchor) / beatgridInterval) × beatgridInterval` (clamped to ≥ `beatgridAnchor`). Its offset ahead of the capture point, `downbeatSourceOffset = firstDownbeatSource − sourceStartSample`, is projected onto the timeline as `downbeatTimeline = playheadSample + downbeatSourceOffset` (the deck plays at its own tempo, so source samples map 1:1 to timeline samples at clip open). The resolver then snaps **only that downbeat** to the nearest master grid line and back-computes the clip start: `anchor = nearestGrid(downbeatTimeline) − downbeatSourceOffset`. The returned `timelineStartSample = anchor`; no further snapping is applied as the clip grows.
6. The Clip Placement Engine writes the returned `timelineStartSample` and stores the returned `AlignmentMode` on the open clip's record (and into the `daw` `ValueTree` clip node) so downstream Epics know how the clip was placed. The engine does **not** re-call the resolver while the clip grows or on close.
7. A test file `Tests/AlignmentResolverTests.cpp` (registered in `Tests/TestRunner.cpp`) exercises both branches, the predicate boundaries, the no-beatgrid fallback, and immutability of the decision.

```text
Source/Features/Daw/Recording/
├- AlignmentResolver.h
└- AlignmentResolver.cpp
```

## 1.4. Acceptance Criteria

- [ ] `AlignmentResolver` is implemented in `Source/Features/Daw/Recording/AlignmentResolver.h` and `AlignmentResolver.cpp`, exposing one pure `resolve(const AlignmentInputs&) const` method returning `{ int64 timelineStartSample; AlignmentMode mode; }`.
- [ ] `AlignmentMode` is a two-value enum (`GridAligned`, `FirstBeatAnchored`) and is the only alignment classification the resolver emits.
- [ ] The match predicate returns `GridAligned` if and only if ALL hold: `isSynced == true`; the folded ratio `masterBPM / deckBPM` lies within PRD-0027's `[0.667, 1.5]` band; and `std::abs(phaseOffsetBeats) < convergenceThreshold`, where `convergenceThreshold` is the `0.02`-beat constant defined by PRD-0028 and imported, never re-declared with a different value.
- [ ] In the `GridAligned` case, `timelineStartSample = snap(playhead, gridLine)` is computed as `gridOrigin + round((playhead − gridOrigin) / gridInterval) × gridInterval` using the MasterGridService (PRD-0064) origin and interval.
- [ ] In the `FirstBeatAnchored` case, the resolver derives the first captured downbeat from `beatgridAnchor` + `beatgridInterval` (PRD-0008) relative to `sourceStartSample`, snaps **only** that downbeat to the nearest master grid line, and returns `timelineStartSample = nearestGrid(downbeatTimeline) − downbeatSourceOffset`.
- [ ] In the `FirstBeatAnchored` case, no per-beat snapping is encoded in the result: the resolver returns exactly one `timelineStartSample` and the engine grows the clip free of further snapping.
- [ ] When the deck has no analysed beatgrid (`beatgridInterval <= 0` or `beatgridAnchor` unset), the resolver returns `FirstBeatAnchored` with `timelineStartSample = playheadSample` (the raw record-playhead position), performing no division by a zero interval and no crash. See §1.5.5.
- [ ] The resolver reads `convergenceThreshold` and the `[0.667, 1.5]` band from the existing PRD-0027 / PRD-0028 definitions; a code-review check confirms it declares no new sync-tolerance or phase-threshold constant of its own.
- [ ] The resolver is a pure function of `AlignmentInputs`: identical inputs yield identical outputs, with no reads of global mutable state, no singletons, and no `ValueTree` access inside `resolve` (the caller assembles the POD inputs).
- [ ] The resolver performs no audio-thread work: it is only ever called on the message thread by the Clip Placement Engine at clip open, and contains no `processBlock` path. No `new`, `delete`, `std::string`, locks, or I/O occur inside `resolve`.
- [ ] The alignment decision is computed once at clip open; there is no API on `AlignmentResolver` to re-resolve or mutate an existing clip's placement, and the Clip Placement Engine does not call `resolve` again for that clip. See §1.5.7.
- [ ] `AlignmentMode` is persisted onto the open clip's record and its `daw` `ValueTree` node so later Epics can read how each clip was placed.
- [ ] `Tests/AlignmentResolverTests.cpp` exists and is registered in `Tests/TestRunner.cpp`, covering: a tempo-matched + phase-aligned input → `GridAligned` with start exactly on a grid line; a non-matching-BPM input → `FirstBeatAnchored` with the first downbeat on a grid line; a matched-BPM-but-not-phase-aligned input (`phaseOffsetBeats = 0.1`) → `FirstBeatAnchored`; a `phaseOffsetBeats` value exactly at `convergenceThreshold` → `FirstBeatAnchored` (strict inequality); a no-beatgrid input → `FirstBeatAnchored` at raw `playheadSample`; and an `isSynced == false` input at matching BPM → `FirstBeatAnchored`.
- [ ] No new audio-thread code, DSP block, or UI atom is introduced by this PRD. The resolver is pure placement arithmetic.

## 1.5. Grey Areas

### 1.5.1. Exact Tolerance Value (Reuse PRD-0027)

EPIC-0009 §1.3.5 references a "tolerance" on `|deckBPM − masterBPM|` but does not give a number, and a tempting mistake is to invent one (e.g. `±0.05` BPM).

**Resolution:** Reuse PRD-0027's existing notion rather than inventing a numeric BPM tolerance. PRD-0027 does **not** define an explicit `|deckBPM − masterBPM|` band; its operational definition of "tempo-matched" is the **SYNC latch** (`IDs::isSynced = true`), which forces `speedMultiplier = masterBPM / deckBPM` normalised into `[0.667×, 1.5×]`. The resolver therefore treats "BPM matches within tolerance" as: `isSynced == true` **and** the folded ratio lies inside PRD-0027's `[0.667, 1.5]` normalisation window (guarding against a half/double-time relationship the latch alone would happily lock). This is a faithful reuse — when SYNC is engaged the deck is, by EPIC-0003's own contract, playing at `masterBPM`, so the BPM equality is exact by construction. No new constant is introduced.

### 1.5.2. Definition of "Phase-Aligned" (Reuse PRD-0028)

"Phase-aligned" could be read as "SYNC is on" or "the phase meter looks close," both of which are vague.

**Resolution:** Reuse PRD-0028's canonical definition exactly: the deck is phase-aligned when its published `phaseOffset` (the `std::atomic<float>` written each `processBlock` by the continuous phase-lock controller) satisfies `std::abs(phaseOffsetBeats) < convergenceThreshold`, with `convergenceThreshold = 0.02` beats — the very dead-band PRD-0028 uses to decide the P-controller has converged. The resolver imports that constant; it does not re-declare it. A deck whose phase-lock has converged is, by PRD-0028's definition, phase-aligned, so this predicate is identical to "the EPIC-0003 phase-lock controller currently holds lock."

### 1.5.3. Deriving the "First Captured Downbeat"

The downbeat must be derived from the deck beatgrid plus the source position at which capture began; there are two candidate beats (the last beat at or before `sourceStartSample`, or the first at or after it).

**Resolution:** Use the first beat **at or after** `sourceStartSample`: `firstDownbeatSource = beatgridAnchor + ceil((sourceStartSample − beatgridAnchor) / beatgridInterval) × beatgridInterval`, clamped to ≥ `beatgridAnchor`. Anchoring the first *upcoming* beat (rather than a beat already behind the capture point) keeps the anchored beat inside the clip's captured region, so the grid line the DJ sees aligns with audio the clip actually contains. The offset `downbeatSourceOffset = firstDownbeatSource − sourceStartSample` is projected to the timeline 1:1 (the clip plays at its own tempo from the open instant), snapped to the nearest master grid line, and back-subtracted to yield the clip start.

### 1.5.4. Snap Granularity (Beat vs Bar) for the Grid-Aligned Case

The `GridAligned` snap could target the nearest beat line or the nearest bar (downbeat) line of the master grid.

**Resolution:** Snap to the **beat** grid line, not the bar. The MasterGridService grid (PRD-0064) is a beat grid; snapping to the nearest beat is the finest quantisation that still guarantees beat coincidence and minimises the displacement applied to the clip start (at most half a beat versus up to half a bar). Because the deck is tempo-matched and phase-aligned in this branch, snapping the start to a beat line makes every subsequent beat coincide with a grid line regardless of bar phase. Bar-level (phrase) quantisation is a higher-level musical concern deferred to a future editing Epic and is not imposed here.

### 1.5.5. Deck Has No Analysed Beatgrid

If the loaded track has not been beat-analysed (`beatgridInterval <= 0`), neither the match predicate nor the downbeat derivation can run meaningfully.

**Resolution:** Fall back to the **raw timeline position**: return `FirstBeatAnchored` with `timelineStartSample = playheadSample`, i.e. place the clip exactly where the record playhead was, with no snapping and no grid relationship. This is the only honest behaviour — without a beatgrid there is no beat to align — and it is non-destructive: when the track is later analysed (or the clip is re-placed manually in a future editing Epic), nothing in the captured crop is lost. The resolver performs no division by the zero interval, so there is no undefined behaviour.

### 1.5.6. What "Remainder Placed Free" Means for a Growing Clip

The clip is still open and growing when the resolver runs, so "remainder placed free" must be defined against a clip whose end is not yet known.

**Resolution:** "Remainder placed free" means the resolver contributes **exactly one** value — the clip's `timelineStartSample` — and nothing about how the clip grows. The Clip Placement Engine (PRD-0073) advances `sourceEndSample` / `timelineEndSample` from the deck's own source position at the deck's own tempo, applying **no** per-beat re-snapping. Concretely: only the start (or, in the anchored case, the first downbeat) touches the grid; every later sample of the clip maps source-to-timeline linearly at the deck's tempo. There is no second resolver call and no quantisation of the clip body.

### 1.5.7. Re-Evaluation if BPM Changes Mid-Clip

If the deck's BPM, sync state, or phase offset changes while the clip is still open, the original alignment decision could in principle be re-evaluated.

**Resolution:** The alignment is **decided at open and fixed for the lifetime of that clip.** The resolver is called exactly once, at clip open; its result is stamped onto the clip and never recomputed. A deck that was tempo-matched at open and later drifts out of sync does not retroactively un-align its already-placed clip, and vice versa — a clip opened at a foreign tempo stays first-beat-anchored even if the DJ engages SYNC a bar later. This keeps capture deterministic and replayable: the recorded arrangement reflects the alignment state that existed at the moment each clip began, which is exactly what the DJ heard. Changes in sync state after open simply influence the *next* clip the engine opens.
