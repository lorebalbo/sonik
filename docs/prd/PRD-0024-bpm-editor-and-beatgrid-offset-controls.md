---
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0008
---

# 1. PRD-0024: BPM Editor and Beatgrid Offset Controls

## 1.1. Problem

The Grid panel in the Controller Widget currently displays a read-only BPM value and four non-functional nudge buttons (`<<`, `<`, `>`, `>>`) with no hover feedback. A DJ frequently needs to manually correct an incorrectly detected BPM — for example when the analyzer halves the tempo of a 140 BPM track to 70 BPM — and must be able to fine-tune the beatgrid phase so that waveform beat markers align precisely with kick transients. Without an editable BPM field and working offset controls, every downstream beat-aware feature (quantize, auto-loop, beat jump, sync) is unreliable on any misanalyzed track, which is unacceptable in a live performance context.

## 1.2. Objective

The system provides a fully operational manual beatgrid correction UI within the Grid panel such that:
- The user can type a BPM value directly into the BPM field and commit it with a `SAVE` button, recomputing and persisting the beatgrid immediately.
- The four nudge buttons shift the beatgrid anchor by a fixed fine or coarse offset in real time, with each click producing an immediate visual update on the waveform overlay.
- All manual corrections are persisted to SQLite keyed by content hash and survive application restarts.
- All interactive elements display a hover state for clear affordance.
- The `SAVE` button matches the visual style of the other buttons already present in the Grid panel.

## 1.3. User Flow

1. The user loads a track. The Grid panel shows the auto-detected BPM in the BPM field (e.g., `126.03`). The four nudge buttons are visible.
2. The user notices the BPM is wrong (e.g., detected at `63.01`). They click the BPM field, which activates editing mode: a cursor appears and the current value is selected.
3. The user types `126.03` and either presses Enter or clicks the `SAVE` button.
4. The system validates the input (20–300 BPM, numeric). The beatgrid is recomputed (`beatIntervalSamples = sampleRate * 60.0 / newBpm`), `manuallyAdjusted` is set to `true`, and the data is saved to SQLite. The waveform overlay re-renders immediately with the corrected grid.
5. The user notices the beat markers are offset slightly from the kick transients. They hover over the `<` button — it changes to a pointing-hand cursor and its fill lightens to the hover color. They click it: the entire grid shifts backward by a fine offset (≈ 10 ms). The waveform overlay updates in real time.
6. After each nudge click, the new anchor is automatically persisted to SQLite (no explicit save action required).
7. The user uses `<<` for a larger backward shift (≈ 50 ms) to correct a more significant phase error, then fine-tunes with `<` / `>`.
8. The user closes the application and reopens it. They reload the same track. The corrected BPM and grid phase are restored from SQLite exactly as left.
9. The user loads a different track that was not previously loaded. The BPM field shows the freshly detected BPM; nudge buttons are enabled and ready.
10. The user ejects a track. The BPM field clears and all nudge buttons become disabled (non-interactive, no hover feedback).

### 1.3.1. Edge Cases

- If the user types an out-of-range or non-numeric value and presses Save, no state change occurs and the field reverts to the last valid BPM.
- If no track is loaded (BeatGrid child absent or BPM = 0.0), the BPM field is read-only and all nudge buttons are disabled.
- Nudge wraps `anchorSample` within `[0, beatIntervalSamples)` to keep the anchor within one beat period.
- Manual corrections written to SQLite do not get overwritten by re-analysis on subsequent loads (`manuallyAdjusted = true` causes the cached data to be used and analysis to be skipped).

## 1.4. Acceptance Criteria

- [ ] The BPM value box in the GRID tab is replaced with an editable `juce::TextEditor` that visually matches the existing read-only box (same size: 60 × 23 px, same font, same `#F9F9F9` background, `#2D2D2D` text, zero border-radius, 2 px `#2D2D2D` border).
- [ ] The BPM `TextEditor` accepts only numeric input with up to 2 decimal places. Non-numeric characters are rejected during typing.
- [ ] A `SAVE` button is rendered immediately adjacent to the BPM field (50 × 46 px, 2 px `#2D2D2D` border, zero border-radius, `#F9F9F9` fill with `#2D2D2D` text at rest; `#2D2D2D` fill with `#F9F9F9` text on hover/press), styled identically to the `SET` and `DEL` buttons already present.
- [ ] Pressing Enter in the BPM `TextEditor` or clicking `SAVE` calls `DeckShellComponent::handleBpmSave(double newBpm)`.
- [ ] `handleBpmSave` validates the input is in the range [20.0, 300.0]. If invalid, the field reverts to the previously stored BPM and no state change occurs.
- [ ] On valid save: `IDs::bpm` is updated in the `IDs::BeatGrid` ValueTree child, `IDs::beatIntervalSamples` is recomputed as `analysisSampleRate * 60.0 / newBpm`, `IDs::manuallyAdjusted` is set to `true`, and `db.saveTrackData(...)` is called to persist the new beatgrid JSON to SQLite.
- [ ] The waveform overlay re-renders within one UI repaint cycle after `handleBpmSave` completes.
- [ ] Fine nudge buttons (`<` and `>`): each click shifts `anchorSample` by ±round(analysisSampleRate * 0.010) samples (≈ 10 ms), scaled to the track's actual sample rate.
- [ ] Coarse nudge buttons (`<<` and `>>`): each click shifts `anchorSample` by ±round(analysisSampleRate * 0.050) samples (≈ 50 ms), scaled to the track's actual sample rate.
- [ ] After each nudge: `IDs::anchorSample` is updated in the `IDs::BeatGrid` ValueTree child, `IDs::manuallyAdjusted` is set to `true`, and `db.saveTrackData(...)` persists the result.
- [ ] Nudge wraps: if the shifted `anchorSample` exceeds `beatIntervalSamples`, it wraps modulo `beatIntervalSamples`. If it goes below 0, it wraps to `beatIntervalSamples + anchorSample`.
- [ ] All four nudge buttons and the `SAVE` button display a hover state: fill transitions to `#E2E2E2` on `mouseEnter`, reverts to `#F9F9F9` on `mouseExit`.
- [ ] The cursor changes to `juce::MouseCursor::PointingHandCursor` when hovering over any interactive button in the Grid panel.
- [ ] When no track is loaded, the BPM `TextEditor` is set to read-only (non-editable), and all five buttons (SAVE, `<<`, `<`, `>`, `>>`) are disabled and do not respond to mouse events or render hover states.
- [ ] The `SET` and `DEL` buttons are not modified by this PRD.
- [ ] No audio thread code is modified. All ValueTree writes occur on the message thread.
- [ ] All changes are confined to `Source/Features/Deck/UI/ControllerWidget.*` and `Source/Features/Deck/UI/DeckShellComponent.*`.

## 1.5. Grey Areas

1. **Nudge offset units.** PRD-0008 specified "+/- 10 ms per click" for the fine nudge. The current `handleGridNudge` implementation uses `beatInterval / 8` (≈ 62 ms at 120 BPM), which varies with tempo. Resolution: use a fixed millisecond-based offset (10 ms fine, 50 ms coarse) scaled to the track's sample rate, as specified above. This gives predictable, tempo-independent control resolution — important for correcting sub-beat phase errors regardless of the track's BPM.

2. **BPM field Enter key vs. explicit Save button.** Some users will expect Enter to commit; others may accidentally press Enter mid-edit. Resolution: both Enter and the `SAVE` button commit. If the user presses Escape, the field reverts to the previously stored BPM without saving.