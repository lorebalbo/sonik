---
status: Not Implemented
epic: EPIC-0009
depends-on:
  - PRD-0012
  - PRD-0015
  - PRD-0072
  - PRD-0073
---

# 1. PRD-0075: Hot-Cue & Beat-Jump Capture

## 1.1. Problem

The clip-placement engine (PRD-0073) and the performance-event bridge (PRD-0072) together can already turn a single, continuous span of deck playback into a growing clip: a play/unmute event opens a clip, the source position advances, and a stop/mute event closes it. But a DJ rarely plays a track from start to finish during a recorded set. They press hot cues to stutter and re-trigger sections (PRD-0012), and they beat-jump forward and backward to skip into the next phrase or replay a buildup (PRD-0015). Each of these actions makes the deck's *source* position jump discontinuously while the *timeline* keeps advancing in real time.

Without explicit handling, the recorder has no defined behaviour for a source discontinuity. The naive "single growing clip" model would record `sourceStartSample` at the play instant and `sourceEndSample` at the deck's current source position — but after a hot-cue jump, the current source position is *behind* (or far ahead of) the start, producing a clip whose source window is nonsensical (negative length, or a window that silently skips the jumped-over audio). The arrangement, when later played back (EPIC-0010), would not reproduce what the DJ actually heard: it would play a contiguous source span the DJ never performed, ignoring the cue trigger or the jump entirely.

EPIC-0009 §1.3.6 and §1.3.4 define the required semantics: a discontinuity in the deck's source position **closes the active clip at the out-point and opens a new clip at the in-point, contiguous on the timeline** — "the set keeps moving forward in time even though the source jumped." This PRD makes that concrete for the two structural event families that originate source discontinuities during recording: hot-cue triggers (PRD-0012 §1.3.2) and beat jumps (PRD-0015 §1.3.1–§1.3.2). It defines how each emits a paired jump-out / jump-in event carrying its source sample positions, and how the resulting two clips reference the **same** `sourceFileId` with **different** crop windows, laid back-to-back on the timeline.

## 1.2. Objective

The capture system splits the active recording clip at every deck source-position discontinuity caused by a hot-cue trigger or a beat jump, such that:

- A hot-cue trigger during recording (PRD-0012 §1.3.2: a seek from the current playhead to the stored cue position) emits, through the PRD-0072 event bridge, a **jump event** carrying `(deckId, jumpOutSourceSample, jumpInSourceSample, timestamp)`, where `jumpOutSourceSample` is the deck's source position at the instant of the trigger and `jumpInSourceSample` is the cue's stored source position.
- A beat jump during recording (PRD-0015 §1.3.1: a seek by `± beatJumpSize * beatInterval`) emits the same jump event shape, where `jumpInSourceSample` is the clamped, optionally quantized destination computed by PRD-0015 §1.4.2.
- On draining a jump event, the PRD-0073 placement engine **closes** the active clip on the affected lane(s): `sourceEndSample = jumpOutSourceSample`, `timelineEndSample = currentTimelinePosition`.
- The placement engine then **opens** a new clip on the **same** lane(s), contiguous on the timeline: `sourceStartSample = jumpInSourceSample`, `timelineStartSample = the just-closed clip's timelineEndSample` (no gap, no overlap), `sourceFileId = the same source file as the closed clip`.
- The timeline never moves backward. Only the source crop window changes between the two clips. The two clips reference one `sourceFileId` with two distinct, possibly non-adjacent (and possibly overlapping) source windows.
- A source discontinuity whose magnitude `|jumpInSourceSample − jumpOutSourceSample|` does not exceed a small threshold (the natural per-block source advance) does **not** trigger a split (see §1.5.1); only genuine jumps split.
- Hot-cue and beat-jump capture works identically whether the originating event fired on the audio thread (a quantized cue, PRD-0012/PRD-0013) or the message thread (a UI button click), because both flow through the single ordered PRD-0072 stream (EPIC-0009 §1.3.3).
- The split applies to **all** lanes the deck is currently writing (per the active source mode from EPIC-0002): if the deck is in stems mode writing four lanes, all four open clips close and four new clips open at the same timeline boundary.
- No audio-thread code is added; the audio thread only enqueues a small POD jump record into the pre-allocated PRD-0072 FIFO. All clip close/open work happens on the message thread.

## 1.3. User Flow

### 1.3.1. Hot-Cue Stutter During Recording

1. The DJ is recording. Deck A is playing its track; the capture engine has an open clip on Deck A's lane: `sourceStartSample = 2,000,000`, growing, `timelineStartSample = 1,323,000` (timeline 0:30 at 44.1 kHz).
2. Ten seconds of timeline later, the deck's source position has advanced to `2,441,000` and the timeline playhead is at `1,764,000`. The DJ presses hot cue C, stored at source sample `88,200` (the first downbeat).
3. PRD-0012's trigger path issues a transport seek from `2,441,000` to `88,200`. At the same instant it emits a jump event to the PRD-0072 bridge: `jumpOutSourceSample = 2,441,000`, `jumpInSourceSample = 88,200`, `timestamp = now`.
4. The message-thread drain reads the event. The placement engine closes Deck A's open clip: `sourceEndSample = 2,441,000`, `timelineEndSample = 1,764,000`. It immediately opens a new clip on the same lane: `sourceStartSample = 88,200`, `timelineStartSample = 1,764,000`, same `sourceFileId`.
5. The DJ presses hot cue C three more times in rhythm. Each press emits a jump event; each closes the tiny just-opened clip (out-point at wherever the source advanced to since the last trigger, e.g. `88,200 + 5,000`) and opens another at `88,200`. The result is a run of short, back-to-back clips on the timeline, each a crop starting at `88,200` — faithfully reproducing the stutter when later played back.
6. The DJ stops pressing and lets the deck play. The final open clip grows normally from `88,200` until the next structural event.

### 1.3.2. Beat Jump Forward During Recording

7. The DJ is recording with Deck B playing; an open clip exists with `sourceStartSample = 420,000`. `beatJumpSize = 4`, `beatInterval = 21,000` samples (126 BPM).
8. The deck's source position is `462,000`. The DJ presses Beat Jump Forward. PRD-0015 computes the destination `462,000 + 4 × 21,000 = 546,000` (quantized/clamped per PRD-0015 §1.4.2) and issues the transport seek.
9. The same action emits a jump event: `jumpOutSourceSample = 462,000`, `jumpInSourceSample = 546,000`.
10. The placement engine closes the open clip at source `462,000` / current timeline, and opens a new clip at source `546,000` on the same lane, contiguous on the timeline. The arrangement now contains two clips referencing the same source file: one crop `[420,000 … 462,000]` and one crop opening at `546,000` — the `[462,000 … 546,000]` source span (the jumped-over audio) is correctly absent, because the DJ never heard it.

### 1.3.3. Beat Jump Backward (Replay) During Recording

11. Deck B's source is at `546,000`. The DJ presses Beat Jump Backward (size 4): destination `546,000 − 84,000 = 462,000`.
12. The jump event carries `jumpOutSourceSample = 546,000`, `jumpInSourceSample = 462,000`. The engine closes at `546,000`, opens at `462,000`, contiguous on the timeline.
13. The new clip's source window now *overlaps* the earlier clip's window (`462,000` was already played once). This is correct and expected: the DJ replayed that section, so the arrangement reproduces it twice, back-to-back on the moving timeline.

### 1.3.4. Stems Mode: Multi-Lane Split

14. Deck C is in stems mode (EPIC-0002), writing four lanes (drums, bass, melody, vocals); four open clips exist, all sharing the deck's source position.
15. The DJ presses a hot cue. One jump event is drained; the placement engine closes all four open clips at the same `jumpOutSourceSample` / timeline boundary and opens four new clips at the same `jumpInSourceSample` / timeline boundary, one per lane. All four splits land on the identical timeline sample so the lanes stay aligned.

### 1.3.5. Jump While No Clip Is Open

16. The DJ has the deck paused (or muted) during recording, so no clip is open on its lane. The DJ presses a hot cue or beat jump (PRD-0012 §1.3.2 / PRD-0015 §1.3.4 both allow jumps while paused).
17. The capture engine receives the jump event but finds no open clip to close. It performs no split and writes nothing; the jump only repositions the deck's source pointer (see §1.5.6). When the DJ next unmutes/plays, the new clip opens at the then-current (post-jump) source position via the normal play-event path (PRD-0073).

## 1.4. Acceptance Criteria

- [ ] A hot-cue trigger (PRD-0012 §1.3.2) occurring while recording emits exactly one jump event through the PRD-0072 event bridge, carrying `deckId`, `jumpOutSourceSample` (the deck source position at the trigger instant), `jumpInSourceSample` (the cue's stored source position), and a timestamp.
- [ ] A beat jump (PRD-0015 §1.3.1–§1.3.2) occurring while recording emits exactly one jump event of the same shape, with `jumpInSourceSample` equal to the clamped/quantized destination from PRD-0015 §1.4.2 and `jumpOutSourceSample` equal to the pre-jump source position.
- [ ] The jump event record enqueued on the audio thread (quantized cue case) is a fixed-size POD; enqueuing it performs no allocation, takes no lock, and does no I/O, per `AGENTS.md` and EPIC-0009 §1.3.3.
- [ ] On draining a jump event for a deck with an open clip on lane L, the placement engine sets the open clip's `sourceEndSample = jumpOutSourceSample` and `timelineEndSample = currentTimelinePosition`, finalising it.
- [ ] Immediately after closing, the engine opens a new clip on the same lane L with `sourceStartSample = jumpInSourceSample`, `timelineStartSample` equal to the closed clip's `timelineEndSample` (contiguous: no gap, no overlap on the timeline), and the **same** `sourceFileId` as the closed clip.
- [ ] The two resulting clips reference one identical `sourceFileId` and differ only in their source crop window; the source windows may be non-adjacent (forward jump) or overlapping (backward jump).
- [ ] The timeline `timelineStartSample` of the new clip is greater than or equal to the closed clip's `timelineStartSample` (the timeline never moves backward), regardless of jump direction.
- [ ] A source discontinuity whose magnitude `|jumpInSourceSample − jumpOutSourceSample|` is at most the split threshold (defined in §1.5.1) does not produce a split; the active clip continues growing uninterrupted.
- [ ] Any source discontinuity larger than the threshold — whether from a hot cue or a beat jump — produces a close+open split (§1.5.1: all discontinuities above threshold split).
- [ ] When the deck is writing multiple lanes (stems mode, EPIC-0002), a single jump event closes every open clip on those lanes and opens one new clip per lane, all sharing the same `timelineEndSample` / `timelineStartSample` boundary so the lanes remain time-aligned.
- [ ] A jump event drained for a deck that has no open clip (paused/muted, §1.3.5) produces no clip and no error; it does not create a zero-length clip.
- [ ] Rapid successive jumps (cue juggling) each produce a split; the minimum-clip-length / coalescing policy of §1.5.4 governs how very short resulting clips are handled, and no clip with `sourceEndSample < sourceStartSample` or `timelineEndSample < timelineStartSample` is ever written.
- [ ] The split path runs entirely on the message thread; the audio thread contributes only the enqueued POD jump record.
- [ ] Whether the jump originated on the audio thread (quantized cue) or the message thread (UI click), the resulting split is identical, because both pass through the single ordered PRD-0072 stream.
- [ ] No continuous parameter automation (EQ, filter, gain, tempo) is captured by this PRD; only the structural source discontinuity is recorded (EPIC-0009 §1.2.2).
- [ ] No playback or editing of the resulting clips is performed by this PRD (EPIC-0010); this PRD only writes the split clips into the `daw` `ValueTree`.

## 1.5. Grey Areas

### 1.5.1. Distinguishing a Deliberate Cue/Jump From Normal Source Advance

Every `processBlock` advances the deck's source position by the block size; a needle-drop or waveform seek (EPIC-0001) also produces a source discontinuity. The capture engine must decide which discontinuities split the clip and which do not.

**Resolution:** Any source discontinuity larger than a small threshold closes the active clip and opens a new one — the split logic is **source-driven**, not event-type-driven. The threshold is the maximum legitimate per-projection source advance (the projection block size used by PRD-0073's grow cadence, plus a small margin); a normal forward advance of one block is below it and never splits, while a hot-cue trigger, a beat jump, *and* a manual needle-drop/seek are all above it and all split. This is the correct and simplest policy: the recorder's job is to reproduce what was heard, and a needle-drop changes what is heard exactly as a cue does. Treating all three uniformly means PRD-0072 does not need to tag events by intent for splitting purposes — it only needs to surface the out/in source positions. (The event *type* is still carried for other consumers, e.g. loop capture in PRD-0076, but the split decision keys off the source delta.) Hot cues and beat jumps are simply the two *named* sources of discontinuity this PRD enumerates; the engine handles any source jump through the same path.

### 1.5.2. Quantized Cue Firing on the Audio Thread vs the Message Thread

A hot-cue trigger may fire on the audio thread when quantize is enabled (PRD-0013): the actual seek is deferred to the next beat boundary inside `processBlock`, so the `jumpOutSourceSample` is only known at that audio-thread instant. A non-quantized cue, or a beat jump from a UI click, originates on the message thread.

**Resolution:** The PRD-0072 event bridge handles both, and this PRD relies on that. For an audio-thread-fired quantized cue, the audio thread captures `jumpOutSourceSample` and `jumpInSourceSample` at the moment the seek executes and enqueues a POD jump record into the pre-allocated FIFO — no allocation, no lock, no I/O. For a message-thread cue/jump, the same record is enqueued through the same path so there is one ordered stream (EPIC-0009 §1.3.3). The placement engine never distinguishes the origin thread; it only drains records. This keeps the split logic origin-agnostic and audio-thread-safe, and it correctly captures the *post-quantize* source position (the seek's true landing point), not the pre-quantize button-press position.

### 1.5.3. Contiguity vs Leaving a Gap on the Timeline

When a clip closes at a jump-out and a new one opens at a jump-in, the new clip could either start exactly where the old one ended (contiguous) or leave a small timeline gap representing the imperceptible time the seek took.

**Resolution:** Strictly contiguous — `newClip.timelineStartSample = closedClip.timelineEndSample`, no gap and no overlap. EPIC-0009 §1.3.6 is explicit: the split is "contiguous on the timeline (the set keeps moving forward in time even though the source jumped)." A hot-cue or beat-jump seek is effectively instantaneous (a single `processBlock` crossfade, PRD-0012/PRD-0015); there is no real silence to represent. Introducing a gap would create audible holes in the played-back arrangement that never existed in the live performance. Genuine silence (the deck stopped or muted) is represented separately by *closing* a clip with no immediate re-open (PRD-0073), which is a different event family than a jump.

### 1.5.4. Very Rapid Cue Juggling Producing Many Tiny Clips

A DJ juggling a hot cue on every 1/16 note can fire dozens of triggers per second, each producing a close+open. This could generate a flood of extremely short clips (a few hundred samples each), bloating the `daw` model.

**Resolution:** Each genuine trigger produces its own clip — the arrangement must faithfully reproduce the juggle, so the clips are *not* discarded. However, two safeguards apply: (a) a clip whose grown length is below a minimum-clip-length floor (a small musical value, e.g. one projection block) is still written but flagged so EPIC-0010 playback can render it as a discrete re-trigger rather than attempting a sub-block crossfade; (b) the placement engine never writes a degenerate clip (`sourceEndSample < sourceStartSample` or a zero/negative timeline length) — if two jump events arrive within the same drain with no source advance between them, the intermediate zero-length clip is coalesced away (the close and the immediately following open collapse to a single boundary). This bounds the worst case to one clip per audible re-trigger while preventing zero-length artefacts. True per-sample coalescing into a "loop" construct is **not** done here — repeated identical crops are the loop-capture concern of PRD-0076; this PRD only guarantees correctness and non-degeneracy of the split clips.

### 1.5.5. Whether the New Clip Re-Runs the Alignment Resolver

PRD-0074's alignment resolver decides, at clip open, whether the clip is grid-aligned (BPM matches master) or first-beat-anchored. A jump opens a new clip mid-performance — does that new clip re-run the resolver, or inherit the closed clip's alignment?

**Resolution:** The new clip re-runs the PRD-0074 resolver at its open, exactly as any freshly opened clip does. Alignment is a property of "a clip opening at a source position against the master grid," and a jump-in is precisely that. Re-running the resolver is correct because the deck's BPM-match / phase-aligned state may have changed since the previous clip opened (the DJ may have nudged tempo or sync between the open and the jump), and because the jump-in source position has its own downbeat that must be anchored to the grid. Inheriting the prior clip's alignment would mis-anchor the new crop. The contiguity guarantee of §1.5.3 constrains only the *timeline* start (it must equal the closed clip's timeline end); the resolver still chooses grid-snap vs first-beat-anchor for that fixed timeline position — in practice, because the timeline start is already pinned for contiguity, the resolver's role for a jump-in is to classify alignment metadata for playback, not to move the clip. This PRD defers the precise interaction of "pinned contiguous start" vs "resolver snap" to PRD-0074's contract and simply requires that the resolver is invoked.

### 1.5.6. Hot Cue or Beat Jump While the Deck Is Paused

PRD-0012 §1.3.2 and PRD-0015 §1.3.4 both permit triggering while the deck is paused/stopped: the source pointer moves but no audio is produced. During recording, no clip is open on a paused deck's lane.

**Resolution:** A jump while paused produces a jump event (PRD-0072 still surfaces it) but no split, because there is no open clip to close and nothing was heard. The engine updates its notion of the deck's current source pointer so that the *next* play/unmute event opens its clip at the post-jump source position (via PRD-0073's normal open path), but it writes no clip for the paused jump itself. This is correct: capture records what was *heard*, and a paused jump is silent. Writing a zero-length or phantom clip would corrupt the arrangement. The acceptance criteria explicitly forbid creating a clip for a jump with no open clip (§1.3.5).

### 1.5.7. A Jump That Lands Inside an Active Loop

A beat jump can occur while a loop is engaged (PRD-0015 §1.3.5 shifts the loop region), and a hot cue can land inside or outside a loop. Loop capture is PRD-0076's concern, which renders the looped segment as repeated back-to-back crops.

**Resolution:** This interaction is deferred to PRD-0076. When a loop is active, PRD-0076 owns the clip-rendering policy for the looped region (repeated crops per pass). A jump that occurs while a loop is engaged is surfaced by PRD-0072 as a jump event as usual, but how it composes with the loop's repeated-crop rendering — e.g. a beat-jump loop-shift that moves both loop boundaries, or a cue that exits the loop — is resolved by PRD-0076's loop-capture contract, which has visibility into the loop state. This PRD guarantees only the non-loop split behaviour (close at out, open at in, contiguous) and the degeneracy safeguards (§1.5.4); it does not attempt to define loop-interaction semantics here, to avoid a contract conflict with PRD-0076. When no loop is active, this PRD's split rules apply unconditionally.
