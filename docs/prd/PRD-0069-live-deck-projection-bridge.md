---
status: Implemented
epic: EPIC-0008
depends-on:
  - PRD-0062
  - PRD-0063
  - PRD-0067
  - PRD-0068
---

# 1. PRD-0069: Live Deck Projection Bridge

## 1.1. Problem

By the time this PRD is reached, every static piece of the DAW canvas exists but
nothing on the timeline moves. PRD-0063 defines the `daw` ValueTree branch
(`daw.tracks[]` → `daw.tracks[i].lanes[]` → `daw.tracks[i].lanes[j].clips[]`)
and the non-destructive `DawClip` value object. PRD-0062 publishes each deck's
active source mode (original vs stems) as a `sourceMode` ValueTree property plus
a `SourceModeReader` consumer. PRD-0064 reconciles the DAW's musical grid with
`MasterClockManager`. PRD-0067 builds the `LaneView` / `ChannelGroupHeader`
molecules, and PRD-0068 builds the `ClipBlock` atom that renders a clip's source
waveform crop. All of these are inert: the lanes are drawn empty, the grid
ticks, but no clip ever appears because nothing is wired to the live decks.

The deck side already publishes everything needed, but only as raw audio-thread
atomics. The authoritative cross-thread source sample position is
`DeckAudioState::playheadPosition` (`std::atomic<int64_t>`), reachable on the
message thread via `DeckStateManager::getAudioState(deckId) -> DeckAudioState*`.
The deck's transport state is `DeckAudioState::playbackStatus`
(`std::atomic<int>`, enum `PlaybackStatusCode { empty=0, stopped=1, playing=2,
paused=3 }`). The audio-thread stem flag is `DeckAudioSource::stemsActive`, but
the message-thread-safe view of the active source mode is PRD-0062's
`SourceModeReader`. Per-stem mute is `DeckAudioState::stemVocalsMuted /
stemDrumsMuted / stemBassMuted / stemOtherMuted`.

What is missing is the bridge: a message-thread component that polls these deck
atomics at UI refresh rate and grows a "live clip" on the matching lane(s) of
the deck group's `daw` sub-tree, sample-for-sample, so the DJ sees the
arrangement build itself as the deck plays. Per `AGENTS.md` this bridge must
never touch the audio thread, never allocate / lock / do I/O on the audio
thread, and mutate the model exclusively on the message thread. It is read-only
with respect to the decks (it only loads atomics) and write-only with respect to
the `daw` model. Without it, EPIC-0008's defining feature — the always-on live
deck projection — does not exist.

## 1.2. Objective

The system renders an always-on, real-time live projection of each playing deck
onto its timeline lane(s) such that:

- A message-thread `LiveProjectionTimer` (`Source/Features/Daw/Projection/
  LiveProjectionTimer.h/.cpp`) runs at UI refresh rate (~30–60 Hz, see §1.5.1)
  and, on each tick, reads — never writes — every active deck's
  `DeckAudioState::playheadPosition`, `DeckAudioState::playbackStatus`, the
  active source mode (via PRD-0062's `SourceModeReader`, consumed through a
  `Source/Features/Daw/Projection/SourceModeReader.h` include), and the four
  per-stem mute flags.
- While a deck's `playbackStatus == playing`, a single "live clip" per deck
  group grows on the lane(s) matching the active source mode: original mode →
  the `Original` lane only; stems mode with both stems audible → both the
  `Instrumental` and `Vocal` lanes; stems mode with one stem muted → only the
  audible stem's lane. The clip's `sourceEndSample` advances sample-for-sample
  to track the deck's source position.
- The live clip is anchored so its first sample (`timelineStartSample`) sits at
  the live playhead / now-line timeline position at the moment playback began,
  and its `sourceStartSample` equals the deck's source position at that moment;
  placement is grid-relative, sharing the master grid phase from PRD-0064. This
  Epic keeps the anchor at the raw live position (no first-beat re-alignment —
  see §1.5.6).
- A play→stop or play→pause transition FINALISES the live clip by freezing its
  `sourceEndSample` at the last observed source position (finalisation policy
  for paused vs stopped, see §1.5.7).
- A source-position discontinuity while playing (cue / seek — the observed
  `playheadPosition` jumps non-monotonically or beyond a tolerance) finalises
  the current live clip and starts a new one anchored at the new position. This
  is the minimal correct behaviour for EPIC-0008; full hot-cue / beat-jump /
  loop capture is EPIC-0009 and is explicitly NOT built here (see §1.5.3).
- All `daw` model mutation happens on the message thread only; the bridge reads
  deck atomics with `std::memory_order_acquire` loads and introduces no
  audio-thread code, no allocation, no locks, and no I/O on the audio thread.
- Rendering is not the bridge's concern: it only mutates the `daw` model, and
  the existing PRD-0068 `ClipBlock` / PRD-0067 `LaneView` observe the tree via
  JUCE Listeners and redraw. The live clip is an ordinary `DawClip` node so no
  new rendering path is introduced.

## 1.3. Developer / Integration Flow

1. A new `SourceModeReader.h` is added under `Source/Features/Daw/Projection/`
   that wraps (or re-exports) PRD-0062's `SourceModeReader` contract, exposing a
   message-thread query `getActiveSourceMode(deckId)` returning an enum
   (`Original`, `Stems`) plus the per-stem audibility derived from
   `DeckAudioState::stemVocalsMuted / stemDrumsMuted / stemBassMuted /
   stemOtherMuted`. The Daw slice includes only this public contract; it does
   not reach into PRD-0062's internals.

2. `LiveProjectionTimer` is implemented as a `juce::Timer` subclass owned by the
   Daw feature (constructed with references to `DeckStateManager`, the
   `SourceModeReader`, the master-grid service from PRD-0064, and the `daw`
   `ValueTree`). It is started at the configured refresh rate (§1.5.1) when the
   DAW panel is constructed and stopped on teardown (RAII; no global state).

3. The timer keeps a small per-deck-group projection-state struct on the message
   thread (not in the ValueTree): `{ liveClipNode, lastSourcePosition,
   wasPlaying, lastSourceMode, lastStemAudibility }`. This is plain
   message-thread state with no cross-thread sharing, so it needs no atomics or
   locks.

4. On each `timerCallback()`, for every active deck group:
   - Load `playbackStatus` (acquire) and `playheadPosition` (acquire) from the
     deck's `DeckAudioState`.
   - Resolve the active source mode and per-stem audibility via the
     `SourceModeReader` + mute flags.
   - Determine the target lane set: `Original`; or `{Instrumental, Vocal}`; or a
     single stem lane; or empty (both stems muted → no lane drawn, see §1.5.5).

5. State-machine transitions per group:
   - `stopped/paused → playing`: create a new live clip on each target lane via
     a model helper (PRD-0063), with `sourceStartSample = playheadPosition`,
     `sourceEndSample = playheadPosition`, and
     `timelineStartSample = master-grid mapping of the current now-line` (raw,
     per §1.5.6). Record the node(s) in projection state.
   - `playing → playing` (steady state): set each live clip's `sourceEndSample`
     to the current `playheadPosition` (clamped to be ≥ `sourceStartSample`),
     growing the clip. The PRD-0068 `ClipBlock` observes the property change and
     extends its drawn width.
   - `playing → stopped` or `playing → paused`: FINALISE — leave the live
     clip's `sourceEndSample` at its last value and clear the projection-state
     `liveClipNode` so the next play starts a fresh clip (paused/stopped policy,
     §1.5.7).
   - Source-mode change mid-play (e.g. original → stems): finalise the clip(s)
     on the now-inactive lane(s) and start fresh clip(s) on the newly-active
     lane(s) at the current position (§1.5.4).
   - Discontinuity while playing (`|playheadPosition − lastSourcePosition|`
     exceeds a forward-tolerance, or the position moves backwards): finalise the
     current clip and start a new one anchored at the new position (§1.5.3).

6. All clip creation / extension / finalisation goes through PRD-0063 model
   helpers that mutate the `daw` ValueTree on the message thread. No direct
   audio-engine writes occur. The bridge never calls into `DeckAudioSource` or
   any audio-thread path; it only reads the published atomics.

7. A new test file `Tests/LiveProjectionBridgeTests.cpp` drives the bridge with a
   synthetic `DeckAudioState` (atomics set directly from the test thread) and a
   stub `SourceModeReader`, ticks the timer logic manually (the tick body is
   factored into a testable `processTick()` that takes no real `Timer`
   dependency), and asserts the resulting `daw` ValueTree mutations: clip
   creation on the correct lane(s), monotonic `sourceEndSample` growth,
   finalisation on stop / pause, lane switching on source-mode change, and a new
   clip on a simulated seek discontinuity.

## 1.4. Acceptance Criteria

- [ ] A `LiveProjectionTimer` class exists at `Source/Features/Daw/Projection/
      LiveProjectionTimer.h/.cpp`, is a `juce::Timer` subclass, and is started
      at a configurable UI refresh rate (default within 30–60 Hz, see §1.5.1)
      when the DAW panel is alive and stopped on teardown.
- [ ] A `Source/Features/Daw/Projection/SourceModeReader.h` consumes PRD-0062's
      published source mode and exposes a message-thread query for the active
      source mode and per-stem audibility per deck; it does not reach into
      PRD-0062's internals.
- [ ] On each tick the bridge reads `DeckAudioState::playheadPosition`,
      `DeckAudioState::playbackStatus`, the active source mode, and the four
      `DeckAudioState::stem{Vocals,Drums,Bass,Other}Muted` flags using
      acquire-ordered atomic loads, and performs no writes to any deck state.
- [ ] While a deck's `playbackStatus == playing` (value `2`), exactly one live
      clip per active lane grows for that deck group, its `sourceEndSample`
      advancing monotonically to track `playheadPosition`, and its
      `sourceStartSample` and `timelineStartSample` fixed at the values captured
      when playback began.
- [ ] Original source mode draws a live clip on the `Original` lane only.
- [ ] Stems source mode with both stems audible draws live clips on both the
      `Instrumental` and `Vocal` lanes simultaneously, growing in lockstep.
- [ ] Stems source mode with exactly one stem muted draws a live clip only on
      the audible stem's lane; the muted stem's lane shows no live clip.
- [ ] Stems source mode with both stems muted draws no live clip on any lane
      (see §1.5.5); when a stem becomes audible again a fresh clip starts.
- [ ] A `playing → stopped` transition finalises the live clip(s) (freezes
      `sourceEndSample`) and the next `→ playing` starts a new clip rather than
      extending the finalised one.
- [ ] A `playing → paused` transition finalises the live clip(s) per the policy
      in §1.5.7; resuming from pause starts a new live clip.
- [ ] A mid-play source-mode change finalises the clip(s) on the now-inactive
      lane(s) and starts fresh clip(s) on the newly-active lane(s) at the
      current source position (see §1.5.4).
- [ ] A source-position discontinuity while playing (a non-monotonic jump, or a
      forward jump beyond the configured tolerance, i.e. a cue / seek) finalises
      the current live clip and starts a new one anchored at the new position;
      no attempt is made to model the jump as a split / repeat (deferred to
      EPIC-0009, see §1.5.3).
- [ ] The live clip's `timelineStartSample` is computed from the master-grid
      service (PRD-0064) so the projection shares the master grid phase; this
      Epic anchors at the raw live position with no first-beat re-alignment (see
      §1.5.6).
- [ ] The live clip is an ordinary PRD-0063 `DawClip` node and is rendered by
      the existing PRD-0068 `ClipBlock` via PRD-0067 `LaneView` JUCE Listeners;
      no new rendering path is added by this PRD.
- [ ] All `daw` ValueTree mutation occurs on the message thread; no audio-thread
      code path is added or modified by this PRD, and no allocation, lock, or
      I/O is introduced on the audio thread (per `AGENTS.md`).
- [ ] No persistence or recording of the projection is introduced; the live clip
      is an ephemeral "live view" that is reset / overwritten each playthrough
      (persistence and event capture are EPIC-0009, see §1.5.2).
- [ ] `Tests/LiveProjectionBridgeTests.cpp` exercises a synthetic
      `DeckAudioState` + stub `SourceModeReader` and asserts: clip creation on
      the correct lane(s), monotonic `sourceEndSample` growth, finalisation on
      stop and on pause, lane switching on source-mode change, no clip when both
      stems muted, and a new clip on a simulated seek discontinuity.

## 1.5. Grey Areas

### 1.5.1. Refresh Rate: 30 Hz vs 60 Hz

The timer cadence trades visual smoothness against CPU cost. At 30 Hz the live
clip's leading edge advances in ~33 ms steps; at 60 Hz, ~16 ms steps. Because
the clip is a single growing rectangle (the leading edge moves, the body is
static), the perceptual difference between 30 and 60 Hz is small, and every tick
does a handful of atomic loads plus, in steady state, a single ValueTree
property write per active lane.

**Resolution:** Default to a configurable rate, set initially to match the
display via `juce::Timer` at ~60 Hz on capable hardware but no requirement to
exceed the host refresh rate, with a hard floor of 30 Hz. The rate is a single
constant in `LiveProjectionTimer` so it can be tuned without touching call
sites. 60 Hz is chosen as the default because the cost is dominated by the
ValueTree write count (bounded by active lanes ≤ 4 decks × ≤ 2 lanes), which is
trivial, and the smoother leading edge reads as more "live". If profiling on
low-end hardware shows the listener-driven repaint chain is the bottleneck, the
constant drops to 30 Hz with no behavioural change. The bridge's correctness
does not depend on the rate — `sourceEndSample` is always set to the actual
observed `playheadPosition`, never accumulated — so a slower tick simply means
coarser visual steps, never drift.

### 1.5.2. One Ephemeral Live Clip per Group vs Persisted History

When a deck stops and restarts, the bridge can either (a) keep the finalised
clip on the lane and add a second clip for the new playthrough, accumulating a
history of every play span, or (b) discard / overwrite so only the current (or
most recent) live span is shown.

**Resolution:** This Epic is an ephemeral "live view", not a recorder. The live
projection is a read-only visualisation of what the deck is doing right now;
accumulating a persistent arrangement of every play span is exactly the event
capture owned by EPIC-0009. For EPIC-0008 the bridge maintains at most one live
clip per lane and, on each new play span, starts fresh — a previously finalised
live clip from an earlier span may remain visible until overwritten by a new
span on the same lane, but nothing is saved and nothing is guaranteed to persist
across a stop. When EPIC-0009's Record is engaged, it will own clip lifetime and
persistence; until then, the live view is deliberately disposable. This keeps the
model small and avoids prematurely committing to a capture policy that EPIC-0009
must own.

### 1.5.3. Cue / Seek Discontinuity Handling

A DJ cueing, needle-dropping, or beat-jumping mid-play makes `playheadPosition`
jump. Faithfully modelling that as a clip split, a repeat, or a back-reference is
the heart of EPIC-0009's event capture and is genuinely complex (a hot-cue jump
backwards should arguably draw a second clip referencing earlier source samples;
a loop should draw a repeated region).

**Resolution:** Do the simplest correct thing and no more. When the observed
`playheadPosition` moves backwards, or forwards by more than a small tolerance
(a few tens of milliseconds at the project sample rate — larger than one tick's
worth of normal advance), the bridge finalises the current live clip and starts a
new live clip anchored at the new position. This produces a truthful "the deck is
now playing from here" view without attempting to model the semantics of the
jump. Full hot-cue / beat-jump / loop capture — including back-referencing
earlier source ranges and drawing repeats — is explicitly EPIC-0009 and is NOT
built in this PRD. Keeping it minimal here avoids baking in a capture model that
the recording Epic must own and possibly redo.

### 1.5.4. Source-Mode Change Mid-Play (Switch Lane)

If the DJ flips a deck from original to stems (or vice-versa, or mutes/unmutes a
stem) while it is playing, the set of audible lanes changes mid-clip.

**Resolution:** Treat a source-mode (or stem-audibility) change as a lane
transition: finalise the live clip(s) on every lane that is no longer audible and
start fresh live clip(s) on every lane that is newly audible, both at the current
source position. The result is that each lane shows a contiguous clip for exactly
the span during which that lane was audible — which is the honest live view. The
bridge stores `lastSourceMode` and `lastStemAudibility` in its message-thread
projection state to detect the change. No attempt is made to "continue" a clip
across a mode change, because the lanes are different sources; a continuation
would misrepresent which source produced which span.

### 1.5.5. Both Stems Muted: No Lane Drawn

In stems mode the DJ can mute both audible stems, so the deck is playing but
producing no audio on any projected lane.

**Resolution:** Draw nothing. If the active source mode is stems and both the
instrumental and vocal sides are muted, the bridge creates no live clip and
finalises any existing live clips on those lanes (the deck is audibly silent on
those lanes). When a stem becomes audible again, the normal `→ audible`
transition starts a fresh clip at the then-current position. This matches the
"projection reflects what is audible" principle: a muted lane is silent, so its
lane is empty. It also avoids drawing a misleading clip for audio the DJ cannot
hear.

### 1.5.6. Grid Alignment When Deck BPM ≠ Master: Raw vs First-Beat Anchor

The live clip's `timelineStartSample` could be (a) the raw now-line position at
the instant playback began, or (b) snapped to the nearest master-grid beat / bar
so the clip's start aligns musically. When the deck's BPM differs from the master
BPM, a raw anchor means the clip's internal beats will not line up with the grid
lines under it.

**Resolution:** Anchor at the raw live position for this Epic. The live
projection is a faithful "this is where the deck is, right now" view, and
snapping the start to a grid line would misrepresent the actual timeline moment
playback began. Crucially, full musical alignment — re-anchoring clip starts to
beats and time-stretching clips so their internal grid matches the master grid —
is the alignment work owned by EPIC-0009 (the clip model is non-stretched 1:1 in
EPIC-0008 by §1.3.1 of the Epic). Doing raw placement now keeps this PRD honest
about scope and avoids a half-built alignment that EPIC-0009 must redo. The clip
still shares the master grid phase (the grid lines drawn under it come from
PRD-0064), so the visual reference is correct even though the clip's first sample
is not beat-snapped.

### 1.5.7. Paused vs Stopped Finalisation

A deck can leave the `playing` state into either `paused` (3) or `stopped` (1),
and the bridge must decide whether the two are treated identically.

**Resolution:** Both `paused` and `stopped` finalise the live clip — its
`sourceEndSample` freezes at the last observed source position and the
projection-state `liveClipNode` is cleared. The two are treated identically for
finalisation because, in both cases, the deck has stopped advancing its source
position and the live span has ended. Resuming from pause therefore starts a new
live clip at the resume position rather than re-extending the paused clip. This
is the simplest correct behaviour and is consistent with §1.5.3: any later resume
is just another play span. A future Epic could choose to "stitch" a paused clip
back together on resume if the resume position is identical to the pause
position, but that is a capture-semantics refinement owned by EPIC-0009, not a
live-view concern; treating pause as a finalisation keeps this Epic's model
trivially correct and avoids guessing the DJ's intent.
