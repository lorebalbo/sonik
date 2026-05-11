---
status: Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
  - PRD-0033
---

# 1. PRD-0037: Track Rating and Played Session Indicator

## 1.1. Problem

Professional DJs curate large collections of tracks — often thousands of entries — and must make rapid, high-stakes decisions during a live set. Without a persistent rating system, there is no programmatic way to distinguish a DJ's best, most reliable tracks from rarely used or mediocre ones. The entire library appears uniform, forcing the DJ to rely on memory or external notes, which is error-prone and slows down navigation under pressure.

A second, independent problem occurs during the set itself: without a visual indicator showing which tracks have already been played in the current session, a DJ can accidentally load and play the same track twice. Repeating a track in a live performance is one of the most visible and embarrassing mistakes a DJ can make in front of an audience. The current library view provides no mechanism to prevent this.

These two gaps — absence of a rating hierarchy and absence of a session-scoped played indicator — degrade both the preparation workflow and the live performance experience.

## 1.2. Objective

The system enables the DJ to assign a 1–5 star rating to any track directly from the library row without opening a dialog. Ratings persist across sessions in the `library_tracks.rating` database column and are available as a search scope operator (`rating:N` filters for tracks with rating >= N) and as a sort axis in the track table.

The system also marks each track row with a small, pixel-art visual indicator the moment the track begins playback for the first time in the current application session. This indicator is session-scoped: it is never written to the database and resets automatically when the application launches. It provides an at-a-glance record of what has already been played, allowing the DJ to avoid repetition without breaking flow.

## 1.3. User Flow

### 1.3.1. Rating a Track

The DJ opens the library panel. The track table contains a Rating column displaying five star-glyph positions per row. Each star is rendered as a pixel-art filled-square block glyph (consistent with the design system's monochrome palette). Filled glyphs represent the current rating; empty glyphs represent unset positions.

The DJ clicks the Nth star glyph in any row. The UI immediately fills positions 1 through N (optimistic update). In the background, the system writes the new integer value to `library_tracks.rating` for that track's ID. If the write fails, the UI reverts to the previous state and surfaces an error notification.

If the DJ clicks the currently active star (the highest filled position), the rating resets to 0 (unrated). All five positions render as empty glyphs.

The DJ can rate a track whose file is marked missing (`is_missing = true`). Rating is metadata and is independent of file availability; the inline interaction behaves identically to a present track.

The DJ can rate a track while it is currently playing on a deck. The rating update proceeds normally; playback is unaffected.

### 1.3.2. Filtering and Sorting by Rating

The DJ types `rating:4` into the search bar. The library displays only tracks with `rating >= 4`. The operator follows the minimum-match semantic: `rating:4` includes 4-star and 5-star tracks.

The DJ clicks the Rating column header to sort the library by rating in descending order. A second click sorts ascending. Unrated tracks (rating = 0) sort last in descending order and first in ascending order.

### 1.3.3. Played Session Indicator

The DJ loads a track onto a deck and starts playback. At the moment playback begins (the same event that increments `play_count`), the library row for that track gains a played indicator: a small filled-square glyph (`■`) rendered in the primary foreground colour (`#000000` on a light-mode row, inverted on a selected/highlighted row). The indicator appears in a dedicated Played column positioned at the leftmost edge of the row or as a prefix mark within the track-title cell — whichever the layout specification defines.

If the DJ loads the same track on a second deck simultaneously, the played indicator does not duplicate or change state. A single mark is shown regardless of how many decks carry the track.

The played indicator persists for the duration of the application session. Closing and relaunching the application clears all played indicators; no session data is written to the database.

The DJ can filter the library to hide already-played tracks by activating a "Hide played" toggle in the library toolbar. This removes rows with an active played indicator from the current view without affecting the underlying data.

## 1.4. Acceptance Criteria

### 1.4.1. Rating Column

- AC-01: The track table includes a Rating column. Each row in the column renders exactly five star-position glyphs using the pixel-art filled-square block character defined in the design system. No SVG icons, no system font characters, no color fills.
- AC-02: Filled glyphs (positions 1 through `rating`) render in `#000000`. Empty glyphs (positions `rating+1` through 5) render as the same glyph shape with reduced opacity or as an outline variant, remaining monochrome and consistent with the design system.
- AC-03: A track with `rating = 0` renders all five positions as empty glyphs.
- AC-04: Clicking the Nth star glyph in a row sets that track's rating to N. The visual state updates synchronously (optimistic update) before the database write completes.
- AC-05: Clicking the star that matches the current rating (i.e., the rightmost filled glyph) resets the rating to 0. All five positions render as empty immediately.
- AC-06: The optimistic UI update is followed by an asynchronous write to `library_tracks.rating`. If the write fails, the row reverts to its previous rating value and a non-blocking error notification is displayed.
- AC-07: Rating interaction is available for tracks whose `is_missing` flag is `true`. The interaction and persistence behaviour is identical to present tracks.
- AC-08: Rating interaction is available while the track is currently loaded and playing on any deck. Playback is not interrupted or affected.
- AC-09: Rating values persist across application sessions. Closing and relaunching the application restores the previously saved rating for each track.
- AC-10: There is no modal dialog or confirmation step for rating changes. The interaction is entirely inline within the table row.

### 1.4.2. Rating Filter and Sort

- AC-11: The search scope operator `rating:N` (where N is 1–5) filters the library to display only tracks with `rating >= N`. Tracks with `rating = 0` are excluded from all `rating:N` queries.
- AC-12: The Rating column header is clickable and triggers table sorting. Descending sort places 5-star tracks first; unrated tracks (rating = 0) appear last. Ascending sort places unrated tracks first. A visual sort indicator (arrow glyph, pixel-art) reflects the current sort direction.

### 1.4.3. Played Session Indicator

- AC-13: A Played column (or per-row prefix glyph) is present in the track table. It is empty for all tracks at application launch.
- AC-14: The played indicator activates the moment playback starts on a deck for a given track. "Playback start" is defined as the same event that triggers the `play_count` increment. The indicator does not activate on track load alone; it requires transport play.
- AC-15: The played indicator is a single pixel-art filled-square glyph (`■`) rendered in the primary foreground colour of the row's current state. On a default (unselected) row it renders in `#000000`. On an inverted (selected) row it renders in `#f9f9f9` (or the row background colour), preserving contrast. No colour other than the monochrome palette is used.
- AC-16: Loading the same track on two or more decks simultaneously does not produce duplicate played indicators. The row displays a single indicator regardless of the number of decks carrying the track.
- AC-17: The played indicator is session-scoped. It is not written to any database table or file. Closing and relaunching the application clears all played indicators; `play_count` in the database is unaffected by this reset.
- AC-18: The played indicator does not alter the row's height, truncate text cells, or otherwise degrade the readability of other columns.
- AC-19: A "Hide played" toggle control is available in the library toolbar. When active, rows with an active played indicator are hidden from the library view. Deactivating the toggle restores full visibility. This filter composes with other active filters (text search, `rating:N`, tag filters) using AND logic.
- AC-20: The played indicator activates even if the DJ starts playback from a cue point, a hot-cue, or a loop — any transport play event qualifies.

### 1.4.4. Data Integrity and Edge Cases

- AC-21: If the application crashes after a rating change has been displayed in the UI but before the database write has completed, the previous rating value is preserved in the database. The implementation must use a synchronous write strategy or a write-ahead flag to minimise the risk of a divergence between the displayed and persisted state. The known residual risk must be documented in the implementation notes.
- AC-22: When a track is loaded onto a deck and its `play_count` is incremented, the session-played state for that track ID is set in an in-memory data structure (e.g., a `std::unordered_set<TrackID>` on the UI thread). All library rows referencing the same track ID read from this shared structure to render the played indicator.
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