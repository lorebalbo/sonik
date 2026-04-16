---
name: "PRD-0012: Cue Points and Hot Cues"
status: Not Implemented
epic: EPIC-0001
depends-on:
  - PRD-0001
  - PRD-0004
  - PRD-0006
  - PRD-0008
---

# 1. PRD-0012: Cue Points and Hot Cues

## 1.1. Problem

Professional DJs prepare tracks before a performance by marking key structural positions — the first downbeat, the vocal entry, the drop, a breakdown — so they can navigate instantly during a live set. Without stored cue points, the DJ must rely on memory or real-time waveform scanning to find these positions, which is slow, error-prone, and incompatible with the speed of live mixing where transitions happen every 30 to 90 seconds. A DJ with a library of 10,000+ tracks cannot memorize structural positions for every song.

The temporary cue from PRD-0004 solves in-the-moment navigation (return to a single point), but it resets on track load and cannot persist across sessions. DJs need 8 independently assignable, color-coded, persistent hot cue pads that survive application restarts and track reloads — matching the workflow established by Pioneer CDJ-3000 and Traktor Pro. Without this, Sonik forces the DJ to re-discover every track's structure on every load, making it unsuitable for professional use.

Beyond navigation, hot cues are a performance instrument. DJs trigger hot cues rhythmically to create stutters, jump between sections for live remixing, and use color-coded waveform markers to visually plan transitions. The system must respond within a single audio buffer cycle, produce zero audible artifacts on every jump, and display markers on the waveform with frame-accurate precision.

## 1.2. Objective

The system provides 8 color-coded, persistent hot cue pads per deck that:
- Allow the DJ to store a sample-accurate position on any of 8 pads (labeled A through H) with a single press, recallable by pressing the same pad during playback or while stopped.
- Trigger playback from the stored position within a single `processBlock` cycle (under 3 ms at 128 samples / 44.1 kHz), using the same 64-sample crossfade ramp as the transport system (PRD-0004) to eliminate clicks and discontinuities.
- Persist all hot cue data (position, color, label) in SQLite keyed by track content hash, so cue points survive application restarts, track reloads, and re-imports.
- Render color-coded triangular markers on both the overview and detail waveforms via the `samplePositionToPixelX` coordinate mapping from PRD-0006.
- Support user-selectable colors from a 16-color palette, with each pad defaulting to a unique color from the palette.
- Support optional text labels (up to 12 characters) per hot cue for structural annotation (e.g., "Drop", "Vocal", "Outro").
- Provide a dedicated delete mode with single-action undo to protect against accidental cue loss.
- When quantize mode is enabled (future PRD-0013), snap cue placement to the nearest beat position from the beatgrid (PRD-0008). Triggering is always instant regardless of quantize state.
- Communicate all cue operations between UI and audio thread using only `std::atomic` and lock-free structures, with zero allocations, locks, or I/O on the audio thread.

## 1.3. User Flow

### 1.3.1. Setting a Hot Cue

1. The user has a track loaded on Deck A and the playhead is at a desired position (e.g., the first downbeat at sample 88,200). The 8 hot cue pads (A through H) are displayed below the waveform, all currently unlit (no cue assigned).
2. The user presses pad C (the third pad). The system stores the current playhead position (88,200 samples) as hot cue C. The pad illuminates with its default color (pad C defaults to yellow, index 2 in the palette). A yellow triangular marker appears on both the overview and detail waveforms at the corresponding position.
3. If quantize mode is active and a beatgrid exists, the stored position snaps to the nearest beat. For example, if the playhead is at sample 89,000 and the nearest beat is at 88,200, the cue stores 88,200.
4. If quantize mode is inactive or no beatgrid exists, the exact playhead position is stored with no snapping.
5. The hot cue data (pad index, position, color, label) is written to SQLite immediately. The write occurs on a background database thread, never on the UI or audio thread.

### 1.3.2. Triggering a Hot Cue

6. The user presses pad C during playback. The transport executes a seek to sample 88,200 with a 64-sample fade-out at the current position and a 64-sample fade-in at the target position (identical to PRD-0004 seek behavior). Playback continues from the new position without interruption.
7. The user presses pad C while the deck is Paused or Stopped. The playhead jumps to sample 88,200 instantly (no fade needed per PRD-0004 seek-while-stopped behavior). The deck transitions to Playing and audio begins with a 64-sample fade-in.
8. The user presses pad C repeatedly in rhythm (e.g., on every beat). Each press triggers a seek to the stored position. The 64-sample fade-out and fade-in occur on every trigger, producing a clean stutter effect with no clicks.
9. If the target position is within 64 samples of the current playhead, the seek is skipped to avoid overlapping fade ramps.

### 1.3.3. Overwriting a Hot Cue

10. The user navigates to a new position and wants to reassign pad C. The user enters set mode (holds Shift or a dedicated "Set" modifier button) and presses pad C. A confirmation visual (pad flashes twice) indicates the cue has been overwritten. The waveform marker moves to the new position. The SQLite record updates.
11. If the user presses an already-assigned pad without the set modifier, the pad triggers playback from the stored position (default behavior). This prevents accidental overwrites.

### 1.3.4. Deleting a Hot Cue

12. The user wants to remove hot cue C. The user enters delete mode (holds the "Delete" modifier button) and presses pad C. The pad dims, the waveform marker disappears, and the SQLite record is soft-deleted.
13. A toast notification appears: "Hot Cue C deleted. Press Ctrl+Z to undo." The undo window lasts 10 seconds or until the user performs another cue operation.
14. If the user presses Ctrl+Z (Cmd+Z on macOS) within the undo window, the hot cue is restored to its previous state. The pad re-illuminates and the waveform marker reappears.
15. If the undo window expires, the soft-deleted record is permanently removed from SQLite.

### 1.3.5. Color and Label Management

16. The user right-clicks (or long-presses) pad C. A context popup appears showing a 4x4 color palette (16 colors) and a text input field for the label.
17. The user selects a new color (e.g., magenta). The pad, waveform marker, and SQLite record update immediately.
18. The user types a label "Drop" (max 12 characters). The label appears as a tooltip on the waveform marker and as small text below the pad. The label is persisted in SQLite.
19. The user clears the label by deleting all text. The label field returns to empty.

### 1.3.6. Waveform Markers

20. Hot cue markers render as downward-pointing triangles at the top edge of both waveform views, filled with the cue's assigned color.
21. On the detail waveform, each marker also draws a thin vertical line (1 pixel, cue color at 50% opacity) extending from the triangle to the bottom of the waveform area.
22. On the overview waveform, markers are triangle-only (no vertical line) to avoid visual clutter.
23. When the playhead is within 2 seconds (in visible time) of a marker on the detail waveform, the marker's label (if set) fades in above the triangle.
24. Markers update position in real time as the waveform scrolls, using `samplePositionToPixelX` from PRD-0006.

### 1.3.7. Persistence and Track Reload

25. The user quits Sonik. All hot cue data is already persisted in SQLite (written on every set/edit/delete operation).
26. The user relaunches Sonik and loads the same track. The system queries SQLite by content hash, finds 3 stored hot cues (pads A, C, F), and populates the state tree. Pads A, C, F illuminate with their saved colors; markers appear on both waveforms. Pads B, D, E, G, H remain unlit.
27. The user loads the same file from a different path (e.g., moved from Desktop to Music folder). Because persistence is keyed by content hash (not file path), all hot cues are found and restored.
28. The user loads a track that has hot cue positions stored beyond the track's duration (e.g., file was re-encoded and is now shorter). Cues beyond duration are flagged as invalid, hidden from the UI, but not deleted from SQLite (per PRD-0001 acceptance criteria).

### 1.3.8. Empty and Error States

29. The user presses a pad when no track is loaded (deck in Empty state). Nothing happens. Pads are visually inactive (grayed out, no pointer cursor).
30. The user loads a track with no previously saved hot cues. All 8 pads are unlit. No markers appear on the waveform. The DJ begins setting cues from scratch.

## 1.4. Acceptance Criteria

### 1.4.1. Hot Cue Storage and Data Model

- [ ] Each deck supports exactly 8 hot cue slots, indexed 0 through 7, mapped to pads labeled A through H.
- [ ] Each hot cue stores: pad index (`int`, 0-7), sample position (`int64_t`), color index (`int`, 0-15), label (`juce::String`, max 12 characters), and active flag (`bool`).
- [ ] Hot cue state is stored in the deck's `juce::ValueTree` as 8 child nodes of type `HotCue` under a `HotCues` parent node.
- [ ] Each `HotCue` node has properties: `padIndex`, `positionSamples`, `colorIndex`, `label`, `active`.
- [ ] Inactive (empty) hot cue slots have `active = false` and are not rendered on the waveform.

### 1.4.2. Setting Hot Cues

- [ ] Pressing an unassigned pad stores the current playhead position and sets `active = true`.
- [ ] Setting a cue on a pad that is already assigned requires a modifier key (Shift or dedicated Set button). Without the modifier, the pad triggers playback.
- [ ] When quantize mode is enabled and a beatgrid exists, the stored position snaps to the nearest beat: `nearestBeat = anchor + round((playhead - anchor) / beatInterval) * beatInterval`.
- [ ] When quantize mode is disabled or no beatgrid exists, the exact playhead position is stored.
- [ ] Setting a hot cue triggers a SQLite write on a background thread within 100 ms.

### 1.4.3. Triggering Hot Cues

- [ ] Pressing an assigned pad during Playing state executes a transport seek to the stored position with 64-sample fade-out at current position and 64-sample fade-in at target position.
- [ ] Pressing an assigned pad during Paused or Stopped state jumps the playhead to the stored position and transitions to Playing with a 64-sample fade-in.
- [ ] The seek command is delivered to the audio thread via the same lock-free mechanism used by the transport (PRD-0004).
- [ ] Hot cue trigger latency is under 1 `processBlock` cycle from button press to audio output change.
- [ ] If the stored position is within 64 samples of the current playhead, the seek is a no-op to prevent overlapping fade ramps.
- [ ] Triggering a hot cue does not affect the temp cue point (PRD-0004). The temp cue remains at its independently set position.

### 1.4.4. Deleting Hot Cues

- [ ] Deleting requires a modifier key (dedicated Delete button) plus pad press. Pressing a pad without the modifier always triggers.
- [ ] Deletion soft-deletes the hot cue: `active` is set to `false`, the pad dims, and the waveform marker is hidden.
- [ ] A single-level undo is available for 10 seconds after deletion (Ctrl+Z / Cmd+Z). Undo restores the cue to its pre-deletion state.
- [ ] After the 10-second undo window, the SQLite record is permanently deleted.
- [ ] Performing another cue operation (set, delete, overwrite on any pad) cancels the pending undo.

### 1.4.5. Color System

- [ ] A fixed 16-color palette is defined, matching the Rekordbox-compatible color set:
  - Index 0: Pink (#F870A0)
  - Index 1: Red (#E8302A)
  - Index 2: Orange (#F08828)
  - Index 3: Yellow (#E8D820)
  - Index 4: Green (#10B020)
  - Index 5: Aqua (#18C8C8)
  - Index 6: Blue (#1870F0)
  - Index 7: Purple (#A028E8)
  - Index 8: Magenta (#E840A0)
  - Index 9: Salmon (#F06850)
  - Index 10: Peach (#F0A060)
  - Index 11: Lime (#A0E828)
  - Index 12: Teal (#28B898)
  - Index 13: Sky (#50B8F0)
  - Index 14: Lavender (#9878F0)
  - Index 15: Rose (#E868A8)
- [ ] Default color assignment: pad A = index 1 (Red), pad B = index 3 (Yellow), pad C = index 4 (Green), pad D = index 6 (Blue), pad E = index 7 (Purple), pad F = index 2 (Orange), pad G = index 5 (Aqua), pad H = index 0 (Pink).
- [ ] Color is selectable via a right-click/long-press context popup showing the 4x4 palette grid.
- [ ] Changing a color updates the pad, waveform markers, and SQLite record immediately.

### 1.4.6. Labels

- [ ] Each hot cue accepts an optional text label of up to 12 characters.
- [ ] Labels are editable via the same right-click/long-press context popup that shows the color picker.
- [ ] Labels are displayed as tooltip text on waveform markers (visible when the playhead is within 2 seconds of the marker on the detail view).
- [ ] Labels are displayed as small text below the pad letter in the pad UI.
- [ ] Labels are persisted in SQLite alongside position and color.

### 1.4.7. Waveform Markers

- [ ] Active hot cues render as downward-pointing filled triangles (8px wide, 8px tall) at the top edge of the waveform, using the cue's assigned color.
- [ ] On the detail waveform, each marker also renders a 1-pixel vertical line from the triangle to the waveform bottom, at 50% opacity of the cue color.
- [ ] On the overview waveform, markers are triangle-only (no vertical line).
- [ ] Marker positions are computed via `samplePositionToPixelX(positionSamples)` from PRD-0006.
- [ ] Markers re-render correctly on zoom level changes, window resize, and track scrolling.
- [ ] Marker rendering does not degrade waveform frame rate below 60 fps.

### 1.4.8. Persistence

- [ ] Hot cue data is stored in a SQLite table `hot_cues` with columns: `id` (INTEGER PRIMARY KEY), `track_hash` (TEXT NOT NULL), `pad_index` (INTEGER NOT NULL, 0-7), `position_samples` (INTEGER NOT NULL), `color_index` (INTEGER NOT NULL), `label` (TEXT DEFAULT ''), `created_at` (TEXT NOT NULL).
- [ ] A UNIQUE constraint exists on `(track_hash, pad_index)` to prevent duplicate entries.
- [ ] On track load, the system queries `SELECT * FROM hot_cues WHERE track_hash = ?` and populates the state tree.
- [ ] All writes (set, overwrite, delete, color change, label edit) execute on a background database thread, never blocking the UI or audio thread.
- [ ] Cached load of 8 hot cues for a track completes in under 50 ms.
- [ ] Hot cues with `position_samples` beyond the loaded track's `totalSamples` are flagged as invalid (`active = false`) but not deleted from the database.
- [ ] Persistence is keyed by content hash (from PRD-0001), not file path. Moving a file preserves all cue data.

### 1.4.9. Audio Thread Safety

- [ ] The audio thread reads hot cue target positions from `std::atomic<int64_t>` values, one per pad.
- [ ] Hot cue trigger commands are delivered via the same lock-free command queue used by the transport (PRD-0004).
- [ ] Zero memory allocations, zero locks, zero I/O occur on the audio thread for any cue operation.
- [ ] All cue state mutations (set, delete, color, label) occur on the UI thread and propagate to the audio thread via atomics.

### 1.4.10. UI Pad Component

- [ ] The pad strip renders 8 square pads in a horizontal row, each labeled A through H.
- [ ] An assigned pad is filled with its cue color. An unassigned pad is dark/inactive.
- [ ] Pads are visually interactive: hover brightens by 20%, press darkens by 10%, release returns to base.
- [ ] All pads are grayed out and non-interactive when the deck is in Empty state (no track loaded).
- [ ] The pad strip component mounts into the DeckShellComponent below the waveform area.

### 1.4.11. Scope Boundaries

- [ ] Memory cues (unlimited, non-pad positional bookmarks) are NOT implemented in this PRD.
- [ ] Loop cues (hot cue that triggers a loop from its position) are NOT implemented in this PRD. Pads store position only.
- [ ] Gate cues (play only while pad is held) are NOT implemented in this PRD.
- [ ] Quantize-aware hot cue triggering (delaying the jump to the next beat boundary) is NOT implemented in this PRD. All triggers are instant. PRD-0013 may add quantized triggering in the future.
- [ ] MIDI pad mapping is NOT implemented in this PRD. Pad interaction is mouse/keyboard only.
- [ ] All code resides under `Source/Features/Cue/`. Dependencies passed via constructor injection. No singletons.

## 1.5. Grey Areas

### 1.5.1. Hot Cue Trigger Behavior: Instant Jump vs Crossfade

Pioneer CDJ-3000 and Rekordbox perform an effectively instant jump when a hot cue is triggered during playback. Internally, Pioneer applies a micro-fade of approximately 1 to 2 ms to suppress clicks, but the transition is perceptually instantaneous. Traktor uses a similar approach with a short crossfade of approximately 5 ms. Sonik reuses the 64-sample crossfade ramp established in PRD-0004 (~1.45 ms at 44.1 kHz), which matches the Pioneer behavior. This is a fade-out at the old position and fade-in at the new position, not a true crossfade (both positions are never mixed simultaneously). This keeps the implementation unified with the transport seek path and avoids a separate crossfade buffer.

### 1.5.2. Memory Cue vs Hot Cue Distinction

Rekordbox distinguishes between memory cues (unlimited positional markers visible on the waveform, navigable via prev/next buttons) and hot cues (8 pads for instant access). Pioneer CDJ-3000 inherits this distinction. Traktor has only hot cues. Sonik defers memory cues to a future PRD. The 8 hot cue pads cover the primary performance use case. DJs who need more than 8 marked positions per track can reassign pads per section. The temp cue from PRD-0004 serves as the single non-persistent navigational cue point. If DJ feedback indicates demand, a future PRD can add a memory cue list that is separate from the hot cue pads.

### 1.5.3. Accidental Deletion and Undo

Pioneer CDJ-3000 and Traktor provide no undo for cue deletion. This is a known pain point in both ecosystems. Sonik improves on this with a 10-second single-level undo window. The 10-second window is short enough that the soft-deleted record does not accumulate in memory, but long enough for the DJ to realize the mistake and press undo. Only the most recent deletion is undoable (not a multi-level stack) to keep the implementation simple and the mental model clear. Performing any other cue operation cancels the pending undo, which prevents state confusion.

### 1.5.4. SQLite Persistence Schema

Hot cues are persisted keyed by `track_hash` (content hash from PRD-0001), not file path. This ensures that cue data survives file moves and renames. The `position_samples` field stores the absolute sample index at the track's original sample rate. If a file is re-encoded at a different sample rate, the positions may be invalid; however, the content hash would also change in this case, so the cue data naturally does not apply. The `created_at` timestamp supports future features like "recently modified cues" sorting but has no functional impact in this PRD.

### 1.5.5. Quantize Interaction

Quantize mode (PRD-0013, future) affects two operations differently. First, cue placement: when setting a hot cue with quantize enabled, the stored position snaps to the nearest beat from the beatgrid (PRD-0008). This uses `getBeatPositionForSample` to find the closest beat and stores that position instead of the raw playhead value. Second, cue triggering: on Pioneer CDJ-3000, quantize mode delays hot cue triggering until the next beat boundary, maintaining phrase alignment during live performance. Sonik does not implement quantized triggering in this PRD. All triggers are instant. PRD-0013 may add an option for beat-quantized triggering by buffering the seek command until the playhead crosses the next beat boundary.

### 1.5.6. Color Palette Selection

The 16-color palette is designed to be Rekordbox-compatible, using visually distinct hues that are identifiable under stage lighting and on LED pads. The palette avoids colors that are too similar (e.g., no two shades of blue) and ensures sufficient contrast against both dark waveform backgrounds and lit/unlit pad states. User-defined custom colors are not supported in v1 to avoid UI complexity and incompatibility with MIDI controller LED color ranges. The 16-color palette covers the practical range of colors that MIDI pad controllers (e.g., Novation Launchpad, Native Instruments Maschine) can display.

### 1.5.7. Hot Cue Types: Position-Only vs Loop/Gate Cues

Rekordbox supports multiple hot cue types: position cue (jump to point), loop cue (jump and activate a loop), and gate cue (play only while held). Pioneer CDJ-3000 supports all three. Sonik v1 implements position-only hot cues. Loop cues depend on the looping system (future PRD-0014) and gate cues add complexity to the transport state machine that is not justified before core features stabilize. The data model does not include a `type` field in v1. When loop cues are added, a `cue_type` column and `loop_length_samples` column will be added to the `hot_cues` table via a schema migration.

### 1.5.8. Hot Cue Trigger During Stopped State

On Pioneer CDJ-3000, pressing a hot cue while the player is stopped (cued) starts playback from the hot cue position. This differs from the CUE button, which only previews while held. Sonik follows this convention: triggering a hot cue from Stopped or Paused always transitions to Playing. This gives the DJ a way to instantly launch into any section of the track without first pressing Play. The state transition is Stopped/Paused -> Playing (with seek to cue position), using the same 64-sample fade-in ramp as a normal Play command.