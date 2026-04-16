---
status: Not Implemented
epic: EPIC-0001
---

# 1. PRD-0009: Key Detection

## 1.1. Problem

Harmonic mixing — blending tracks in compatible musical keys — is one of the core techniques that separates a professional DJ set from a playlist on shuffle. When two tracks clash harmonically, the result is an ugly, dissonant collision that audiences perceive as a mistake even if the beats are perfectly aligned. To select harmonically compatible tracks, the DJ must know each track's musical key. Manually determining key by ear is a specialist skill that requires music theory training and absolute or relative pitch — most DJs cannot do it reliably, and even those who can cannot do it fast enough across a library of thousands of tracks. Without automatic key detection, the DJ is forced to either guess, consult external analysis tools before every set, or avoid harmonic mixing entirely. Every professional DJ platform (Traktor, Serato, rekordbox, Mixed In Key) provides key detection as a core analysis feature because the cost of getting it wrong is immediately audible to the audience.

## 1.2. Objective

The system provides a background key detection engine per deck that:
- Analyzes the decoded PCM buffer on a background thread, determining the track's musical key using chromagram extraction and key-profile correlation (Krumhansl-Schmuckler algorithm).
- Detects all 24 possible keys (12 pitch classes x 2 modes: major and minor) with an accuracy target of 75% or above for exact key match on a reference dataset of mixed-genre commercial music.
- Stores the detected key in a canonical integer format (0-23) in the deck's ValueTree state, supporting conversion to Camelot notation (1A-12B), Open Key notation (1m-12d), and standard musical notation (e.g., C major, A minor).
- Displays the key in Camelot notation on the Track Info Display (PRD-0007), with a color derived from the Camelot wheel position for rapid visual harmonic matching.
- Computes a confidence score (0.0-1.0) and suppresses display for tracks with ambiguous or undetectable key (atonal, percussion-only, noise).
- Completes analysis of an 8-minute track in under 2 seconds on the background analysis thread pool shared with BPM analysis (PRD-0008).
- Persists the detected key and confidence in SQLite keyed by content hash, restoring instantly on subsequent loads.
- Reads embedded key metadata from ID3/Vorbis tags when present and uses it as a fallback when detection confidence is low.

## 1.3. User Flow

1. The user loads a track onto a deck. The system checks SQLite for cached key data (keyed by content hash).
2. If cached, the key value loads in under 100 ms. The Track Info Display shows the Camelot notation (e.g., `8A`) with its associated color.
3. If not cached, key analysis begins on the background analysis thread (shared with BPM analysis from PRD-0008). The key field displays `--` while analysis is in progress.
4. The analysis engine downmixes the PCM buffer to mono, selects a representative 60-second segment, computes a chromagram (12-bin pitch class energy distribution) using short-time Fourier transforms, averages the chroma vectors, and correlates the result against major and minor key profiles for all 12 roots.
5. The key with the highest correlation score is selected. A confidence score is computed from the ratio between the best and second-best correlation scores.
6. Analysis completes. The detected key (e.g., canonical value `15` mapping to A minor / `8A` Camelot) and confidence publish to the deck's ValueTree state. The Track Info Display updates from `--` to `8A` with the corresponding Camelot color.
7. The audio file has an embedded key tag (e.g., ID3v2 `TKEY` field reading `Am`). The system stores this as `embeddedKey` in the state tree. If the embedded key matches the detected key, no further action. If they disagree, the detected key takes display priority.
8. The DJ adjusts the pitch fader (changing `speedMultiplier`). The key display does not change — it reflects the track's original detected key. The actual sounding pitch shifts, but tracking that shift is deferred to the pitch/key-lock feature (future PRD).
9. The user loads a percussion-only track (e.g., a drum break). Analysis returns a confidence score below 0.4. The key field displays `--`, indicating no reliable key was detected. If the file has an embedded key tag, the embedded key is promoted to the display as a fallback.
10. The DJ has two decks loaded. Deck A shows `8A` and Deck B shows `8B`. The DJ knows these are harmonically compatible (same Camelot number, adjacent inner/outer wheel positions) and proceeds with a confident harmonic mix.
11. The DJ clicks the key display on Deck A and manually overrides the detected key by selecting `9A` from a dropdown of all 24 Camelot values. The override persists in SQLite.
12. The user ejects the track. The key field resets to `--` and the color indicator clears.
13. The user reloads the same track later. Cached key data restores instantly from SQLite; no re-analysis is needed. The manual override is preserved.

## 1.4. Acceptance Criteria

- [ ] Key analysis executes on the background analysis thread pool (shared with PRD-0008 BPM analysis), never on the UI or audio thread.
- [ ] Analysis downmixes the PCM buffer to mono before processing.
- [ ] Chromagram extraction uses STFT with a 4096-sample window, 2048-sample hop, and Hann windowing, producing 12 pitch-class energy bins per frame.
- [ ] Frequency-to-chroma mapping uses logarithmic binning aligned to A440 tuning (A4 = 440 Hz).
- [ ] A representative segment of at least 60 seconds starting from 15% into the track is analyzed. If the track is shorter than 90 seconds, the entire track is analyzed.
- [ ] The per-frame chroma vectors across the analyzed segment are averaged into a single 12-element chroma profile.
- [ ] The averaged chroma profile is correlated against all 24 key profiles (12 major + 12 minor) using the Krumhansl-Schmuckler key-profile weights.
- [ ] The key with the highest Pearson correlation coefficient is selected as the detected key.
- [ ] A confidence score is computed as `1.0 - (secondBestCorrelation / bestCorrelation)`. Values are clamped to the 0.0-1.0 range.
- [ ] When confidence is below 0.4, the key is set to an "unknown" sentinel value (-1) and displays `--`.
- [ ] When confidence is below 0.4 and a valid `embeddedKey` exists, the embedded key is promoted to the display as a fallback.
- [ ] The detected key is stored canonically as an integer 0-23 (0 = C major, 1 = C minor, 2 = C# major, 3 = C# minor, ..., 22 = B major, 23 = B minor) in the ValueTree property `detectedKey`.
- [ ] Confidence is stored as a `double` property `keyConfidence` (0.0-1.0).
- [ ] A `KeyUtils` utility class provides static conversion functions: `toCamelot(int canonicalKey)` returning a `juce::String` (e.g., `"8A"`), `toOpenKey(int)`, and `toMusicalNotation(int)`.
- [ ] Camelot mapping is correct for all 24 keys (1A = A-flat minor through 12B = E major).
- [ ] The Track Info Display (PRD-0007) reads `detectedKey` from the state tree and renders it in Camelot notation via `KeyUtils::toCamelot()`.
- [ ] Each Camelot position has an assigned display color defined in `KeyUtils::getCamelotColor(int)`. Colors follow a 12-hue wheel (30-degree HSL increments), with minor (A) variants at reduced saturation and major (B) variants at full saturation.
- [ ] The key text or a small color indicator adjacent to the key text in the Track Info Display uses the Camelot color.
- [ ] Key display does not change when `speedMultiplier` changes. The displayed key always reflects the original detected key of the loaded file.
- [ ] Embedded key metadata (ID3v2 `TKEY`, Vorbis `KEY` or `INITIALKEY` comment) is extracted during file loading (PRD-0003 pipeline) and stored as `embeddedKey` (canonical integer, or -1 if absent or unparseable) in the state tree.
- [ ] When both `embeddedKey` and `detectedKey` are present and agree, `detectedKey` is displayed. When they disagree, `detectedKey` takes display priority.
- [ ] Analysis of an 8-minute 44.1 kHz stereo track completes in under 2 seconds.
- [ ] Key data is cached in SQLite by content hash alongside BPM/beatgrid data. Cached load completes in under 100 ms.
- [ ] Manual key override: the user can click the key display and select a key from a dropdown of all 24 Camelot values. Manual selection is persisted in SQLite with a `manuallyAdjusted` flag.
- [ ] Auto-analysis does not overwrite a manually set key on reload.
- [ ] A reset option re-runs auto-analysis, discarding the manual override.
- [ ] Key analysis is cancellable on eject or new track load without resource leaks.
- [ ] Independent per-deck analysis with no cross-talk between decks.
- [ ] When no key data is available (pre-analysis or low confidence with no embedded fallback), the key field displays `--` and no color indicator is shown.
- [ ] Detected key is stored independently of `speedMultiplier`. The key value represents the original file's key at unity pitch.
- [ ] Key data is persisted with the analysis sample rate. No sample-rate conversion is needed for key data (unlike beatgrid anchors).
- [ ] All code resides under `Source/Features/KeyDetection/`. Dependencies (PCM buffer access, state tree, SQLite handle) passed via constructor injection. No singletons.

## 1.5. Grey Areas

1. **ID3 embedded key vs detected key (trust model).** Some DJs meticulously tag their libraries with correct keys using tools like Mixed In Key; others have files with incorrect or absent tags. Resolution: detected key takes display priority because it provides a consistent, self-verified result. The embedded key is stored in the state tree as `embeddedKey` for reference. If detection confidence is below 0.4 and a valid embedded key exists, the embedded key is promoted to the display as a fallback. A future Preferences PRD may add a user toggle to prefer embedded key over detected key globally.

2. **Key display when pitch is shifted (speedMultiplier != 1.0).** When the DJ changes pitch without key lock, the actual sounding key shifts. The display could reflect the transposed key or the original. Resolution: the key display always shows the original detected key of the file, matching Traktor, Serato, and rekordbox behavior. Tracking the sounding key requires integration with the pitch fader and key-lock system (future PRD). Showing a shifting key value would cause visual noise that harms more than it helps. A future "effective key" indicator can be added alongside the original key when the pitch/key-lock PRD is implemented.

3. **Camelot color scheme (specific palette).** There is no single universal Camelot color standard — different tools use different palettes. Resolution: adopt a 12-hue color wheel where each Camelot number (1-12) maps to an evenly spaced hue (30-degree increments on HSL), with the A (minor) variant at reduced saturation and the B (major) variant at full saturation. This provides clear visual differentiation without requiring the user to memorize an arbitrary color table. The palette is defined in `KeyUtils` and can be overridden in a future Preferences PRD.

4. **Accuracy target and validation methodology.** The 75% exact-key accuracy claim needs a verification approach. Resolution: accuracy is validated against a curated test set of 200+ tracks with known ground-truth keys (from professional key-tagging services or manual verification). This test set is maintained as a development-time asset, not shipped with the application. The 75% threshold is a minimum gate for the initial release. If accuracy falls below this, the algorithm is tuned (e.g., by substituting Temperley or Albrecht-Shanahan key profiles for Krumhansl-Schmuckler) before shipping.

5. **Major vs minor detection accuracy.** Distinguishing relative major/minor pairs (e.g., C major vs A minor) is the hardest part of key detection because they share identical pitch classes and differ only in emphasis. Resolution: accept that relative major/minor confusion will be the dominant error mode. On the Camelot wheel, relative major/minor pairs are harmonically compatible by definition (e.g., `8A` and `8B` are adjacent), so a major/minor misclassification still results in a compatible mix suggestion. The system prioritizes getting the correct Camelot number (pitch-class group) over the correct mode letter. This is explicitly documented so users understand that `8A` vs `8B` confusion is a soft error, not a hard failure.

6. **Key compatibility display (showing which keys mix well).** The Camelot wheel defines compatibility as same number, +/-1 number (same letter), or same number (opposite letter). Resolution: deferred to a future PRD. This PRD establishes the key data infrastructure. A key compatibility overlay (highlighting compatible decks, sorting library by compatible key) belongs to the Mixer or Library epics and depends on multi-deck state comparison out of scope here. The canonical integer storage format and `KeyUtils` conversion functions are designed to make compatibility computation trivial when the time comes.

7. **Analysis segment selection (full track vs representative portion).** Analyzing the entire track is thorough but slower; analyzing a segment is faster but risks missing key changes. Resolution: analyze a 60-second segment starting from 15% into the track to skip intros that are often percussion-only or ambient. If the track is shorter than 90 seconds, analyze the entire track. Key modulations within a single track are rare in DJ music and are not detected — the system returns the single dominant key. A `keyModulation` field is reserved in the schema for future multi-segment analysis but is not populated in this PRD.

8. **Tracks with no detectable key (atonal, percussion-only, sound effects).** These tracks should not show a misleading key value. Resolution: the confidence threshold of 0.4 handles this. Below that threshold, key displays `--` and no color indicator is shown. Downstream harmonic features treat a missing key as "compatible with anything" rather than "incompatible with everything," since a percussion track layered under any harmonic track will not clash.