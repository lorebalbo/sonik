---
status: Not Implemented
epic: EPIC-0001
---

# 1. PRD-0008: BPM and Beatgrid Analysis

## 1.1. Problem

A DJ's ability to mix tracks seamlessly depends on knowing each track's tempo and having a precise grid of beat positions aligned to that tempo. Without automatic BPM detection, the DJ is forced to tap-tempo manually for every track in a library of thousands. Without a beatgrid — a series of sample-accurate beat positions — every downstream beat-aware feature (quantize, auto-loop, beat jump, sync, phase meters) has no reference frame and cannot function. A beatgrid that is off by even 10 ms causes loops to click, quantized cue points to land between beats, and sync to pull tracks into audible drift.

## 1.2. Objective

The system provides a background BPM detection and beatgrid analysis engine per deck that:
- Analyzes the decoded PCM buffer on a background thread, detecting tempo with +/- 0.05 BPM accuracy for constant-tempo electronic music.
- Resolves BPM half/double ambiguity by preferring the 80-160 BPM range.
- Identifies the first downbeat position (beat 1 of a 4-beat bar) to anchor the beatgrid.
- Generates a fixed beatgrid (anchor sample + beat interval) for constant-tempo tracks.
- Completes analysis of an 8-minute track in under 3 seconds.
- Persists BPM and beatgrid data in SQLite keyed by content hash.
- Provides manual correction controls: BPM nudge, halve/double, beatgrid shift, and tap tempo.
- Exposes beat position data for waveform overlay, quantize, loop, beat jump, and sync features.

## 1.3. User Flow

1. The user loads a track. The system checks SQLite for cached BPM/beatgrid data.
2. If cached, BPM and grid load in under 100 ms. Beat grid lines appear on the waveform immediately.
3. If not cached, background analysis begins. BPM shows `--`, no grid lines visible.
4. Analysis detects tempo via spectral flux onset detection and autocorrelation. A downbeat anchor is identified.
5. Analysis completes. BPM (e.g., `126.03`) and beatgrid publish to the state tree. Grid lines render on the waveform aligned to kick transients.
6. The DJ inspects the grid. If lines are slightly off, they use grid shift nudge buttons (+/- 10 ms) to align them.
7. For a live-drummer track with tempo drift, the system flags "Variable BPM" and provides a best-effort fixed grid.
8. For a half-time track detected at double tempo, the DJ clicks "BPM /2" to halve the detected value.
9. For an ambient track with no beat, BPM shows `--`, no grid is generated, and beat-aware features degrade to time-based mode.
10. The DJ can override detection entirely via tap tempo (4+ taps) or manual BPM nudge (+/- 0.01).
11. All corrections persist in SQLite and are restored on subsequent loads.

## 1.4. Acceptance Criteria

- [ ] BPM analysis executes on a dedicated background thread, never on the UI or audio thread.
- [ ] Analysis downmixes stereo to mono before processing.
- [ ] Onset detection uses spectral flux (2048-sample FFT window, 512-sample hop, Hann window).
- [ ] Tempo estimation uses autocorrelation over a lag range of 60-200 BPM.
- [ ] BPM candidate within 80-160 BPM preferred when half/double ambiguity exists.
- [ ] BPM accuracy within +/- 0.05 BPM for constant-tempo tracks with clear percussion.
- [ ] BPM published to state tree as `double` property (`beatgridBpm`).
- [ ] Downbeat detection evaluates onset strength at beat-period offsets across the first 30 seconds.
- [ ] Beatgrid anchor published as `int64_t` (`beatgridAnchorSample`).
- [ ] Beatgrid stored as anchor + interval (`sampleRate * 60.0 / bpm`). Individual beats computed on demand, not stored as array.
- [ ] Analysis of an 8-minute 44.1 kHz track completes in under 3 seconds.
- [ ] Data cached in SQLite by content hash. Cached load under 100 ms.
- [ ] Confidence score (0.0-1.0) computed. Below 0.4: BPM = 0.0 (displays `--`), no grid generated.
- [ ] Variable-tempo flag set when inter-onset interval deviation exceeds 3% of mean. Fixed grid still generated from average BPM.
- [ ] Manual BPM nudge: +/- 0.01 per click, updates interval, anchor fixed.
- [ ] BPM x2 and /2 buttons with 30-300 BPM clamp.
- [ ] Tap tempo: 4+ taps within 3s each compute average interval, replace detected BPM, recompute grid.
- [ ] Grid shift nudge: +/- 10 ms per click (wraps within one beat interval).
- [ ] Reset button re-runs auto-analysis, discarding manual corrections.
- [ ] Manual corrections persisted in SQLite with `manuallyAdjusted` flag. Auto-analysis does not overwrite manual data on reload.
- [ ] `getBeatPositionForSample(int64_t)` returns nearest beat position and index in O(1).
- [ ] `getBeatsInRange(int64_t start, int64_t end)` returns all beat positions for waveform overlay.
- [ ] Beat phase computable as `(samplePos - anchor) % beatInterval / beatInterval` (0.0-1.0).
- [ ] Waveform detail view renders beat grid lines using `samplePositionToPixelX`. Downbeats (beat 1 of 4) visually stronger than other beats.
- [ ] Grid overlay maintains 60 fps rendering.
- [ ] Analysis cancellable on eject/new load without resource leaks.
- [ ] Independent per-deck analysis, no cross-talk.
- [ ] When no beatgrid exists, beat-aware features degrade gracefully (quantize disables, beat jump/loop fall back to time-based).
- [ ] Stored beatgrid independent of `speedMultiplier`. Effective BPM = `beatgridBpm * speedMultiplier`.
- [ ] Beatgrid data persisted with analysis sample rate. On load at different device rate, anchor and interval converted by rate ratio.
- [ ] All code under `Source/Features/BeatGrid/`. Dependencies via constructor injection.