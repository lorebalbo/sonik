---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0026
  - PRD-0027
  - PRD-0028
  - PRD-0063
---

# 1. PRD-0064: Master Grid Service & Master-Clock Reconciliation

## 1.1. Problem

EPIC-0008 introduces a DAW-style timeline whose ruler must show bar and beat grid lines, and whose every coordinate transform (PRD-0065) and clip placement depends on a single, authoritative answer to two questions: "how many samples is one beat?" and "where is beat 0?". The naive way to answer those questions inside the DAW would be to compute and store the DAW's own tempo and its own phase reference. That is exactly the failure EPIC-0003 was built to prevent: a second source of tempo truth that drifts away from the decks' actual playback.

The decks already share one coherent tempo and phase reference. PRD-0026 publishes a `MasterClockSnapshot` (`masterBPM`, `masterPhaseOriginSample`, `masterIsPlaying`) via a SeqLock, and PRD-0027 / PRD-0028 consume it for sync and phase correction. If the DAW grid were derived from anything other than that same snapshot, the timeline ruler would visibly disagree with where the audible beats land — bar lines would slide against the deck waveforms, and clips dropped on a "bar boundary" would not actually be on a bar. The grid would lie.

There is currently no message-thread service that reads the master clock and turns it into a DAW grid. Without it, the ruler (PRD-0066), the pixel mapping (PRD-0065), and clip snapping have no shared, correct definition of beats and bars, and each would be tempted to invent its own — reintroducing the drift problem one consumer at a time. The service must also behave sanely when the clock is dormant (no deck playing, `masterBPM` possibly stale or zero) so that an empty session shows a coherent ruler rather than a division-by-zero or a blank panel.

## 1.2. Objective

The system provides a read-only, message-thread `MasterGridService` (in `Source/Features/Daw/Model/MasterGridService.h/.cpp`) that:

- Reads the authoritative `MasterClockSnapshot` published by `MasterClockManager` (PRD-0026) via `MasterClockPublisher::read()` (SeqLock), and never publishes, mutates, or caches a competing tempo or phase value.
- Derives bar/beat grid lines for the ruler from `masterBPM` and the project sample rate, exposing each grid line as a sample position plus a classification (bar line vs beat line vs sub-beat line).
- Treats `masterPhaseOriginSample` as the grid's beat-0 reference, so the DAW grid and the deck beatgrids share exactly one phase origin.
- Exposes the project sample rate and pure `samplesToBeats` / `beatsToSamples` (and `samplesToBars` / `barForSample`) conversion helpers that every coordinate transform (PRD-0065) and clip-placement consumer calls, so there is exactly one conversion implementation in the DAW.
- Defines beats-per-bar as a single configurable compile-time constant (`kBeatsPerBar`, default `4`) consumed everywhere a bar boundary is computed; no consumer hard-codes `4`.
- Handles a dormant or zero-tempo clock gracefully: when `masterBPM <= 0` or `masterIsPlaying == false` with no usable tempo, the service reports a well-defined fallback grid (see §1.5.2) rather than dividing by zero or returning NaN.
- Does not drive, write, or own the master tempo. Master-tempo automation (a future writer through `MasterClockManager`) is EPIC-0011 and explicitly out of scope here.

Out of scope for this PRD: pixel/coordinate mapping (PRD-0065), the ruler UI and tick rendering (PRD-0066), and any path that writes tempo or phase back into the master clock.

## 1.3. Developer / Integration Flow

1. A `kBeatsPerBar` constant (default `4`) is defined in the DAW state layer (`Source/Features/Daw/State/DawState.h`) as `static constexpr int kBeatsPerBar = 4;`. Every bar-boundary computation in the DAW references this constant; no `4` literal appears in grid math.

2. `MasterGridService` is constructed with two injected dependencies (constructor injection, no singletons): a `MasterClockPublisher&` (PRD-0026, the read side of the SeqLock) and a project sample-rate provider that returns the current audio device sample rate (see §1.5.6). The service owns neither; it only reads.

3. On the message thread, a consumer (PRD-0065 transform or PRD-0066 ruler) calls `MasterGridService::sampleGrid(int64_t firstSample, int64_t lastSample)`. The service reads one coherent `MasterClockSnapshot` via `MasterClockPublisher::read(snapshot)`, computes `samplesPerBeat = sampleRate * 60.0 / masterBPM`, anchors beat 0 at `masterPhaseOriginSample`, and emits the ordered set of grid lines that fall within `[firstSample, lastSample]`, each tagged Bar / Beat / SubBeat.

4. The same snapshot read backs the pure conversion helpers: `beatsToSamples(double beats)` returns `masterPhaseOriginSample + beats * samplesPerBeat`; `samplesToBeats(int64_t sample)` returns `(sample - masterPhaseOriginSample) / samplesPerBeat`. `barForSample` and `samplesToBars` divide the beat result by `kBeatsPerBar`. These helpers take the sample rate and snapshot as already-read inputs (or read once internally per call — see §1.5.4) and perform no allocation, no locking beyond the SeqLock retry, and no I/O.

5. When `masterBPM <= 0` (dormant clock that has never had a non-zero BPM, or an explicitly zero tempo), the service substitutes the fallback tempo defined in §1.5.2 for grid-line generation and conversions, and reports `isFallbackGrid() == true` so the UI (PRD-0066) can render the ruler in a visually distinct "no master" state if it chooses. Conversions still return finite, monotonic values.

6. Sub-beat divisions are produced by a single `subBeatDivision` parameter on `sampleGrid` (default `1`, i.e. beats only; `2` = 1/2-beat lines, `4` = 1/4-beat lines). The service emits sub-beat lines but does not decide *which* division to display — that policy belongs to the ruler/zoom logic in PRD-0066 (see §1.5.5).

7. A test file under `Tests/` (`MasterGridServiceTests.cpp`) verifies: grid-line sample positions for a known BPM / sample rate / phase origin; that beat 0 lands exactly on `masterPhaseOriginSample`; round-trip identity of `beatsToSamples`/`samplesToBeats`; bar boundaries every `kBeatsPerBar` beats; fallback behaviour when `masterBPM == 0`; and that no grid line, bar index, or conversion ever returns NaN or infinity for any snapshot.

## 1.4. Acceptance Criteria

- [ ] `MasterGridService` is implemented in `Source/Features/Daw/Model/MasterGridService.h/.cpp` and is constructed via constructor injection with a `MasterClockPublisher&` and a sample-rate provider; it instantiates no singleton and owns no global mutable state.
- [ ] `MasterGridService` reads master tempo and phase exclusively through `MasterClockPublisher::read(MasterClockSnapshot&)`; it never calls `publish(...)` and never stores a tempo or phase value as its own authoritative copy.
- [ ] `kBeatsPerBar` is defined once as `static constexpr int kBeatsPerBar = 4` in `Source/Features/Daw/State/DawState.h`; every bar-boundary computation in `MasterGridService` references it, and no integer literal `4` is used for beats-per-bar in the grid math.
- [ ] `samplesPerBeat` is computed as `sampleRate * 60.0 / masterBPM` from the read snapshot; beat 0 is anchored at `masterPhaseOriginSample`.
- [ ] `beatsToSamples(double beats)` returns `masterPhaseOriginSample + llround(beats * samplesPerBeat)` and `samplesToBeats(int64_t sample)` returns `(sample - masterPhaseOriginSample) / samplesPerBeat`; the two are round-trip consistent (a beat value converted to samples and back differs by less than one beat of rounding error).
- [ ] `barForSample(int64_t sample)` and `samplesToBars(int64_t sample)` divide the beat result by `kBeatsPerBar`, so a sample exactly `kBeatsPerBar * samplesPerBeat` after the phase origin reports bar 1, beat 0.
- [ ] `sampleGrid(int64_t firstSample, int64_t lastSample, int subBeatDivision = 1)` returns grid lines only within `[firstSample, lastSample]`, in ascending sample order, each classified as Bar, Beat, or SubBeat; a line whose beat index is a multiple of `kBeatsPerBar` is classified Bar.
- [ ] When `masterBPM <= 0`, the service uses the §1.5.2 fallback tempo for all grid generation and conversions, returns `isFallbackGrid() == true`, and never returns NaN, infinity, or a division-by-zero result from any method.
- [ ] When `masterIsPlaying == false` but `masterBPM > 0` (paused master), the grid is generated normally from the held `masterBPM` and `masterPhaseOriginSample`; `isFallbackGrid()` is `false`.
- [ ] All `MasterGridService` methods run on the message thread; the service introduces no audio-thread code, performs no allocation in the conversion helpers, takes no lock other than the bounded SeqLock retry inside `MasterClockPublisher::read`, and performs no I/O.
- [ ] The service reads the project sample rate from the injected provider (current audio device rate); a sample-rate change is reflected on the next `sampleGrid` / conversion call without the service caching a stale rate beyond one call (see §1.5.6).
- [ ] `MasterGridService` lives in the DAW feature slice and only `#include`s public contracts of other slices (the master-clock snapshot/publisher header and the sample-rate provider contract); it does not reach into any other slice's `internal/`.
- [ ] `Tests/MasterGridServiceTests.cpp` covers: grid-line positions for a known BPM/sample-rate/phase-origin triple, beat-0 alignment to `masterPhaseOriginSample`, `beatsToSamples`/`samplesToBeats` round-trip, bar boundaries every `kBeatsPerBar` beats, the `masterBPM == 0` fallback path, and absence of NaN/infinity across a sweep of snapshots.
- [ ] No PRD-0065 pixel mapping, no PRD-0066 ruler UI, and no tempo-writing code path is introduced by this PRD.

## 1.5. Grey Areas

### 1.5.1. Beats-Per-Bar: Fixed 4/4 vs Configurable

The DAW grid needs a beats-per-bar value to know where bar lines fall. It could be hard-coded to `4` (4/4 is overwhelmingly the dominant time signature in DJ-oriented electronic music) or fully configurable per project / per track with arbitrary meters.

**Resolution:** Define beats-per-bar as a single configurable compile-time constant `kBeatsPerBar` defaulting to `4`, consumed everywhere a bar boundary is computed — but do not build per-track meter selection in this PRD. This gives the correct architectural seam (no `4` literals scattered through the code, one place to change) without paying for variable-meter UI, state, and per-region meter changes that no current consumer needs. The master clock itself publishes only a scalar BPM, not a time signature, so a true variable-meter system would require extending `MasterClockSnapshot` — a larger, EPIC-0011-era change. Until then, a project-wide configurable constant is honest about the current capability and trivially extensible.

### 1.5.2. Dormant Clock / `masterBPM == 0`: Blank Grid vs Default Tempo

When no deck is playing and the clock is dormant, `masterBPM` may hold a stale-but-non-zero value (PRD-0026 preserves the last BPM) or, in a fresh session that has never had a master, be `0`. The grid service must decide what to draw: a blank ruler, or a default-tempo ruler.

**Resolution:** Two distinct cases. (a) If `masterBPM > 0` (stale-but-valid held tempo from a previous master), draw the grid normally from that held BPM and phase origin — this matches PRD-0026's design intent that the held value is "a meaningful reference before the new track's BPM is analyzed", and keeps the ruler stable across a stop. (b) If `masterBPM <= 0` (never had a master, or an explicit zero), substitute a fallback tempo of `120.0 BPM` with `masterPhaseOriginSample` treated as `0`, and set `isFallbackGrid() == true`. A `120 BPM` fallback is chosen over a blank ruler because a blank timeline gives the DJ no spatial reference for dropping clips before a deck starts, and `120` is the conventional neutral default. Marking it as a fallback lets PRD-0066 render it dimmed or labelled "no master" so the DJ is not misled into thinking it reflects a real tempo. Crucially, the fallback never produces NaN or a division-by-zero; it is a valid, finite grid.

### 1.5.3. Phase-Origin Sign and Convention

`masterPhaseOriginSample` is the sample index of beat 0. Consumers must agree on the sign convention: is beat 0 at `masterPhaseOriginSample`, and are earlier beats negative? A subtle off-by-one in sign would shift the entire ruler against the audible beats.

**Resolution:** Beat 0 is exactly at `masterPhaseOriginSample`, with beats increasing for later samples and decreasing (negative beat indices) for earlier samples. The forward conversion is `sample = masterPhaseOriginSample + beats * samplesPerBeat`; the inverse is `beats = (sample - masterPhaseOriginSample) / samplesPerBeat`. This makes the phase origin the literal beatgrid anchor that PRD-0026 derives from the master deck's beatgrid, so the DAW grid and the deck beatgrid line up by construction. Negative beat indices are legal (a clip or grid line before the anchor) and `sampleGrid` happily emits bar/beat lines with negative indices when `firstSample < masterPhaseOriginSample`; bar classification uses a floor-based modulo so that bar boundaries remain correct on both sides of the origin. No consumer is permitted to invent its own offset; all must route through these helpers.

### 1.5.4. SeqLock Sampling Cadence: On-Demand vs Cached-Per-Frame

Every grid query and conversion needs a `MasterClockSnapshot`. The service could read the SeqLock once per public call (simple, always fresh) or cache one snapshot per UI frame and serve all that frame's queries from the cache (fewer reads, guaranteed intra-frame consistency).

**Resolution:** Read on demand per public call, with one refinement: within a single `sampleGrid` invocation the snapshot is read exactly once at the top and reused for every line in that call, so all lines in one grid query are mutually coherent. Across separate calls in the same frame, a fresh read is taken each time. The SeqLock read is cheap (a handful of atomic loads, single-iteration under no contention per PRD-0026), so caching per frame would add lifecycle complexity (when is "a frame"? who invalidates?) for negligible gain. The only consistency guarantee that matters — that all grid lines and conversions used to lay out one ruler pass agree — is provided by reading once per `sampleGrid` call and by PRD-0065 reading the snapshot-derived `samplesPerBeat` once for its transform. If a future profiler shows the per-call reads are hot, a per-frame cache can be added behind the same method signatures without changing any consumer.

### 1.5.5. Sub-Beat Divisions: Who Decides 1/4 vs 1/8

The ruler can show beat lines only, or finer sub-beat divisions (1/2, 1/4, 1/8 beat) that appear as the user zooms in. The question is whether `MasterGridService` decides the division or merely produces whatever division it is asked for.

**Resolution:** `MasterGridService` is a mechanism, not a policy: `sampleGrid` takes a `subBeatDivision` parameter (default `1` = beats only; `2`, `4`, `8` = progressively finer) and emits exactly the requested lines, classifying multiples of `kBeatsPerBar` as Bar, integer beats as Beat, and the rest as SubBeat. The *decision* of which division to request at a given zoom level belongs to the ruler/zoom logic in PRD-0066, which has the pixels-per-beat information needed to avoid an unreadably dense ruler. Keeping the policy out of the grid service keeps this PRD purely about correct sample positions and lets the zoom heuristics evolve in PRD-0066 without touching grid math.

### 1.5.6. Project Sample Rate Source and Device-Rate Changes

The samples↔beats conversion needs a sample rate. It could be a fixed constant (e.g. `44100`) baked into the DAW, or the live audio device sample rate (which can change if the user switches output devices or the device renegotiates).

**Resolution:** Use the live audio device sample rate, supplied through an injected sample-rate provider, not a baked-in constant. The whole point of the grid is to agree with the audible deck playback, and the decks render at the device rate; a fixed `44100` would desynchronise the grid from a `48 kHz` device. The provider returns the current rate on each query, so a device-rate change is picked up on the next `sampleGrid` / conversion call — the service deliberately does not cache the rate across calls. If the device reports an invalid or zero rate during a transition, the service falls back to `44100.0` for that call (alongside the §1.5.2 tempo fallback if applicable) so conversions stay finite. Because `masterPhaseOriginSample` is itself expressed in samples at the same device rate (it comes from the deck audio sources), a rate change rescales grid and phase consistently and no separate reconciliation is needed; the grid simply recomputes from the new rate on the next call.
