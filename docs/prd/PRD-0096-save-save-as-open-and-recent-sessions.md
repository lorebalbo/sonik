---
status: Not Implemented
epic: EPIC-0012
depends-on:
  - PRD-0095
  - PRD-0083
---

# 1. PRD-0096: Save / Save As / Open & Recent Sessions

## 1.1. Problem

PRD-0095 delivers the `.soniksession` schema and serializer: a deterministic,
versioned mapping between the `daw` `ValueTree` and a file on disk, plus a
migration hook for older schema versions. But a serializer on its own is inert.
There is no way for the DJ to actually invoke it: no menu, no keyboard shortcut,
no file picker, no notion of "the session I am currently working on," and no
protection against discarding unsaved work. A serializer that nobody can call is
a unit test, not a feature.

The DAW model produced by EPIC-0008 through EPIC-0011 is a long-lived,
high-investment artefact: a DJ may spend an hour assembling clips, drawing
automation, and tuning the grid. Losing that to an accidental window close, a
mistaken **New**, or an **Open** that silently replaces the live model is
unacceptable. The application therefore needs a **session lifecycle controller**
that owns the full Save / Save As / Open / New flow, tracks whether the live
model has unsaved changes (the "dirty" state), prompts before any destructive
transition, and — critically — **rebuilds the entire DAW UI and restores the
saved view state (zoom, scroll)** when a session is opened, so reopening a
project genuinely returns the DJ to where they left off rather than to a
deserialized-but-blank-looking timeline.

This lifecycle layer also has no current home in the UI. There is no visible
"current session" indicator, so the DJ cannot tell at a glance which file they
are editing or whether it has unsaved edits. PRD-0083's undo manager gives a
precise, cheap signal for "has the model changed since the last save," but
nothing consumes it for dirty-state tracking. And the platform-standard
expectations — `Cmd/Ctrl+S` to save, `Shift+Cmd/Ctrl+S` for Save As, `Cmd/Ctrl+O`
to open, `Cmd/Ctrl+N` for new — are unmet.

## 1.2. Objective

A `SessionController` (`Source/Features/Daw/Session/SessionController.h/.cpp`)
owns the session lifecycle and exposes it to the UI such that:

- **Save** serializes the current `daw` model (via PRD-0095) to the current
  session path. If there is no current path (an untitled session), Save falls
  through to Save As.
- **Save As** opens a `juce::FileChooser` constrained to the `.soniksession`
  extension, writes the model to the chosen path, and adopts that path as the
  current session path (and title).
- **Open** opens a `juce::FileChooser` constrained to `.soniksession`,
  deserializes the file (via PRD-0095) into the `daw` model, **rebuilds the DAW
  UI** to reflect the reconstructed model, and **restores the saved view state**
  (zoom level and horizontal/vertical scroll) so the timeline appears exactly as
  it did when saved. Missing-source resolution is invoked here but owned by
  PRD-0097.
- **New** discards the current model (after the unsaved-changes prompt) and
  initialises a fresh, empty `daw` model with a clean undo history, no current
  path, and a default "Untitled" title.
- A **dirty-state flag** tracks whether the live model differs from the
  last-saved state, derived from PRD-0083's undo-manager change index (see
  §1.5.1). Save and Save As clear the flag; any model mutation sets it.
- Any **destructive transition** (New, Open, or application Quit) while the
  session is dirty presents a modal **"Save / Don't Save / Cancel"** prompt
  before proceeding. Cancel aborts the transition entirely.
- A **Recent Sessions** list (most-recent-first, de-duplicated, capped) is
  persisted in the application's `juce::PropertiesFile` and re-offered on the
  menu so the DJ can reopen a recent project in one click. Opening or saving a
  session promotes its path to the top of the list.
- The **current session title and dirty marker** are shown in the UI in the
  `DESIGN.md` monochrome language (e.g. `My Set.soniksession •` where the
  trailing `•` denotes unsaved changes), and the window title mirrors it.
- **Keyboard shortcuts** are bound: `Cmd/Ctrl+S` (Save), `Shift+Cmd/Ctrl+S`
  (Save As), `Cmd/Ctrl+O` (Open), `Cmd/Ctrl+N` (New).

This PRD owns the lifecycle UI and controller only. It does not own the schema
or serializer internals (PRD-0095), missing-source relocation (PRD-0097, merely
invoked here), import (PRD-0098), or export (PRD-0099–PRD-0101).

## 1.3. User Flow

1. The DJ has built an arrangement. The session indicator reads
   `Untitled •` (no path yet, dirty because the model has mutated since the
   empty New state). The window title mirrors this.
2. The DJ presses `Cmd/Ctrl+S`. Because there is no current path, the controller
   routes to Save As: a `juce::FileChooser` opens, defaulted to the platform
   documents directory with a suggested filename `Untitled.soniksession` and the
   `.soniksession` filter active.
3. The DJ picks a location and name (`My Set`). The controller serializes the
   `daw` model (PRD-0095) to `My Set.soniksession`, adopts that path as the
   current session, captures the current undo change index as the "saved
   baseline," clears the dirty flag, and promotes the path to the top of the
   Recent Sessions list. The indicator now reads `My Set.soniksession` (no dot).
4. The DJ keeps working; a clip is moved. The model mutation advances the undo
   change index past the saved baseline, so the controller sets the dirty flag
   and the indicator becomes `My Set.soniksession •`.
5. The DJ presses `Cmd/Ctrl+S` again. A path exists, so Save writes directly to
   `My Set.soniksession` with no dialog, re-captures the saved baseline, and
   clears the dot.
6. The DJ chooses **File → New** (`Cmd/Ctrl+N`) but the session is dirty. A
   modal prompt appears — `Save`, `Don't Save`, `Cancel` — in the monochrome
   dialog style. Choosing `Cancel` returns to the existing session unchanged;
   `Don't Save` discards and creates a fresh `Untitled` model; `Save` writes
   first, then creates the fresh model.
7. Later, the DJ chooses **File → Open Recent → My Set.soniksession**. Because
   the current session is clean, no prompt appears. The controller deserializes
   the file into the `daw` model, **tears down and rebuilds the DAW UI** from the
   reconstructed model, and **restores the saved zoom and scroll** so the
   timeline appears exactly as it was saved. Any clips whose sources cannot be
   resolved trigger the PRD-0097 relocation flow (owned there). The indicator
   reads `My Set.soniksession` (clean, since a freshly opened session has no
   unsaved edits).
8. The DJ closes the application window with unsaved edits pending. The Quit path
   presents the same `Save / Don't Save / Cancel` prompt; `Cancel` aborts the
   quit and keeps the application open.

## 1.4. Acceptance Criteria

- [ ] A `SessionController` class exists at
  `Source/Features/Daw/Session/SessionController.h/.cpp`, constructed with
  explicit dependencies (the `daw` `ValueTree`, the PRD-0095 serializer, the
  PRD-0083 undo manager, and the `juce::PropertiesFile` for recents) injected via
  the constructor. No singletons or global mutable state are introduced.
- [ ] **Save** serializes the current `daw` model to the current session path via
  PRD-0095 when a path exists; when no path exists, Save transparently routes to
  Save As.
- [ ] **Save As** presents a `juce::FileChooser` filtered to `*.soniksession`,
  writes the serialized model to the chosen path, adopts that path as the current
  session path, updates the title, and clears the dirty flag.
- [ ] Saving (Save or Save As) appends the `.soniksession` extension if the user
  omitted it, never produces a double extension, and overwrites an existing file
  only after the platform file chooser's native overwrite confirmation.
- [ ] **Open** presents a `juce::FileChooser` filtered to `*.soniksession`,
  deserializes the chosen file via PRD-0095 into the `daw` model, **rebuilds the
  DAW UI** from the reconstructed model, and **restores the persisted view state**
  (zoom level, horizontal scroll, vertical scroll) so the timeline matches its
  saved appearance.
- [ ] After Open, the current session path and title reflect the opened file, the
  dirty flag is clear, and the undo history is reset so the opened state is the
  new undo baseline (the user cannot "undo" past the freshly opened session).
- [ ] **New** discards the current model (after the §1.4 unsaved-changes prompt if
  dirty), installs a fresh empty `daw` model, clears the current path, sets the
  title to `Untitled`, resets the undo history, and clears the dirty flag.
- [ ] The dirty flag is **set** whenever the live model is mutated after the last
  save/open/new, and **cleared** on Save, Save As, Open, and New. Its source of
  truth is PRD-0083's undo-manager change index compared against a stored "saved
  baseline" index (see §1.5.1).
- [ ] Any destructive transition — **New**, **Open**, or application **Quit** —
  while the session is dirty presents a modal `Save` / `Don't Save` / `Cancel`
  prompt. `Cancel` aborts the transition with zero side effects; `Don't Save`
  proceeds discarding changes; `Save` performs a Save (or Save As if untitled)
  first and proceeds only if the save succeeds.
- [ ] A **Recent Sessions** list is persisted in the application
  `juce::PropertiesFile`, capped at a fixed length (see §1.5.3), de-duplicated,
  and ordered most-recent-first. Opening or saving a session promotes its path to
  position 0. A recent entry whose file no longer exists is either removed lazily
  on click or visually marked (see §1.5.3).
- [ ] The UI exposes File-menu actions **New**, **Open…**, **Open Recent ▸**,
  **Save**, and **Save As…**, with the recent submenu populated from the
  persisted list and a **Clear Recent** action at its foot.
- [ ] Keyboard shortcuts are bound and routed through the controller: `Cmd/Ctrl+S`
  → Save, `Shift+Cmd/Ctrl+S` → Save As, `Cmd/Ctrl+O` → Open, `Cmd/Ctrl+N` → New.
- [ ] The current session title and a dirty marker (a trailing `•` glyph) are
  displayed in the DAW UI and mirrored in the OS window title, rendered in the
  `DESIGN.md` monochrome palette (`#2d2d2d` / `#fdfdfd`), `Space Mono` font,
  with no border-radius and no gradient. The unsaved-changes prompt uses the
  monochrome modal dialog style with `2px solid #2d2d2d` buttons and a dithered
  drop shadow.
- [ ] Save, Save As, and Open run entirely off the real-time audio thread; no
  `processBlock` path is touched by this PRD. File I/O occurs on a message-thread
  or background-thread context, never inside any audio callback.
- [ ] Opening a file whose schema version is newer than the running build is
  handled gracefully (see §1.5.7): the controller defers the version decision to
  PRD-0095's migration hook and, if the file is unreadable, surfaces a monochrome
  error dialog and leaves the current model untouched rather than corrupting it.
- [ ] Unit/integration tests under `Tests/` cover: Save-with-path vs
  Save-As-routing, dirty-flag set/clear across mutate/save/open/new, the
  unsaved-changes prompt's three branches (including `Cancel` aborting), recent
  list promotion/dedup/cap, and view-state round-trip (zoom/scroll restored on
  open). A fake serializer and an in-memory `PropertiesFile` are injected so the
  tests need no real disk dialogs.

## 1.5. Grey Areas

### 1.5.1. Dirty-State Detection: Undo Change Index vs ValueTree Listener

Dirtiness can be tracked two ways: (a) compare PRD-0083's undo-manager change
index against a stored "saved baseline" index, or (b) attach a
`ValueTree::Listener` to the `daw` branch and set a boolean on any change.

**Resolution:** Use the undo change index (approach a). It is the single source
of truth that already exists, it is monotonic and cheap to compare, and — most
importantly — it correctly reports a session as **clean** when the user mutates
and then *undoes back to the saved baseline*. A naive ValueTree listener would
latch dirty on the first change and never un-latch on undo, producing false
"unsaved changes" prompts after the user has effectively reverted their edits.
The saved baseline is the change index captured at the moment of the last
successful Save / Save As / Open / New; `isDirty()` is simply
`currentChangeIndex != savedBaselineIndex`. If PRD-0083's undo manager does not
expose a stable monotonic index, this PRD adds a minimal accessor to it rather
than falling back to a listener.

### 1.5.2. Unsaved-Changes Prompt Scope: New / Open / Quit

The prompt must guard every path that discards or replaces the live model. The
question is whether it also guards *opening a recent* (yes — Open Recent is just
Open) and whether it guards in-place reloads.

**Resolution:** The prompt fires on exactly three triggers — **New**, **Open**
(including Open Recent), and application **Quit** — and only when `isDirty()` is
true. A clean session transitions silently. The prompt is modal and blocking:
the destructive action does not proceed until the user answers, and `Cancel`
must fully abort (for Quit, this means vetoing the window close). Save-then-act
is sequential: if the user picks `Save` but the subsequent Save As dialog is
itself cancelled (untitled session), the whole transition is aborted as if the
user had picked `Cancel` — never discard on a half-completed save.

### 1.5.3. Recent List Length and Storage

How many recents, where stored, and what to do with stale entries are open
choices. Options span 5–20 entries, stored in the shared `PropertiesFile`
(used elsewhere by the app) versus a dedicated file.

**Resolution:** Store the recent list in the existing application
`juce::PropertiesFile` (the same store used for other app preferences) under a
single key holding a serialized, ordered, de-duplicated list of absolute paths,
**capped at 10**. Most-recent-first; opening or saving promotes to index 0;
duplicates are collapsed to their newest occurrence. A recent entry whose file no
longer exists is **kept but visually disabled/greyed** in the menu (monochrome
disabled state) and, if the user clicks it anyway, is **removed lazily** with a
brief "file not found" monochrome notice — rather than eagerly scanning the disk
on every menu open, which would stall the message thread. A **Clear Recent**
action empties the list.

### 1.5.4. Default Save Directory and Suggested Filename

Save As must seed the file chooser with a directory and a filename. Choosing
poorly (e.g. the app bundle directory) frustrates the DJ.

**Resolution:** Seed the chooser's directory with, in priority order: (1) the
directory of the current session path if one exists; else (2) the directory of
the most-recent recent-list entry; else (3) the platform user documents
directory (`juce::File::getSpecialLocation (userDocumentsDirectory)`). Seed the
suggested filename with the current title (`My Set.soniksession`) or
`Untitled.soniksession` for a fresh session. This makes "Save As" near a
project's existing siblings the default, matching DJ muscle memory, while never
defaulting into a non-writable or surprising location.

### 1.5.5. Restoring View State vs Resetting on Open

The `.soniksession` stores zoom/scroll (per EPIC-0012 §1.2.1). On open, the
controller can either restore that exact view or reset to a canonical
"fit/home" view.

**Resolution:** **Restore** the persisted zoom and scroll exactly. The whole
point of reopening a session is to resume work where it was left; resetting the
view would force the DJ to re-navigate to their working region every time. The
one guard: if the persisted view state is absent (an older schema migrated by
PRD-0095 that predates view-state capture) or out of valid range, the controller
falls back to a deterministic "fit arrangement to width, scroll to start"
default rather than applying garbage offsets. View-state application happens
**after** the UI rebuild completes, so the restored scroll targets real,
laid-out components.

### 1.5.6. Save vs Save As Path Tracking

After a Save As to a new path, subsequent plain Saves must target the new path,
not the original. The controller must track "the current session path" as
mutable lifecycle state.

**Resolution:** The controller holds a single `currentSessionPath`
(`juce::File`, possibly invalid for an untitled session). Save As sets it; Save
reads it (and routes to Save As when it is invalid); Open sets it; New clears it.
There is exactly one such field — there is never a notion of "original vs current"
path, because Sonik has no separate "revert to original file" concept in this
PRD. Save always means "write to `currentSessionPath`," and Save As always means
"choose a new `currentSessionPath`, then write." This keeps the mental model and
the code path trivially predictable.

### 1.5.7. Opening a Newer Schema Version

A session written by a newer build may carry a schema version the running build
does not understand. This PRD invokes deserialization but does not own schema
logic.

**Resolution:** Defer entirely to PRD-0095's migration hook. The controller
calls the serializer's load entry point and treats the result as a typed
success/failure: on success (including a successful migration), proceed with the
UI rebuild and view-state restore; on failure (unreadable, corrupt, or a
forward-version the migration hook refuses), the controller **leaves the current
live model completely untouched** and surfaces a monochrome error dialog stating
the file could not be opened. Under no circumstances does a failed open partially
mutate the live model — deserialize into a *staging* tree first and swap it in
only on full success, so a mid-parse failure cannot corrupt the DJ's current
work.

### 1.5.8. Shortcut Bindings and Platform Conventions

`Cmd/Ctrl+S`, `Shift+S`, `O`, `N` are near-universal, but Sonik already binds
many keys for transport and deck control; collisions must be avoided.

**Resolution:** Bind the four session shortcuts through the application's central
`juce::ApplicationCommandManager` (the same command target the menu uses), so the
menu items and the keystrokes invoke identical command IDs and there is a single
authoritative binding table. Use the platform modifier (`Cmd` on macOS, `Ctrl`
on Windows) via `juce::ModifierKeys` conventions, with `Shift+Cmd/Ctrl+S` for
Save As. These four commands are scoped to fire only when the DAW view has focus,
so they never shadow a deck/transport shortcut active in a different view; if a
genuine collision with an existing global binding is found during
implementation, the session command keeps the platform-standard key and the
conflicting non-standard binding is the one re-homed.
