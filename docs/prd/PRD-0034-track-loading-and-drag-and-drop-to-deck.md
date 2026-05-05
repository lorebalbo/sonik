---
status: Not Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0033
  - PRD-0001
---
# 1. PRD-0034: Track Loading and Drag-and-Drop to Deck

## 1.1. Problem

Without a mechanism to load tracks from the library panel into a deck, the library is a read-only display that is entirely disconnected from the DJ's live performance workflow. The DJ is forced to rely on operating system file dialogs to open audio files, which breaks their focus, interrupts session continuity, and is incompatible with the fast-paced demands of a live set. There is no visual feedback confirming which deck a track will be loaded into, no handling for tracks whose files have been moved or deleted, and no integration with the deck's playback state. The absence of this feature makes the application non-functional as a DJ tool.

## 1.2. Objective

The user is able to load any track from the library table into any available deck through three interaction methods: drag-and-drop, a right-click context menu, and double-click. The system ensures that:
- The correct deck receives the track path via a `juce::ValueTree` property write, maintaining strict module isolation between `Library` and `Deck` features.
- Missing files are detected synchronously before dispatch and the user is offered a relocate or remove option.
- Unsupported file formats are rejected with clear user feedback before any deck state is modified.
- A deck currently playing accepts a new track load, but its SYNC state is automatically disengaged.
- The play count for a track is incremented in the `library_tracks` database table at the moment playback begins on the deck, not at the moment of loading.
- The library table reflects a "Now Playing" row indicator in real time whenever a track is active on any deck.

## 1.3. User Flow

### 1.3.1. Drag-and-Drop

The DJ initiates a drag gesture on any row in the library table. The row becomes the drag source. As the pointer moves over a deck's designated drop zone, the deck header region inverts its colours (binary black/white inversion, no animation, no border-radius, zero blur) to signal that a drop is accepted. If the pointer leaves all drop zones, the visual state of all decks reverts to normal. If the drag is cancelled (mouse released outside any drop zone, or Escape pressed), no state changes on any deck. If the drag is released over a valid drop zone, the system performs the missing file check and, on success, writes the file path to the target deck's `IDs::pendingLoadPath` ValueTree property.

### 1.3.2. Right-Click Context Menu

The DJ right-clicks any row in the library table. A context menu appears listing one entry per configured deck: "Load to Deck 1", "Load to Deck 2", and so on, up to the number of decks active in the current session layout (two-deck or four-deck). The list is generated dynamically at menu-open time by reading the deck subtrees present in the root ValueTree. Selecting an entry triggers the same missing file check and ValueTree write path as drag-and-drop. If the DJ has multiple rows selected, only the row that was right-clicked is loaded; the multi-selection is not affected.

### 1.3.3. Double-Click

The DJ double-clicks any row in the library table. The system selects the target deck using the following priority:
1. The first deck (by index order) whose `IDs::loadedFilePath` ValueTree property is empty (no track loaded).
2. If all decks have a track loaded, deck 1 is used as the fallback target.

The same missing file check and ValueTree write path applies.

### 1.3.4. Missing File Handling

Before any ValueTree write, the system resolves the absolute path stored in `file_path` for the selected track and checks for the file's existence synchronously on the message thread. If the file is absent, a modal dialog is presented offering two options:
- "Relocate": opens a `juce::FileChooser` filtered to supported audio formats. On confirmation, the selected path replaces the stored `file_path` in the `library_tracks` SQLite table and the load proceeds.
- "Remove from Library": removes the track row from the `library_tracks` table and dismisses the dialog without loading anything.

### 1.3.5. Unsupported Format Rejection

If the resolved file exists but its extension or MIME type is not in the set of formats supported by `AudioFileLoader`, the load is rejected before any ValueTree write. A non-blocking notification message is shown to the user (inline label or brief overlay in the library panel) stating that the format is not supported. No deck state is modified.

### 1.3.6. SYNC Auto-Disengage on Playing Deck

If the target deck is currently playing (its `IDs::isPlaying` ValueTree property is `true`) at the time the `IDs::pendingLoadPath` property is written, the deck's SYNC state (`IDs::syncEnabled`) is set to `false` as part of the same atomic ValueTree modification group before the load is dispatched to `AudioFileLoader`.

### 1.3.7. Play Count and Now Playing

The `play_count` column in `library_tracks` is incremented by one for the loaded track the first time `IDs::isPlaying` transitions from `false` to `true` on the deck after that track was loaded. It is not incremented at load time. A "Now Playing" indicator (a filled pixel-art marker in the leftmost column of the library table row) is applied to the row corresponding to the track currently loaded and playing on any deck. The indicator is cleared when the track is unloaded or replaced.

## 1.4. Acceptance Criteria

AC-01: A drag gesture initiated on any library table row and released over a deck drop zone loads the corresponding track into that deck by writing its absolute file path to `IDs::pendingLoadPath` on the target deck's ValueTree subtree.

AC-02: While a drag is in progress and the pointer is over a deck's drop zone, the deck header region displays the binary-inverted colour state (foreground and background colours swapped, no animation, no border-radius, no shadow blur) exclusively for the hovered deck.

AC-03: When the pointer moves off a deck drop zone during a drag, that deck's header reverts immediately to its normal colour state without any transition animation.

AC-04: If a drag is cancelled by releasing the mouse outside all deck drop zones, or by pressing Escape, no `IDs::pendingLoadPath` write occurs and all deck visual states remain unchanged.

AC-05: Right-clicking a library table row opens a context menu. The menu contains exactly one "Load to Deck N" entry per deck present in the current session (e.g., two entries for a two-deck layout, four entries for a four-deck layout). The deck list is read dynamically from the root ValueTree at the moment the menu opens.

AC-06: Selecting "Load to Deck N" from the right-click context menu triggers the same missing file check and ValueTree write path as a drag-and-drop load.

AC-07: When the DJ right-clicks a row while multiple rows are selected in the library table, only the right-clicked row's track is loaded; the multi-selection state is preserved and no other tracks are dispatched.

AC-08: Double-clicking a library table row loads the track into the first deck (by ascending index) whose `IDs::loadedFilePath` ValueTree property is empty. If no such deck exists, the track is loaded into deck 1.

AC-09: Before any `IDs::pendingLoadPath` write, the system checks synchronously on the message thread that the file at the stored `file_path` exists on disk. This check must complete before any deck state is modified.

AC-10: If the file existence check fails, a modal dialog is presented offering exactly two options: "Relocate" and "Remove from Library". No `IDs::pendingLoadPath` write occurs until the user resolves the dialog.

AC-11: Selecting "Relocate" opens a `juce::FileChooser` filtered to supported audio extensions. On user confirmation of a valid file, the new absolute path is persisted to the `file_path` column in `library_tracks` via the SQLite database and the load then proceeds to the ValueTree write.

AC-12: Selecting "Remove from Library" in the missing file dialog removes the track row from the `library_tracks` table, dismisses the dialog, and does not load anything into any deck.

AC-13: If the file exists but its format is not supported by `AudioFileLoader`, the system rejects the load before writing any ValueTree property. A non-blocking error message is displayed within the library panel informing the user that the file format is not supported.

AC-14: No header file from `Source/Features/Deck/` is included anywhere in `Source/Features/Library/`. The only cross-module coupling is the root `juce::ValueTree` reference passed at construction time to the Library feature's top-level component or controller.

AC-15: When the target deck's `IDs::isPlaying` property is `true` at the time a load is initiated, the deck's `IDs::syncEnabled` property is set to `false` in the same ValueTree modification as, or strictly before, the `IDs::pendingLoadPath` write.

AC-16: The `play_count` column in `library_tracks` is incremented by exactly one the first time `IDs::isPlaying` transitions from `false` to `true` on a given deck after a new track has been written to that deck's `IDs::loadedFilePath`. It is not incremented at load time, and not incremented on subsequent play/pause toggles for the same loaded track.

AC-17: The library table row corresponding to the track currently loaded and playing on any deck displays a "Now Playing" indicator (a filled pixel-art marker in the leftmost table column). The indicator appears when `IDs::isPlaying` becomes `true` for that track and is cleared when the track is unloaded or replaced on all decks.

AC-18: The "Now Playing" indicator is cleared from a row if the track is loaded into a deck but has never started playing, or if it is replaced by another track load on the same deck.

AC-19: In a four-deck layout, loading a track to deck 3 or deck 4 via any of the three interaction methods (drag-and-drop, right-click, double-click) works identically to loading to deck 1 or deck 2. The context menu dynamically lists all four decks.

AC-20: When a double-click load finds no empty deck and falls back to deck 1, and deck 1 is currently playing, the SYNC auto-disengage rule (AC-15) applies before the load is dispatched.

AC-21: If the drag source row belongs to a track that was already playing on a deck, dragging it to a different deck initiates a new load on the target deck. The source deck is not stopped as a result of the drag action alone.

AC-22: The context menu rendered on right-click conforms to the design system: dithered shadow (2 px offset, 50% checkerboard pattern, zero blur), zero border-radius, strictly monochrome palette (#000000 / #f9f9f9).

AC-23: All three loading interactions (drag-and-drop, right-click, double-click) write exclusively to `IDs::pendingLoadPath` on the target deck's ValueTree subtree and do not call any method on a `Deck` or `AudioFileLoader` object directly from within any `Library` class.
<!--
Describe step by step how the user interacts with this feature.
Use a numbered list that follows the user's real journey, from entry to exit.
Also include relevant alternative cases (e.g., errors, empty states, missing permissions).
-->

## 4. Acceptance Criteria
<!--
List the criteria that must be met for the feature to be considered complete.
Use a checklist in the format "[ ] The system/user...".
Each criterion must be binary: either it is met or it is not. Avoid ambiguous or subjective criteria.
-->