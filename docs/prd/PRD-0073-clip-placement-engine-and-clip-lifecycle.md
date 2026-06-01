---
status: Implemented
epic: EPIC-0009
depends-on:
  - PRD-0063
  - PRD-0071
  - PRD-0072
---

# 1. PRD-0073: Clip Placement Engine & Clip Lifecycle

## 1.1. Problem

By the time this PRD is reached, the recording stack has its clock and its event
pipe but no consumer that turns events into a recorded arrangement. PRD-0071
delivers the recording session controller and the master-clock-anchored record
playhead; PRD-0072 delivers the performance-event bridge — a lock-free FIFO into
which deck/mixer structural events (play, stop, unmute, mute, jump-in, jump-out,
loop-boundary, source-mode-change) are pushed from the audio thread and drained
on the message thread. PRD-0063 defines the persisted `DawClip` crop model
(`laneId`, `sourceFileId`, `sourceStartSample`, `sourceEndSample`,
`timelineStartSample`) inside the `daw` `ValueTree`. What does not yet exist is
the engine that sits between the drained event stream and the persisted clip
model: the component that decides *when a clip opens, how it grows, and when it
closes*, and writes the result into the `daw` tree.

EPIC-0008 already projects the live deck source position into an ephemeral
on-screen "live-view" clip via a projection timer, but that projection is
**discarded** every frame — it exists only to draw the moving edge of what the
deck is currently playing. The recorder needs the same cadence of source-position
sampling, except the result must be **persisted** into a real `DawClip` whose
`sourceEndSample` advances over time. Without a dedicated placement engine,
events drained from PRD-0072 have nowhere to go, the alignment rule (PRD-0074)
has no clip to align, and the specific event handlers (PRD-0075/0076/0077) have
no lifecycle to hook into.

The core difficulty is the lifecycle itself: a single contiguous stretch of deck
playback is one clip, but a hot-cue jump, a loop wrap, or a source-mode change
*mid-playback* must split that stretch into multiple back-to-back clips with no
gap and no overlap on the timeline — the set keeps moving forward in time even
though the source position jumped. This PRD owns that lifecycle and the
persistence; it deliberately does **not** own the alignment math (PRD-0074) or
the semantics of any specific jump/loop/source-mode event (PRD-0075/0076/0077),
which layer on top of the open/close primitives defined here.

## 1.2. Objective

The system provides a `ClipPlacementEngine` that consumes drained performance
events on the message thread and converts them into persisted `DawClip` entries
in the `daw` `ValueTree`, implementing the open → grow → close lifecycle:

- The engine runs entirely on the message thread. It is driven by (a) the
  PRD-0072 drain callback delivering ordered POD events and (b) a periodic
  grow tick (see §1.3) that advances open clips. It reads — but never blocks —
  the audio thread.
- **Open**: a play / unmute / jump-in / source-mode event for a deck with no
  currently-open clip (on the resolved lane) creates a new open clip with
  `sourceStartSample = deck source position at the event` and
  `timelineStartSample = record playhead at the event` (PRD-0071). The lane is
  resolved from the deck's active source mode (Original vs stems). The alignment
  rule (PRD-0074) is invoked at this open instant to fix `timelineStartSample`;
  this PRD calls the resolver as a seam and uses the unsnapped playhead as the
  fallback when PRD-0074 is not yet present.
- **Grow**: while a clip is open, its `sourceEndSample` is advanced to track the
  deck's live source position at the engine's grow cadence, persisting into the
  `daw` tree what EPIC-0008's live projection discards.
- **Close**: a stop / mute / jump-out / source-mode-change / loop-boundary event
  finalises the open clip's `sourceEndSample` (to the deck source position at the
  event) and its `timelineEndSample` (to the playhead at the event). A jump or
  source-mode change closes the current clip and **immediately opens the next
  clip contiguously** — the new clip's `timelineStartSample` equals the closed
  clip's `timelineEndSample`, guaranteeing no gap and no overlap.
- A captured clip is fully described by
  `(laneId, sourceFileId, sourceStartSample, sourceEndSample, timelineStartSample)`
  (EPIC-0009 §1.3.1). The engine never records audio samples.
- **Multi-deck**: the engine tracks one open-clip slot per (deck, lane) pair, so
  all active decks 1–4 capture concurrently, each writing into its own channel
  group, without cross-talk.
- On record-stop, every still-open clip is finalised at the stop playhead before
  the engine quiesces, so no clip is left dangling in the `daw` tree.

## 1.3. Developer / Integration Flow

1. A new component is added at
   `Source/Features/Daw/Recording/ClipPlacementEngine.h/.cpp`. It is constructed
   with explicit dependencies (no singletons): a reference to the `daw`
   `ValueTree` (PRD-0063 model), the record playhead/session controller
   (PRD-0071), a deck-source-position provider (a small interface returning the
   current source sample and `sourceFileId` for a given deck id), a source-mode /
   lane resolver (returns the lane id(s) for a deck's active source mode), and
   an alignment-resolver seam (PRD-0074; defaulted to identity-on-playhead until
   that PRD lands).
2. PRD-0072's drain step invokes a single `onEvent(const PerformanceEvent&)`
   entry point on the engine for each drained POD event, in FIFO order, on the
   message thread. The engine switches on the event type and applies the
   lifecycle: open-class events (`Play`, `Unmute`, `JumpIn`, `SourceModeEnter`)
   open a clip; close-class events (`Stop`, `Mute`, `JumpOut`, `LoopBoundary`,
   `SourceModeExit`) close the open clip; paired split events (`JumpOut`+`JumpIn`,
   `SourceModeExit`+`SourceModeEnter`) close-then-open contiguously.
3. Growth is driven by a message-thread `GrowTick` at the same cadence as
   EPIC-0008's `LiveProjectionTimer`. This PRD exposes a `grow()` method that the
   recording session controller calls from its existing projection timer; the
   engine iterates every open-clip slot and writes the current deck source
   position into that clip's `sourceEndSample` property in the `daw` tree. No new
   timer is introduced (see §1.5.1).
4. Each open-clip slot is held in a small message-thread map keyed by
   `(deckId, laneId)`. A slot stores the live `juce::ValueTree` node of the open
   clip plus the cached `sourceStartSample`/`timelineStartSample`. Closing a slot
   writes the final `sourceEndSample`/`timelineEndSample` and removes the slot
   from the map; the clip node remains in the `daw` tree as a finalised clip.
5. Clip nodes are created as children of the per-deck channel-group node defined
   by PRD-0063. The engine writes only the five descriptor properties plus the
   finalisation pair (`sourceEndSample`, `timelineEndSample`); it sets no
   playback, gain, or automation properties (those belong to later Epics).
6. The alignment seam: at open, the engine computes
   `timelineStartSample = alignmentResolver.resolveOpen(deckId, rawPlayhead,
   rawSourceStart)`. PRD-0074 supplies the real implementation; this PRD ships a
   pass-through resolver that returns `rawPlayhead` so the engine is testable and
   correct (contiguous, persisted) before alignment exists.
7. On record-stop, the session controller (PRD-0071) calls
   `finaliseAll(stopPlayhead)`; the engine closes every open slot at the stop
   playhead, clears the slot map, and is ready for the next recording.
8. A new test file `Tests/ClipPlacementEngineTests.cpp` drives the engine with
   synthetic `PerformanceEvent` sequences and a stub deck-source provider,
   asserting the resulting `daw` `ValueTree` clip nodes have the expected
   descriptor tuples, contiguity, and finalisation behaviour.

## 1.4. Acceptance Criteria

- [ ] A `ClipPlacementEngine` lives at `Source/Features/Daw/Recording/ClipPlacementEngine.h/.cpp`, constructed via explicit dependency injection (no singletons, RAII), and operates exclusively on the message thread.
- [ ] A `Play` / `Unmute` / `JumpIn` / `SourceModeEnter` event for a deck with no open clip on the resolved lane opens a new `DawClip` node in the `daw` `ValueTree` with `sourceStartSample` = the deck's source position at the event, `timelineStartSample` = the record playhead at the event (via the alignment seam), `sourceFileId` = the deck's current source file, and `laneId` = the lane resolved from the deck's active source mode.
- [ ] While a clip is open, calling `grow()` advances that clip's `sourceEndSample` property in the `daw` tree to the deck's current source position; repeated `grow()` calls monotonically advance `sourceEndSample` (never moving it backwards) for a normally-playing deck.
- [ ] A `Stop` / `Mute` / `JumpOut` / `LoopBoundary` / `SourceModeExit` event finalises the open clip: `sourceEndSample` = deck source position at the event, `timelineEndSample` = record playhead at the event, and the slot is removed so the clip is no longer grown.
- [ ] A jump (`JumpOut` immediately followed by `JumpIn`) or a source-mode change (`SourceModeExit` + `SourceModeEnter`) closes the current clip and opens the next clip such that the new clip's `timelineStartSample` equals the closed clip's `timelineEndSample` exactly — no gap and no overlap on the timeline.
- [ ] Each captured clip is fully described by `(laneId, sourceFileId, sourceStartSample, sourceEndSample, timelineStartSample)`; the engine writes no audio data and reads no audio samples at any point.
- [ ] The engine tracks open clips per `(deckId, laneId)` concurrently: with two decks playing simultaneously, both have independent open clips growing in parallel into their respective channel groups, with no cross-deck interference.
- [ ] `finaliseAll(stopPlayhead)` (invoked by PRD-0071 on record-stop) closes every still-open clip at the stop playhead, leaving zero open slots and zero un-finalised clips in the `daw` tree.
- [ ] The alignment rule is invoked at open via a resolver seam; in the absence of PRD-0074 the engine uses a pass-through resolver returning the raw playhead, and the engine's contiguity and persistence guarantees hold regardless of which resolver is installed.
- [ ] Clip nodes are created as children of the correct per-deck channel-group node defined by PRD-0063; multi-deck capture writes each deck's clips into its own group.
- [ ] No audio-thread code is added by this PRD. The engine only reads positions provided to it on the message thread and mutates the `daw` `ValueTree` on the message thread; it allocates and locks freely on the message thread but never on the audio thread, and adds no new audio-thread path.
- [ ] No UI is added by this PRD (record control and playhead rendering are PRD-0078); the engine has no `Component` and no `DESIGN.md` surface.
- [ ] `Tests/ClipPlacementEngineTests.cpp` covers, at minimum: single open→grow→close producing one correct clip; a mid-playback split producing two contiguous clips; concurrent two-deck capture; `grow()` monotonic advance; and `finaliseAll` closing a still-open clip.

## 1.5. Grey Areas

### 1.5.1. Grow Cadence: Reuse the EPIC-0008 Live Projection Timer vs a Dedicated Recording Timer

EPIC-0008 already runs a `LiveProjectionTimer` on the message thread to advance
the ephemeral on-screen live-view clip edge. The grow step of this engine needs
the same cadence of source-position sampling. The options are (a) reuse the
existing EPIC-0008 timer by having it also call `ClipPlacementEngine::grow()`,
or (b) introduce a dedicated recording-grow timer owned by the recording stack.

**Resolution:** Reuse the existing cadence (option a), but do not couple the
engine to the timer. The engine exposes a plain `grow()` method and holds no
timer of its own; the recording session controller (PRD-0071) — which already
shares the projection tick context — calls `grow()` from the same cadence that
drives EPIC-0008's projection. This avoids a second timer drifting against the
first (which would produce subtly inconsistent `sourceEndSample` values between
the live view and the recording), keeps the engine timer-agnostic and trivially
unit-testable (tests call `grow()` directly), and honours the EPIC's own framing
that growth is "the same projection cadence as EPIC-0008's live projection, now
persisted rather than discarded." The grow rate is a sampling rate for the
*end* of a crop, not an audio rate; the exact crop boundary is finalised
precisely at the close event from the deck's reported position, so any tick
granularity in between is invisible in the final clip.

### 1.5.2. Minimum Clip Length and Discarding Ultra-Short Clips

An open-immediately-followed-by-close (e.g. a play then an instant stop, or a
rapid double hot-cue) can produce a clip whose `sourceEndSample` equals or barely
exceeds `sourceStartSample` — a zero-length or sub-sample clip with no audible
content but a real node in the `daw` tree.

**Resolution:** On close (and within `finaliseAll`), if a clip's resulting source
length is `<= 0` samples, the clip node is removed rather than finalised; if its
length is `> 0` but below a small minimum threshold (default: 1 ms at the project
sample rate), the clip is still kept — short clips are legitimate performance
artefacts (a stutter, a quick cue stab) and discarding them would lose
intentional detail. Only the degenerate zero/negative-length case is discarded,
because it carries no information and would complicate later playback (EPIC-0010)
and export (EPIC-0012). Crucially, when a discarded clip was part of a contiguous
split (close-then-open), the *next* clip still opens at the same timeline point,
so discarding the degenerate clip does not introduce a gap. The 1 ms keep-floor
is a tunable constant, not a hard contract; the only invariant is "drop
zero/negative length, keep everything positive."

### 1.5.3. Contiguity Guarantee at Close-Then-Open

A jump or source-mode change must produce two clips that meet exactly on the
timeline. The risk is that the close event and the subsequent open event each
independently sample the record playhead, and if the playhead advanced between
the two reads (or if they read slightly different clock snapshots), the clips
would gap or overlap.

**Resolution:** Close-then-open is performed as a single atomic operation against
one playhead snapshot. The engine captures the playhead once for the paired
event, finalises the closing clip's `timelineEndSample` to that snapshot, and
opens the next clip's `timelineStartSample` from the **same** snapshot — not from
a fresh read. This makes `newClip.timelineStartSample == oldClip.timelineEndSample`
by construction, not by coincidence. The alignment seam (PRD-0074) is applied to
the *source* anchoring of the new clip but must not re-snap the timeline start of
a contiguous continuation away from the closing clip's end (PRD-0074 owns the
precise rule; this PRD's invariant is that whatever the resolver returns for a
contiguous open, the contiguity of the *timeline* is preserved). Source position
is read independently per clip (the jump *is* a source discontinuity, so the
source positions legitimately differ); only the timeline must be contiguous.

### 1.5.4. Concurrent Multi-Deck Open Clips

Up to four decks can be playing during recording, each potentially with its own
open clip, and (with stems) a single deck can write to multiple lanes. The engine
must keep these independent so that an event for deck A never closes or grows
deck B's clip.

**Resolution:** Open clips are keyed by `(deckId, laneId)`, not by deck alone.
Every event carries its originating deck id (PRD-0072's POD record), and the lane
is resolved from that deck's source mode at the event instant. `grow()` iterates
all slots and advances each from its own deck's source provider. This cleanly
supports both multi-deck capture (distinct `deckId`) and multi-lane stems capture
on one deck (distinct `laneId` under the same `deckId`), and makes per-deck
finalisation a matter of removing only the matching slots. A deck with no open
clip simply has no slot; events for it create one. This is the minimal key that
satisfies both the multi-deck and stems requirements without a special case.

### 1.5.5. Finalisation on Record-Stop with Still-Open Clips

When the DJ presses Stop while decks are still playing, every open clip is
mid-grow. The engine must close them cleanly so the recorded arrangement is
internally consistent and contains no clip without an end.

**Resolution:** PRD-0071's stop transition calls `finaliseAll(stopPlayhead)`
before the recording session is considered closed. Each open clip is finalised
exactly as a `Stop` event would finalise it: `sourceEndSample` = the deck's
current source position, `timelineEndSample` = the stop playhead. The engine does
**not** wait for a real `Stop` event per deck (the deck may keep playing audibly
after the DAW stops recording — recording-stop is a DAW concern, not a deck
concern). After `finaliseAll`, the slot map is empty and the engine is reusable
for the next recording without reconstruction. Zero/negative-length open clips
caught at stop are discarded per §1.5.2.

### 1.5.6. Relationship Between the Ephemeral Live-View Clip (EPIC-0008) and the Persisted Recorded Clip

EPIC-0008 draws an ephemeral live-view clip that shows what each deck is currently
playing; this PRD persists a recorded clip from the same source-position stream.
The two could be conflated (one object serving both purposes) or kept fully
separate.

**Resolution:** They are separate objects with a shared *input*, not a shared
*identity*. EPIC-0008's live-view clip is a transient, redraw-every-frame visual
that exists whenever a deck plays, regardless of recording state, and is never
written to the `daw` model. This PRD's recorded clip is a persisted `DawClip`
node that exists only while recording is armed/running and is the durable record
of the performance. They consume the same deck-source-position projection (hence
the shared grow cadence, §1.5.1), but the live view is discarded each frame while
the recorded clip accumulates. Keeping them separate means recording can start
and stop without disturbing the live view, and the live view keeps working when
recording is off. The only coupling is the cadence seam: the recorded clip grows
"at the same rate" the live view projects, so the two never disagree about where
the deck currently is.

### 1.5.7. Lane Resolution for the Open Clip

The lane a clip is written to depends on the deck's active source mode (Original
vs stems, and which stems are active). PRD-0077 owns source-mode *capture*
semantics, but this PRD must resolve a lane at every open in order to key and
place the clip.

**Resolution:** This PRD resolves the lane through an injected source-mode/lane
resolver interface and treats the result as opaque — it does not interpret what
"Original" or a given stem means. At open, the engine asks the resolver for the
lane id(s) for the deck's current source mode and opens one clip per returned
lane. For Original mode the resolver returns a single deck lane; for stems it
returns the active stem lanes. The detailed semantics of *transitions* between
source modes (which old lanes close, which new lanes open, contiguity across the
switch) are PRD-0077's concern and layer on top of the open/close primitives
defined here. This PRD's contract is narrow: "given a deck and the moment of an
open event, the resolver names the lane(s), and a clip is opened on each" — which
is enough to satisfy multi-lane capture without pre-empting PRD-0077's design.
