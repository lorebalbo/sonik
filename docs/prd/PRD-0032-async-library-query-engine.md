---
status: Implemented
epic: EPIC-0004
depends-on:
  - PRD-0030
---

# 1. PRD-0032: Async Library Query Engine

## 1.1. Problem

DJs managing large collections of 50,000 or more tracks face an unusable library experience when search queries execute on the JUCE Message Thread. Because the Message Thread is also responsible for rendering every UI frame, a single synchronous SQLite query that takes 80–200 ms causes the entire application window to freeze: knobs stop responding, the waveform display stutters, and the transport controls become unresponsive. This is unacceptable in a live performance context where a DJ must be able to search for the next track while the current one is playing.

The problem is compounded by fast typists. Each keystroke in the search bar triggers a new query. Without a debounce mechanism, a user typing "deadmau5 128" fires eleven successive queries in under 300 ms, several of which are still executing when the next keystroke arrives. This creates a query storm: competing SQLite reads contend for the same connection, intermediate results are painted to the table only to be immediately overwritten, and the perceived search latency increases rather than decreases with collection size.

A third dimension of the problem appears when deck state changes mid-search. The library offers Deck-Aware BPM filtering: it narrows the displayed collection to tracks whose BPM falls within a window of the active deck's effective tempo. When the DJ adjusts the pitch fader on a playing deck, the effective tempo changes continuously. Without a mechanism to cancel the in-flight query and reissue it with updated BPM bounds, the table briefly displays results that are already stale, confusing the DJ about which tracks are actually compatible.

Finally, exposing the raw SQLite `sqlite3*` handle across threads without ownership rules creates data races. SQLite in its default build is not thread-safe for shared connections. Any approach that runs queries from two threads on the same connection — even with a mutex — introduces latency jitter as threads block waiting for the lock to clear.

## 1.2. Objective

The system provides a persistent `LibraryQueryThread` that:
- Moves all read queries off the Message Thread so that search, sort, and filter operations never block UI rendering or transport controls, regardless of collection size.
- Debounces search input at exactly 150 ms so that rapid keystrokes are consolidated into a single query dispatch, eliminating query storms.
- Owns an exclusive `sqlite3*` connection that is opened on the `LibraryQueryThread` at construction and never accessed from any other thread, removing all need for a mutex over the SQLite connection.
- Cancels any in-flight query when a newer query request arrives, so the table always reflects the most recent user intent rather than a race between competing results.
- Parses Traktor-style scope operators client-side before SQL dispatch, translating the seven supported prefixes (`bpm:`, `key:`, `rating:`, `title:`, `artist:`, `album:`, and bare words) into parameterized SQL clauses that are safe from injection.
- Returns results via `juce::MessageManager::callAsync` so that the result buffer swap happens on the Message Thread, keeping the `juce::TableListBox` consistent with JUCE's single-threaded rendering model.
- Supports reactive re-dispatch when deck state changes so the Deck-Aware BPM filter always reflects the current effective tempo without the DJ having to manually re-enter the search query.

## 1.3. User Flow

1. The application launches. `LibraryQueryThread` starts, opens its own `sqlite3*` connection to the same database file used by `TrackDatabase`, and immediately dispatches a blank query that returns the full `library_tracks` table sorted by `date_added DESC`. The result buffer is populated and the `juce::TableListBox` shows the entire collection.

2. The DJ types "dead" into the search bar. A `juce::Timer` on the Message Thread resets to 150 ms. No query is dispatched yet.

3. Within 150 ms the DJ continues typing, entering "deadmau5". The timer resets on each keystroke. After the last keystroke, 150 ms elapse without further input. The timer fires, the scope operator parser runs on the raw string, finds no prefix operators, and dispatches a full FTS query: `SELECT ... FROM library_tracks JOIN library_fts ON ... WHERE library_fts MATCH 'deadmau5*' ...`. The query is placed in the `LibraryQueryThread`'s pending-query slot.

4. The `LibraryQueryThread` wakes, reads the pending query, opens a `sqlite3_stmt`, and begins stepping through rows. After collecting all results it calls `juce::MessageManager::callAsync`, which schedules a lambda on the Message Thread that swaps the result buffer and calls `updateContent()` on the `juce::TableListBox`. The table repaints showing all matching tracks.

5. The DJ refines the search by typing " bpm:128-135" after the artist name, producing the full string "deadmau5 bpm:128-135". The timer resets and fires 150 ms after the final keystroke. The parser identifies the `bpm:` range operator and extracts lower bound 128 and upper bound 135. It also extracts the bare word "deadmau5" as an FTS term. The engine builds a parameterized query joining `library_fts MATCH` with a `WHERE bpm BETWEEN ? AND ?` clause, binding the sanitized values via `sqlite3_bind_double`.

6. While the previous query is still stepping rows, the new query arrives. `LibraryQueryThread` sets its cancellation flag. The current query checks the flag between every `sqlite3_step` call, sees it set, finalizes the statement immediately without posting results, and begins the new query instead.

7. The DJ changes the pitch fader on Deck A. `LibraryComponent` receives a `valueTreePropertyChanged` callback for `IDs::speedMultiplier` on the Deck A subtree. It recomputes `DeckAwareFilterState` (effective BPM = `beatgrid_bpm × speedMultiplier`), merges the updated BPM window into the current query parameters, resets the 150 ms debounce timer, and re-dispatches the query when the timer fires. The table updates to reflect the new effective tempo without the DJ retyping anything.

8. The DJ clears the search bar entirely. The parser receives an empty string, produces a blank query (no `MATCH` clause, no `WHERE` clause beyond any active Deck-Aware filters), and dispatches it. The full collection is returned, sorted by the currently active sort column.

9. The DJ clicks the "BPM" column header. A sort request is merged into the current query parameters (no debounce needed for sort changes; the re-dispatch is immediate). The result buffer is refreshed with the same filter but `ORDER BY bpm ASC`.

10. The DJ types a search string containing a single quotation mark (e.g., `d'n'b`). The scope operator parser passes the term to `sqlite3_bind_text` as a bound parameter. The single-quote character reaches the database as data, not SQL syntax. No injection is possible.

11. The query returns zero rows. The result buffer is swapped to an empty vector. The `juce::TableListBox` shows zero rows. A status label below the search bar reads "No tracks found." The DJ can continue typing to refine the query.

12. The application quits. `LibraryQueryThread::stopThread(2000)` is called. The thread checks the cancellation flag, abandons any in-flight query, finalizes any open statement, closes the `sqlite3*` connection with `sqlite3_close`, and exits cleanly.

## 1.4. Acceptance Criteria

- [ ] `LibraryQueryThread` is a `juce::Thread` subclass that is constructed once and never recreated per query. It opens its own `sqlite3*` connection in its `run()` method before entering the query-dispatch loop, and closes it before `run()` returns.
- [ ] The `sqlite3*` handle owned by `LibraryQueryThread` is never accessed from the Message Thread or any other thread. No mutex guards it, because no other thread touches it.
- [ ] `LibraryQueryThread` maintains a single pending-query slot (not a queue of unbounded depth). When a new query is dispatched, it atomically replaces the slot, discarding any unprocessed prior query that had not yet been picked up.
- [ ] When a new query arrives while the thread is actively stepping through rows, the thread sets a `std::atomic<bool>` cancellation flag before placing the new query in the slot. The in-flight query checks this flag between every `sqlite3_step` call, finalizes the statement immediately upon detecting it, and does not post a `callAsync` result for the cancelled query.
- [ ] The 150 ms debounce is implemented as a `juce::Timer` on the Message Thread. The timer resets (restarts its countdown) on every keystroke in the search bar. The query is dispatched only when the timer fires, meaning 150 ms have elapsed since the last keystroke.
- [ ] Sort changes and Deck-Aware filter changes (from `valueTreePropertyChanged`) bypass the 150 ms debounce and trigger an immediate re-dispatch of the current query with updated parameters.
- [ ] An empty search string is a valid query that returns the entire `library_tracks` table, subject to any active Deck-Aware filters, sorted by the currently active sort column.
- [ ] The scope operator parser runs on the Message Thread before dispatching. It scans the raw search string left-to-right and extracts zero or more prefix tokens in the following forms:
    - `bpm:N` — exact BPM match, tolerates ±0.5; maps to `WHERE bpm BETWEEN ? AND ?` with bounds `N - 0.5` and `N + 0.5`
    - `bpm:N-M` — BPM range; maps to `WHERE bpm BETWEEN ? AND ?` with bounds `N` and `M`
    - `key:X` — exact key filter; maps the key string to its `key_index` integer and generates `WHERE key_index = ?`
    - `rating:N` — minimum star rating; maps to `WHERE rating >= ?`
    - `title:word` — field-scoped FTS; the FTS `MATCH` clause targets only the `title` column
    - `artist:word` — field-scoped FTS; the FTS `MATCH` clause targets only the `artist` column
    - `album:word` — field-scoped FTS; the FTS `MATCH` clause targets only the `album` column
    - Bare words (no recognized prefix): contribute to a full FTS `MATCH` across `title`, `artist`, and `album`
- [ ] A single search string may contain multiple operators simultaneously. The string `artist:deadmau5 bpm:128-135` is parsed into an `artist:` FTS clause AND a `bpm:` range clause, both applied in the same SQL query. Bare words present alongside prefix operators are appended as additional FTS terms.
- [ ] All user-supplied values (search terms, BPM values, key index, rating) are passed to SQLite exclusively via `sqlite3_bind_*` functions. No user input is ever interpolated directly into the SQL string. This applies to all query variants including FTS MATCH queries.
- [ ] FTS queries use a trailing wildcard: the term extracted from the search string is appended with `*` in the `MATCH` expression (e.g., `MATCH 'deadm*'`) to support prefix matching. This wildcard is appended in code, not supplied by the user, so it cannot be exploited.
- [ ] If the `bpm:` operator is present in the search string and the Deck-Aware BPM filter is also active, both BPM constraints are applied: the explicit `bpm:` clause from the user's query and the effective-tempo window from deck state. The intersection of both is used (the narrower range wins).
- [ ] The Deck-Aware BPM filter reads `beatgrid_bpm × speedMultiplier` from the deck's ValueTree subtree using `IDs::bpm` and `IDs::speedMultiplier`. It never accesses the audio thread or any `std::atomic` on the audio path. The effective tempo is a property read on the Message Thread via `valueTree.getProperty()`.
- [ ] `LibraryComponent` registers as a `juce::ValueTree::Listener` on the root ValueTree and listens to `valueTreePropertyChanged` for `IDs::speedMultiplier` and `IDs::bpm` on any Deck subtree. On each change it recomputes `DeckAwareFilterState` and schedules a re-dispatch via the immediate (no debounce) path.
- [ ] After a query completes (all rows stepped without cancellation), the thread calls `juce::MessageManager::callAsync` with a lambda that captures the result vector by move, swaps it into the `LibraryComponent`'s result buffer, and calls `tableListBox.updateContent()`. No result data is touched outside the Message Thread after this point.
- [ ] The result buffer is a `std::vector<LibraryTrackRow>` that is only ever read or written on the Message Thread. `paintCell()` reads from this buffer without any synchronization because it also runs on the Message Thread. No lock is required.
- [ ] `juce::TableListBox::getNumRows()` returns `resultBuffer.size()`. `paintCell()` reads `resultBuffer[rowNumber]` directly. No database access occurs inside `paintCell()`, `paintRowBackground()`, or `selectedRowsChanged()`.
- [ ] Sort is supported on the columns: title (alphabetical), artist (alphabetical), BPM (numeric), key (by `key_index`), rating (numeric), and duration (numeric). The active sort column and direction are stored as part of the query parameters and appended as an `ORDER BY` clause in the generated SQL. Clicking an already-active sort column reverses the direction.
- [ ] If the database file is locked or returns `SQLITE_BUSY` during a query step, the thread retries up to 5 times with a 10 ms busy-wait between attempts (`sqlite3_busy_timeout` is set to 50 ms on the thread's connection at open time). If all retries are exhausted, the query is abandoned, no result is posted, and a warning is written to the JUCE debug log.
- [ ] The `LibraryQueryThread` is owned by `LibraryComponent` (or its parent owner), not by `SonikApplication`. It is constructed in `LibraryComponent`'s constructor and destroyed in its destructor. Destruction calls `stopThread(2000)`, which signals the thread to exit and waits up to 2 seconds for clean shutdown.
- [ ] The cancellation flag is a `std::atomic<bool>` member of `LibraryQueryThread`. It is set to `true` before placing a new query in the pending slot and reset to `false` at the start of each new query execution, before the first `sqlite3_step` call.
- [ ] All code implementing `LibraryQueryThread`, the scope operator parser, and the result buffer swap resides under `Source/Features/Library/`. No header from `Source/Features/Deck/`, `Source/Features/AudioEngine/`, or any other Feature module is included. Cross-feature data (deck BPM, speedMultiplier) is read exclusively through ValueTree property keys from `DeckIdentifiers.h`.

## 1.5. Grey Areas

### 1.5.1. Mixed Operator Parsing Ambiguity

A search string such as `bpm:128 funky` contains both a `bpm:` operator and a bare word. The parser must treat the bare word "funky" as a full-FTS term (not as part of the BPM value). Parsing is left-to-right; a token is classified as a prefix operator if it matches a recognized `prefix:value` pattern and as a bare word otherwise. Unrecognized prefixes (e.g., `genre:house`) are treated as bare words in their entirety, including the colon and the subsequent value, and forwarded as a single FTS term.

### 1.5.2. Key Index Mapping

The `key:Am` operator requires mapping a human-readable key string to the integer `key_index` stored in `library_tracks`. The mapping table (Camelot wheel notation and standard notation) is defined in `KeyDetector` (PRD-0009) and must be reused here rather than duplicated. If the supplied key string does not resolve to a known key index, the `key:` clause is silently dropped and the remainder of the query is executed without the key filter.

### 1.5.3. BPM Tolerance for Deck-Aware Filter vs. Explicit bpm: Operator

The `bpm:` scope operator uses a fixed ±0.5 BPM tolerance for exact matches and exact bounds for range matches. The Deck-Aware BPM VISION window is a user-configurable parameter (default ±6 BPM, stored in application settings). These are two independent mechanisms and must not share state. If both are active simultaneously, the narrower effective range is applied.
Each criterion must be binary: either it is met or it is not. Avoid ambiguous or subjective criteria.
-->