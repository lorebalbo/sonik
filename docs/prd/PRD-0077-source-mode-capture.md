---
status: Implemented
epic: EPIC-0009
depends-on:
  - PRD-0062
  - PRD-0072
  - PRD-0073
---

# 1. PRD-0077: Source-Mode Capture

## 1.1. Problem

By the time this PRD is reached, the capture stack can already open, grow, and
close clips from deck transport events: the performance-event bridge (PRD-0072)
surfaces structural deck/mixer events to the message thread over a lock-free
FIFO, and the clip-placement engine (PRD-0073) turns those events into growing
`DawClip` crops on the correct channel-group lane(s). But the lane(s) a clip is
written to are resolved **once, at open time**, from the deck's source mode as it
stands when Play fires. A deck's source mode is not static: PRD-0062 gives every
deck an explicit `sourceMode` ValueTree property (`"original"` / `"stems"`) plus
the per-stem "VOC" / "INST" mute toggles, and the DJ flips them live, mid-set, as
a performance gesture — dropping the acapella in over an instrumental, pulling the
original back for a pristine breakdown, muting vocals for a tool edit.

When that happens during recording, nothing currently re-targets the open clip.
A deck that started in "STEMS" with both stems audible (writing onto the
`Instrumental` + `Vocal` lanes) and is switched to "ORIG" mid-phrase keeps
growing the two stem-lane clips, even though the audible source is now the
undivided original file that belongs on the `Original` lane. The arrangement no
longer reflects what the DJ did. EPIC-0009 §1.2.1 lists **source-mode capture**
as in-scope precisely to close this gap: a source-mode change during recording
must switch which lane(s) subsequent material is written to, **with no gap on the
timeline**.

The signal to react to already exists. PRD-0062 publishes the `sourceMode`
ValueTree property as the single source of truth, mirrored by a lock-free
`SourceModeReader`, and defines the exact source-mode → lane mapping that
EPIC-0008's live projection consumes. What is missing is the **capture-side
reaction**: detecting a source-mode (or mute) change as a structural event,
closing the active clip(s) on the outgoing lane(s) at the switch point, and
opening new clip(s) on the incoming lane(s) at the same timeline position and the
same instantaneous deck source position, contiguous.

## 1.2. Objective

The system captures deck source-mode changes during recording such that:

- A change to a recording deck's published source state — either the `sourceMode`
  property flipping (`"original"` ↔ `"stems"`) or a per-stem mute toggle ("VOC" /
  "INST") changing while in `"stems"` mode — is surfaced to the recording session
  controller as a single structural **source-mode-change event** over the
  PRD-0072 performance-event bridge, carrying the deck id, the deck's
  instantaneous source-sample position, and the timeline sample position at the
  switch instant.
- On that event, the clip-placement engine (PRD-0073) **closes every active clip
  on the deck's outgoing lane(s)** — finalising `sourceEndSample` at the deck
  source position and `timelineEndSample` at the playhead — and **opens a new
  clip on each of the deck's incoming lane(s)**, with `sourceStartSample` set to
  the same instantaneous deck source position and `timelineStartSample` set to
  the same playhead value, so the new clip(s) are contiguous with the closed
  one(s) on the timeline (no gap, no overlap).
- The incoming and outgoing lane sets are derived from PRD-0062's published
  source-mode → lane mapping, evaluated immediately before and immediately after
  the change:

```text
Deck source state                         → Lanes written
├─ sourceMode == "original"               → Original
├─ sourceMode == "stems", VOC + INST on   → Instrumental + Vocal
├─ sourceMode == "stems", VOC muted       → Instrumental
└─ sourceMode == "stems", INST muted      → Vocal
```

- **Multi-lane transitions are handled as a set difference**: switching
  `"original"` → `"stems"` (both audible) closes the one `Original` clip and opens
  two contiguous clips on `Instrumental` and `Vocal`; switching `"stems"` (both
  audible) → `"original"` closes two clips and opens one. Lanes present in both
  the before-set and after-set (e.g. `Instrumental` when only "VOC" is toggled)
  have their clip closed and a fresh contiguous clip re-opened on the same lane,
  so every lane transition is expressed as a clean clip boundary.
- The switch is captured **only for decks that are actively recording** (have an
  open clip, i.e. are playing and unmuted). A source-mode change on a stopped or
  silent deck updates no clips; it only affects the lane(s) the *next* play event
  will open on, exactly as PRD-0073 already resolves lanes at open time.
- All reaction occurs on the **message thread**. The audio thread only enqueues
  the POD source-mode-change event into the pre-allocated FIFO (when the change
  originates audio-side); no allocation, lock, or I/O is added to any audio-thread
  path. The `sourceMode` ValueTree property remains the single source of truth
  per PRD-0062.

## 1.3. User Flow

1. The DJ arms Record. Deck A is playing in "STEMS" mode with both stems audible.
   Two clips grow on Deck A's channel group: one on `Instrumental`, one on
   `Vocal`.
2. Thirty seconds in, the DJ clicks Deck A's source-mode toggle to "ORIG". The
   message thread sets `sourceMode = "original"` (PRD-0062). The performance-event
   bridge surfaces a source-mode-change event for Deck A with the deck's current
   source position and the current playhead.
3. The clip-placement engine evaluates the before-set (`Instrumental` + `Vocal`)
   and after-set (`Original`). It closes the two stem-lane clips at the switch
   point and opens one new clip on the `Original` lane, starting at the same
   timeline position and the same deck source position. The arrangement now shows
   two stem clips ending and one original clip beginning, edge-to-edge.
4. Later the DJ returns to "STEMS" but with "VOC" muted. A source-mode-change
   event fires; the before-set is `Original`, the after-set is `Instrumental`.
   The `Original` clip closes; one new `Instrumental` clip opens contiguously.
5. The DJ un-mutes "VOC" (still in "STEMS"). The before-set is `Instrumental`, the
   after-set is `Instrumental` + `Vocal`. The `Instrumental` clip is closed and a
   fresh `Instrumental` clip plus a new `Vocal` clip open contiguously (per the
   re-open-shared-lanes resolution, §1.5.1).
6. The DJ stops Deck A. All open clips on its lanes close at the stop point via
   the existing PRD-0073 transport-stop path; subsequent source-mode toggles while
   stopped change no clips.

## 1.4. Acceptance Criteria

- [ ] A change to a recording deck's `sourceMode` property (`"original"` ↔
      `"stems"`) is detected and surfaced to the recording session controller as a
      single source-mode-change structural event via the PRD-0072 performance-event
      bridge, carrying deck id, instantaneous source-sample position, and timeline
      sample position.
- [ ] A per-stem mute toggle ("VOC" or "INST") changing while the deck is in
      `"stems"` mode is surfaced as the **same** source-mode-change event type (see
      §1.5.2), with the same payload fields.
- [ ] On a source-mode-change event for a deck with open clips, every active clip
      on the deck's outgoing lane(s) is closed with `sourceEndSample` = the event's
      deck source position and `timelineEndSample` = the event's playhead position.
- [ ] On the same event, a new clip is opened on each incoming lane with
      `sourceStartSample` = the event's deck source position and
      `timelineStartSample` = the event's playhead position, so each new clip is
      contiguous with the closed clip on the timeline (no gap, no overlap).
- [ ] Incoming and outgoing lane sets are computed from PRD-0062's published
      source-mode → lane mapping exactly: `original` → `Original`; `stems` both
      audible → `Instrumental` + `Vocal`; `stems` VOC muted → `Instrumental`;
      `stems` INST muted → `Vocal`.
- [ ] A `"original"` → `"stems"` (both audible) transition closes one `Original`
      clip and opens two contiguous clips (`Instrumental` + `Vocal`); the reverse
      closes two and opens one.
- [ ] A lane present in both the before-set and after-set (e.g. `Instrumental`
      when only "VOC" toggles) has its clip closed and a fresh contiguous clip
      re-opened on that same lane (per §1.5.1).
- [ ] Muting **both** stems while in `"stems"` mode closes all of the deck's open
      stem-lane clips and opens **no** new clip (a silence gap), per §1.5.6; the
      next audible state re-opens clips via the normal lane mapping.
- [ ] A source-mode change on a deck that has no open clip (stopped or silent)
      mutates no existing clips and only affects which lane(s) the next play event
      opens on.
- [ ] The newly opened clip(s) start at the deck's **instantaneous** source
      position at the switch instant (§1.5.4); they are not back-dated to the prior
      clip's source start nor advanced past the switch point.
- [ ] The alignment resolver (PRD-0074) is re-run for each newly opened clip at
      its open instant (§1.5.7), so a contiguous re-open is independently grid- or
      first-beat-anchored rather than inheriting the closed clip's anchoring.
- [ ] No audio-thread allocation, lock, or I/O is introduced: the audio thread (if
      it is the origin of a quantized change) only pushes a POD source-mode-change
      record into the pre-allocated FIFO; all clip close/open work is on the
      message thread mutating the `daw` ValueTree.
- [ ] The `sourceMode` ValueTree property remains the single source of truth; the
      capture reaction reads source state via the property / `SourceModeReader`
      (PRD-0062) and never becomes a second writer of source mode.
- [ ] A test under `Tests/` (e.g. `SourceModeCaptureTests.cpp`) verifies: a
      single-lane swap (`Original` ↔ single stem), a one-to-two multi-lane open
      (`original` → both stems), a two-to-one multi-lane close, a shared-lane
      re-open (VOC un-mute), the both-stems-muted silence-gap case, contiguity of
      `timelineEndSample`/`timelineStartSample` across each boundary, and that a
      change on a stopped deck mutates no clips.

## 1.5. Grey Areas

### 1.5.1. Shared-Lane Transitions: Re-Open vs Leave-Open

When a source-mode change leaves a lane in both the before-set and after-set —
e.g. un-muting "VOC" takes `Instrumental` → `Instrumental` + `Vocal`, and
`Instrumental` is common to both — the engine could either leave the existing
`Instrumental` clip growing untouched (only opening a new `Vocal` clip) or close
the `Instrumental` clip and re-open a fresh contiguous one alongside the new
`Vocal` clip.

**Resolution:** Close and re-open every lane involved in the transition,
including shared lanes. The whole point of source-mode capture is that a
performance gesture produces a clean, visible structural boundary across the
deck's channel group at one timeline instant. Leaving `Instrumental` growing while
`Vocal` starts fresh would put a clip boundary on one lane and not the other at
the same musical moment, making the arrangement read as if the two stems came from
different events. A uniform "close all outgoing-or-changed lanes, open all incoming
lanes at the switch point" rule yields edge-aligned boundaries across the group,
is trivially correct for the multi-lane cases, and costs only a zero-length-free
clip split. The minor cost — an extra clip boundary on a lane whose audio did not
actually change — is acceptable and is exactly the kind of split EPIC-0010's future
merge/heal tooling can collapse if a DJ wants. Uniformity beats a special "shared
lane stays open" case that would complicate the contiguity proof.

### 1.5.2. Mute Toggle as Source-Mode Change vs a Separate Event

A "VOC" / "INST" mute toggle is, in PRD-0062, a different property from
`sourceMode`, so it could be modelled as its own structural event type rather than
folded into the source-mode-change event. Treating it separately would mirror the
ValueTree's two-property structure.

**Resolution:** Treat a mute toggle (while in `"stems"` mode) as the **same**
source-mode-change event for capture purposes. What the capture engine cares about
is not *which property* changed but *which lane set* the deck now writes to, and
both `sourceMode` and the two mute booleans feed the identical PRD-0062 lane
mapping. Folding them into one event type means the clip-placement engine has a
single, uniform "lanes changed: close outgoing, open incoming" handler instead of
two near-identical ones. The event payload is the new published lane set (derived
from `sourceMode` + the two mutes via `SourceModeReader`), not the raw property
delta, so the handler never needs to know the cause. A mute toggle while in
`"original"` mode is, per PRD-0062 §1.5.4, inert (the toggles are greyed) and
produces no lane change and therefore no event.

### 1.5.3. Multi-Lane Open: One Close + Two Opens Atomicity

Switching `"original"` → both stems must close one clip and open two. If the close
and the two opens were applied as three independently observable ValueTree
mutations, an observer (the live projection, a future undo system) could briefly
see an inconsistent group — e.g. the `Original` clip closed but neither stem clip
opened yet.

**Resolution:** Apply the entire transition as **one atomic message-thread
mutation batch** against the `daw` ValueTree — all closes and all opens for the
event committed together before any listener is notified, reusing the same
batched-edit discipline PRD-0073 already uses for a single open/close. There is
exactly one source position and one playhead value for the whole event, so all
finalised end-samples and all new start-samples share those two numbers; computing
them once and committing the close(s)+open(s) as a unit guarantees the group is
never observed mid-transition and that contiguity holds by construction. This also
keeps the "1 close + 2 opens" and "2 closes + 1 open" cases symmetric: both are
just "close the before-set's clips, open the after-set's clips" applied atomically.

### 1.5.4. Source Position the New Clip Starts At

The new clip(s) need a `sourceStartSample`. Candidates: the deck's instantaneous
source position at the switch instant, the closed clip's `sourceEndSample`, or a
grid-quantized source position.

**Resolution:** Use the deck's **instantaneous source position at the switch
instant** — the identical value used for the closing clip's `sourceEndSample`.
Because Original and the summed stems are time-aligned in PRD-0062 (both indexed by
the same deck `playheadPosition` source sample), the source position is a single
well-defined number across all lanes at the switch instant, and using it for both
the close and the open makes the source crop continuous across the boundary even
though the *lane* changed: the arrangement plays the same musical moment, just from
a different stem decomposition. This mirrors EPIC-0009 §1.3.4's general
open/grow/close contract (`sourceStartSample = deck source position`) and the
hot-cue/beat-jump capture rule, keeping source-mode capture consistent with the
rest of the engine rather than inventing a bespoke start-position policy.

### 1.5.5. Rapid Toggling and Tiny Clips

A DJ stab-toggling the source-mode or a mute button quickly (a stutter-style
gesture) could produce a burst of source-mode-change events milliseconds apart,
each closing and opening clips and yielding a run of near-zero-length crops.

**Resolution:** Capture every change faithfully and do **not** coalesce or impose a
minimum clip length at capture time. The recorder's contract (EPIC-0009 §1.3.1) is
lossless structural fidelity: if the DJ produced a rapid stutter of source changes,
the arrangement should reflect exactly that, including very short clips. Coalescing
or enforcing a min-length here would silently discard a deliberate performance
gesture and would require an arbitrary threshold that is wrong for some musical
contexts. Tiny clips are valid `DawClip`s with `sourceStartSample <
sourceEndSample` (or, at the limit, equal — a zero-length clip, which the engine
drops as it does for any other zero-length close per PRD-0073). Any *optional*
post-hoc smoothing/merging of dense clip runs is an editing concern for EPIC-0010,
not a capture-time decision; capture stays faithful and the bridge's FIFO ordering
guarantees the events are applied in the order they occurred.

### 1.5.6. Both Stems Muted: Silence Gap vs Fall Back to Original

In `"stems"` mode the DJ can mute both "VOC" and "INST", producing silence. The
capture engine could either treat this as "no lane" (close all stem clips, open
nothing — a genuine gap on the timeline) or fall back to writing the `Original`
lane (treating silence as if Original were playing).

**Resolution:** Treat both-stems-muted as **no lane — a silence gap**. Per
PRD-0062's mapping, `sourceMode` is still `"stems"`; the deck is audibly silent,
not playing the original file. Writing an `Original` clip would fabricate audio the
DJ did not produce and would misrepresent the arrangement (playback/export would
emit the original where the DJ intended silence). Closing the stem clips and
opening nothing faithfully records "this deck went silent here", consistent with
EPIC-0009 §1.2.1's rule that recording writes clips only when a deck *produces
audio*. When the DJ un-mutes either stem (or switches to "ORIG"), the next
source-mode-change event re-opens clips on the now-audible lane(s) via the normal
mapping, starting at that instant's source position — leaving a true gap on the
lanes for the muted interval, which is the honest representation.

### 1.5.7. Re-Running the Alignment Resolver at Each New Open

A contiguous re-open creates a new clip mid-stream. The alignment resolver
(PRD-0074) decides grid-alignment vs first-beat-anchoring at open time. The
question is whether each re-opened clip is independently resolved or inherits the
anchoring of the clip it succeeds.

**Resolution:** Re-run the alignment resolver **independently for each newly
opened clip** at its open instant, exactly as PRD-0073 does for any other open
event. A source-mode change does not alter the deck's BPM, phase alignment, or
beatgrid, so in the common case the resolver returns the same decision as the
closed clip and the boundary is seamless — but resolving independently is correct
because the deck *could* have drifted, been nudged, or had sync toggled between the
original open and the switch, and the new clip must reflect the deck's state *now*.
Inheriting the prior clip's anchoring would be a hidden coupling that breaks the
moment those conditions change. Re-resolving per open keeps source-mode capture a
thin orchestration over PRD-0073/PRD-0074's existing open path and introduces no
alignment logic of its own.
