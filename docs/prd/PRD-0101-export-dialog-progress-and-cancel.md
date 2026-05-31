---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0099
  - PRD-0100
---

# 1. PRD-0101: Export Dialog, Progress & Cancel

## 1.1. Problem

EPIC-0012 has, by the time this PRD is reached, built every non-user-facing
piece of the export pipeline: the offline render driver (PRD-0099) that advances
the EPIC-0010 timeline engine and EPIC-0011 automation applier block-by-block in
non-real-time, and the audio exporter (PRD-0100) that takes those summed buffers
and writes WAV / FLAC / MP3 via `juce::AudioFormatWriter`. What does not yet
exist is the human-facing surface that ties them together: a dialog where the DJ
chooses *what* to export and *how*, kicks off the render, watches it progress,
and can abort it cleanly.

Without that surface the export engine is unreachable. Worse, a naive
implementation would run the render on the message thread, freezing the entire
UI for the (potentially many) seconds an arrangement takes to render and encode,
with no progress indication and no way to cancel a mistaken export. The DJ would
see a beachball, not a progress bar, and a half-written file would be left on
disk if they force-quit.

This PRD also serves as the **capstone of EPIC-0012 and of the whole DAW
initiative**. Every prior Epic — recording, automation, editing, playback,
session save/open — culminates here: the DJ must be able to record a set, save
it, reopen it, and export it to a shareable audio file in one continuous round
trip. This PRD owns the dialog organism, the background-thread orchestration,
the progress/cancel UX, the option validation, the error surfacing, and the
end-to-end round-trip validation that proves the DAW loop is closed. It owns no
new render or encoding logic (PRD-0099 / PRD-0100) and no session save/open
logic (PRD-0095 / PRD-0096); it composes them.

## 1.2. Objective

The system presents a DESIGN.md-compliant export dialog organism
(`Source/Features/Daw/Export/Ui/ExportDialog.h/.cpp`) such that:

- The DJ can choose, in a single monochrome modal dialog: output **format**
  (WAV / FLAC / MP3, from PRD-0100), **sample rate**, **bit depth** (WAV/FLAC)
  or **MP3 bitrate**, **export range** (whole arrangement vs the currently
  selected region/loop), **output path** (via `juce::FileChooser`), and a
  **normalization** toggle.
- Conflicting or inapplicable options are disabled, not hidden: choosing MP3
  disables the bit-depth selector and enables the bitrate selector; choosing WAV
  / FLAC does the inverse. The export range "selected region" option is disabled
  when no region/loop is selected.
- Pressing **Export** validates the chosen options and output path, then runs
  the PRD-0099 offline render + PRD-0100 encode on a **background thread**, never
  on the message thread. The dialog shows a **progress bar** (percentage plus
  elapsed and estimated-remaining time) that updates from message-thread
  callbacks marshalled off the render thread.
- The progress bar is rendered per DESIGN.md: a monochrome dithered fill
  (checkerboard density increasing left-to-right with completion), `2px solid`
  `#2d2d2d` border, zero corner radius, `Space Mono` percentage label. No colour,
  no smooth gradient.
- A **Cancel** button is live throughout the render. Pressing it signals the
  render thread to stop at the next block boundary, **deletes the partially
  written output file**, restores the transport/playhead to its pre-export
  state, and returns the dialog to its option-entry state (or closes it).
- Errors are surfaced in DESIGN.md style without crashing or leaving partial
  files: no write permission at the chosen path, an unresolved / missing source
  clip (PRD-0097), or an encoder failure (PRD-0100) each produce a clear
  monochrome error message inside the dialog and abort the render cleanly.
- The **capstone round-trip is validated**: a recorded arrangement can be saved
  (PRD-0095/0096), the app reopened, the session restored, and the arrangement
  exported to a file that plays back correctly — proving the full DAW loop end to
  end.
- All orchestration runs off the real-time audio thread; the live audio device
  is untouched during export (the offline driver pulls the engine in a
  non-real-time loop per PRD-0099).

## 1.3. User Flow

1. The DJ finishes (or reopens) an arrangement and selects **Export** from the
   DAW menu / toolbar. The `ExportDialog` organism opens as a modal monochrome
   dialog centred over the arrangement, with a `2px solid #2d2d2d` border, zero
   radius, dithered drop shadow, and `Space Mono` labels.
2. The dialog presents the option controls, pre-filled with the last-used
   options (persisted, see §1.5.6) or sensible defaults on first run: format
   `WAV`, sample rate matching the project rate, bit depth `24`, range `Whole
   arrangement`, normalization `off`, output path empty.
3. The DJ picks a **format**. Selecting `MP3` disables the bit-depth selector and
   enables an MP3 bitrate selector (e.g. 192 / 256 / 320 kbps); selecting
   `WAV` / `FLAC` enables bit depth and disables bitrate. Disabled controls are
   visibly dimmed via tonal layering, not removed.
4. The DJ optionally changes sample rate, toggles normalization, and chooses the
   **export range**. If no region/loop is currently selected in the arrangement,
   the "Selected region" radio is disabled and "Whole arrangement" is forced.
5. The DJ clicks **Choose…** to open a `juce::FileChooser`, picks an output
   location and filename; the chosen path is shown in a `Space Mono` field. The
   file extension is reconciled to the chosen format.
6. The DJ clicks **Export**. The dialog validates: a path must be chosen, the
   directory must be writable, and (for "Selected region") a region must exist.
   On any validation failure, an inline monochrome error message appears and the
   render does not start.
7. On success the option controls are replaced (in place) by the **progress
   view**: a dithered progress bar with a `Space Mono` percentage, elapsed time,
   and estimated-remaining time, plus a live **Cancel** button. The PRD-0099
   render + PRD-0100 encode run on a background thread.
8. As the render advances, message-thread progress callbacks (marshalled off the
   render thread) update the bar. The percentage and dithering density increase
   monotonically toward 100%.
9. On completion the progress view shows a "Done" state with the output path; the
   DJ dismisses the dialog. The exported file exists at the chosen path and plays
   back correctly.
10. If the DJ clicks **Cancel** mid-render, the render thread stops at the next
    block boundary, the partial output file is deleted, the transport is restored
    to its pre-export state, and the dialog returns to the option view (or
    closes). No partial artefact remains on disk.
11. If an error occurs mid-render (lost write permission, unresolved source,
    encoder failure), the render aborts, any partial file is deleted, and a
    DESIGN.md-style error message replaces the progress bar; the DJ can retry or
    cancel.

## 1.4. Acceptance Criteria

- [ ] A new organism `Source/Features/Daw/Export/Ui/ExportDialog.h/.cpp` exists,
      composing the PRD-0099 offline render driver and PRD-0100 audio exporter;
      it adds no new render or encoding logic of its own.
- [ ] The dialog exposes controls for: format (WAV / FLAC / MP3), sample rate,
      bit depth (WAV/FLAC) or MP3 bitrate, export range (whole arrangement vs
      selected region), output path (via `juce::FileChooser`), and a
      normalization toggle.
- [ ] Selecting `MP3` disables the bit-depth selector and enables the MP3
      bitrate selector; selecting `WAV` or `FLAC` enables bit depth and disables
      bitrate. Disabled controls remain visible (dimmed via tonal layering), not
      hidden.
- [ ] The "Selected region" range option is disabled when no region/loop is
      selected in the arrangement, and "Whole arrangement" is the forced default
      in that case.
- [ ] Choosing the output path uses `juce::FileChooser`; the displayed path uses
      `Space Mono`, and the file extension is reconciled to the chosen format.
- [ ] Pressing **Export** runs the PRD-0099 render and PRD-0100 encode on a
      background thread (`juce::ThreadWithProgressWindow` or a custom
      `juce::Thread` with message-thread progress callbacks); the message thread
      is never blocked by the render.
- [ ] The dialog shows a progress bar with: a percentage label, elapsed time, and
      estimated-remaining time, all in `Space Mono`. The bar is updated from
      message-thread callbacks marshalled off the render thread (no UI mutation
      from the render thread).
- [ ] The progress bar fill is a monochrome dithered (checkerboard) pattern whose
      density increases with completion, with a `2px solid #2d2d2d` border and
      zero corner radius. ✅ No colour, ✅ no smooth gradient. ❌ No green/yellow/red.
- [ ] A **Cancel** button is live for the entire render. Pressing it stops the
      render at the next block boundary within a bounded time, deletes the
      partially written output file, restores the transport/playhead to its
      pre-export state, and returns the dialog to the option view (or closes it).
- [ ] Validation before render start rejects: an empty output path, a
      non-writable target directory, and the "Selected region" range with no
      region selected. Each failure shows an inline DESIGN.md-style monochrome
      error message and does not start the render.
- [ ] An unresolved / missing source clip (PRD-0097) encountered during render
      aborts the render cleanly, deletes any partial file, and surfaces a
      monochrome error message identifying the problem.
- [ ] An encoder failure reported by PRD-0100 (e.g. MP3 encoder unavailable, disk
      full) aborts the render cleanly, deletes any partial file, and surfaces a
      monochrome error message.
- [ ] On successful completion the exported file exists at the chosen path, is
      readable by a `juce::AudioFormatReader`, and matches the chosen format /
      sample rate / bit depth (or bitrate).
- [ ] The last-used options (format, sample rate, bit depth / bitrate, range,
      normalization) are persisted and restored as the dialog's defaults on next
      open; the output path is not persisted (re-chosen each time).
- [ ] The dialog complies with DESIGN.md: strict monochrome palette
      (`#2d2d2d` / `#fdfdfd`), `Space Mono Regular` font, `2px solid #2d2d2d`
      borders with active/inactive fill inversion on buttons, zero `border-radius`,
      dithered shadow, and pixel-art / no-emoji iconography.
- [ ] No export code runs on the real-time audio thread; the live audio device is
      not used during export. The render uses the PRD-0099 non-real-time driver,
      and all cross-thread progress/cancel communication uses `std::atomic` or a
      lock-free FIFO (no locks on any path touched while the live engine could be
      running).
- [ ] A capstone round-trip test (`Tests/SessionRoundTripTests.cpp` or similar)
      builds an arrangement, saves it (PRD-0095/0096), reopens it, exports it via
      the headless export path, and asserts the rendered file is non-empty,
      readable, and the expected duration for the chosen range.
- [ ] An export test suite (`Tests/AudioExportTests.cpp` or similar) covers:
      option validation, format-driven control enable/disable, cancel-deletes-
      partial-file, error-on-missing-source, and last-used-option persistence.

## 1.5. Grey Areas

### 1.5.1. Background-Thread Mechanism: ThreadWithProgressWindow vs Custom Thread

JUCE offers `juce::ThreadWithProgressWindow`, which bundles a worker thread with
a built-in modal progress window, versus rolling a custom `juce::Thread` that
posts progress to a bespoke dialog via `juce::MessageManager::callAsync`.

**Resolution:** Use a **custom `juce::Thread`** driving the bespoke
`ExportDialog`, not `ThreadWithProgressWindow`. The built-in progress window is a
stock JUCE component with rounded corners, system fonts, and a standard
gradient/animated progress bar — it cannot be made DESIGN.md-compliant without
fighting the framework. Since this PRD already owns a custom monochrome dialog
organism, the progress view is just another state of that same dialog, sharing
its border, font, and dithering. The custom thread sets an `std::atomic<bool>`
"should cancel" flag, reads an `std::atomic<float>` progress fraction written by
the render loop, and marshals UI updates onto the message thread via
`callAsync`. This keeps the render→UI boundary lock-free and the look fully
under our control.

### 1.5.2. Cancel Cleanup: Partial File and Transport Restoration

When the DJ cancels mid-render, two pieces of state must be undone: the
partially written output file on disk, and any transport/playhead movement the
offline driver caused. The question is how aggressively to clean up and in what
order.

**Resolution:** On cancel, the render thread (a) stops calling the PRD-0099
driver at the next block boundary, (b) closes/flushes and then **deletes the
partial output file** via `juce::File::deleteFile()`, and (c) restores the
transport to the playhead position and play state captured *before* the export
began. The capture-and-restore is mandatory because PRD-0099's driver may advance
a shared playhead; leaving it parked at the export end-point would surprise the
DJ on their next live action. Order matters: stop the driver first (so no further
writes occur), then delete the file (now safely idle), then restore transport.
If file deletion fails (e.g. the OS still holds a handle briefly), retry once
after the writer is fully released; if it still fails, surface a non-fatal
warning rather than block the cancel.

### 1.5.3. Progress Estimation: Block Count vs Wall-Clock Time

Progress percentage and estimated-remaining time can be derived from the fraction
of audio blocks rendered (deterministic, known total) or from extrapolated
wall-clock timing (accounts for variable source-read / encode cost).

**Resolution:** Use **block count for the percentage** and **wall-clock
extrapolation for the time estimate**. The render has a known total block count
(arrangement length / block size for the chosen range), so the completion
fraction is exact and monotonic — ideal for the dithered bar. The
estimated-remaining *time*, however, depends on real throughput (FLAC encodes
slower than WAV, MP3 slower still, and disk speed varies), so it is computed as
`elapsed / fractionDone − elapsed`, smoothed over a short window to avoid jitter.
This gives an honest, monotonic bar plus a useful (if approximate) ETA, without
pretending the per-block cost is constant.

### 1.5.4. Option Validation and Disabling: Eager vs On-Export

Inapplicable options (bit depth under MP3, "selected region" with no selection)
could be validated only when **Export** is pressed, or continuously disabled as
the DJ changes other controls.

**Resolution:** **Disable continuously**, validate path/range **on Export**.
Format-driven control state (bit depth vs bitrate) and the region availability
are deterministic functions of other UI state, so they are recomputed on every
relevant change and the affected controls are enabled/disabled live — the DJ can
never construct an invalid format/depth combination. Path existence/writability
and "a region is actually selected" are checked at Export time because they
depend on the filesystem and on transient selection state that can change
between dialog open and Export. This split keeps the common path frictionless
(you cannot pick a contradictory option) while deferring filesystem checks to the
last responsible moment.

### 1.5.5. Error Surfacing: Permissions and Missing Sources

Errors arise at different stages: write-permission failures (at validation or
mid-render if permission is revoked), unresolved sources (PRD-0097, discovered as
the render reaches the offending clip), and encoder failures (PRD-0100). They
must be surfaced consistently and never leave a partial file.

**Resolution:** All errors funnel through a single **inline error state** of the
dialog — a `Space Mono` message block replacing the progress bar, with the same
`2px` border and monochrome palette, never a native alert box (which would break
DESIGN.md). Write-permission is checked up front at validation (best effort) and
again handled if the writer fails mid-render. Unresolved sources are detected by
PRD-0097's resolution layer before/at render start; rather than silently
skipping the clip, the export **aborts** with a message naming the missing source
(consistent with EPIC-0012's "never silently drop clips" rule). In every error
case the partial file is deleted (per §1.5.2) before the message is shown. The DJ
can then fix the issue and retry, or cancel.

### 1.5.6. Default Options and Persistence of Last-Used

The dialog could reset to fixed defaults each time, or remember the DJ's
last-used format/quality choices. And those remembered choices could be stored
per-session, per-app, or per-OS-user.

**Resolution:** Persist **last-used options app-wide** (per OS user, via the
application properties store), **except the output path**, which is always
re-chosen. Format, sample rate, bit depth / bitrate, export range *mode*, and
normalization are restored on next open because a DJ who exports 320 kbps MP3
once usually wants it again; forcing them to re-pick every time is needless
friction. The output *path* is deliberately not persisted — silently re-using a
prior path risks overwriting a previous export without the DJ noticing, so a fresh
`FileChooser` is required each time. First-run defaults are: WAV, project sample
rate, 24-bit, whole arrangement, normalization off.

### 1.5.7. Blocking Modal vs Non-Modal During Export

While the render runs, the dialog could be a hard modal (the DJ cannot touch the
arrangement until export finishes or cancels) or non-modal (the DJ can keep
working while the background render proceeds).

**Resolution:** **Modal for this PRD.** The offline driver (PRD-0099) advances a
shared playhead and reads the same arrangement snapshot the live engine uses;
allowing concurrent edits during render would require snapshotting the entire
arrangement at export start and isolating the driver from subsequent mutations —
a substantial addition outside this Epic's scope. A modal export (with a working
Cancel) is the simpler, safer contract: the DJ commits to the export, watches it,
and can abort. Background/non-modal export over an immutable snapshot is a clean
future enhancement that this dialog's structure does not preclude.

### 1.5.8. Capstone Validation Acceptance: What "Closing the Loop" Requires

As the final PRD of the DAW initiative, this PRD must prove the whole loop, but
the precise bar for "validated" is a judgement call — a single happy-path test, a
matrix across formats, or a full manual sign-off.

**Resolution:** The capstone bar is a **headless round-trip integration test plus
a manual test plan**. The automated test (`SessionRoundTripTests.cpp`) builds a
small arrangement, saves it, reopens it from disk into a fresh model, and exports
it via the headless export path (the same render/encode the dialog drives, minus
the UI), asserting the output file is non-empty, readable, and the expected
duration. This proves save → reopen → export deterministically in CI. Exhaustive
format/quality matrices belong to PRD-0100's exporter tests; the round-trip test
needs only one representative format (WAV) to prove the *loop*. The manual test
plan (below) covers the interactive dialog behaviour — option disabling, live
progress, cancel-deletes-partial, error surfacing — that is impractical to fully
automate. Together they constitute "the loop is closed."

## 1.6. Manual Test Plan

1. **Open the dialog.** Build or open an arrangement, then choose **Export**.
   - Expected: A monochrome modal dialog appears, centred, with a `2px solid`
     `#2d2d2d` border, zero corner radius, `Space Mono` labels, and a dithered
     drop shadow. Options are pre-filled with last-used (or first-run defaults).
2. **Toggle format MP3 ↔ WAV.** Select `MP3`, then `WAV`, then `FLAC`.
   - Expected: With `MP3`, the bit-depth selector dims/disables and the bitrate
     selector enables. With `WAV`/`FLAC`, the inverse. Disabled controls stay
     visible (dimmed), never disappear.
3. **Export range with no selection.** Ensure no region/loop is selected.
   - Expected: The "Selected region" option is disabled; "Whole arrangement" is
     forced.
4. **Validation on empty path.** Click **Export** without choosing a path.
   - Expected: An inline monochrome error message appears; no render starts.
5. **Successful export.** Choose a writable path, pick WAV/24-bit/whole
   arrangement, click **Export**.
   - Expected: The option view is replaced by a dithered progress bar with a
     `Space Mono` percentage, elapsed, and estimated-remaining time. The bar fill
     density increases monotonically. The UI stays responsive (no beachball).
6. **Verify output.** When "Done" appears, open the exported file.
   - Expected: The file exists at the chosen path, plays back correctly, and is
     WAV/24-bit at the project sample rate.
7. **Cancel mid-render.** Start a long export and click **Cancel** partway.
   - Expected: The render stops promptly, the dialog returns to the option view
     (or closes), and the partial output file is **not** present on disk. The
     transport/playhead is back where it was before export.
8. **Permission error.** Choose a read-only directory (or revoke write
   permission) and click **Export**.
   - Expected: A monochrome inline error message identifies the write failure; no
     partial file remains.
9. **Missing-source error.** Export an arrangement whose clip references a moved /
   missing source (PRD-0097).
   - Expected: The render aborts with a monochrome message naming the missing
     source; any partial file is deleted; the clip is not silently skipped.
10. **Last-used persistence.** Export once as 320 kbps MP3 with normalization on,
    close the app, reopen, and open the export dialog.
    - Expected: Format `MP3`, 320 kbps, normalization on are restored as defaults;
      the output path field is empty.
11. **Capstone round-trip.** Record/build a set, **Save** it, fully quit, reopen
    the app, **Open** the session, then **Export** it.
    - Expected: The arrangement restores exactly (clips, automation, grid), and
      the export produces a correct audio file — proving the full DAW loop.

Please execute these tests and let me know the results. I will wait for your
confirmation before treating EPIC-0012 as complete.
