---
status: Implemented
epic: EPIC-0010
depends-on:
  - PRD-0064
  - PRD-0066
  - PRD-0081
---

# 1. PRD-0082: DAW Transport

## 1.1. Problem

By the time this PRD is reached, the timeline render engine (PRD-0081) can turn
the recorded arrangement back into audio: given an authoritative playhead
position in project samples, it resolves which clips overlap the current block,
copies pre-rolled source samples from the streaming layer (PRD-0080), applies
clip gain, and sums the active lanes into the master output click-free. But
nothing **moves** that playhead. The render engine is a pure function of "where
is the playhead now" — it has no notion of Play, Pause, Stop, scrubbing, or
looping a region for review. Without a transport, the recorded arrangement is a
static picture that can be drawn but never heard.

This is a different transport from the ones that already exist, and conflating
them would be a serious architectural error:

- The **decks** (EPIC-0003) each own their own transport — Play/Cue/Sync that
  drives the live now-position of a loaded track. Those are live performance
  instruments and must keep working untouched.
- EPIC-0008 introduced a **live now-line** (PRD-0070): a vertical cursor that
  marks where the live deck projection currently is on the timeline as a set is
  performed. It follows the decks; it is not a player.
- EPIC-0009 introduced a **record playhead**: the write cursor that marks where
  incoming live audio is being committed onto the arrangement during capture.

The DAW transport this PRD adds is a **fourth, independent** cursor: a
**playback playhead** over the recorded arrangement, decoupled from the decks
entirely. The DJ must be able to press Play in the DAW panel and hear the mix
they recorded — reconstructed from source files by PRD-0081 — while the decks
may simultaneously be playing something completely different live. There is no
surface today that owns the arrangement playhead position, advances it on the
audio thread, publishes it lock-free to both the renderer and the UI, and
exposes Play / Pause / Stop / scrub / region-loop semantics. Without it, every
later editing PRD (PRD-0083 onward) has no way to audition its edits, and the
Epic's core promise — "press Play and hear your set" — cannot be met.

## 1.2. Objective

The system provides a `DawTransport` that owns the arrangement playback playhead
and drives the PRD-0081 renderer, such that:

- A `DawTransport` (`Source/Features/Daw/Playback/DawTransport.h/.cpp`) owns the
  authoritative arrangement playhead position in **project samples** and a
  transport state (`Stopped`, `Playing`, `Paused`).
- The playhead is advanced **on the audio thread** by the block size each
  `processBlock` while `Playing`, and is published lock-free (a single
  `std::atomic<int64_t>` sample position plus an atomic state enum) so that both
  the PRD-0081 renderer (which reads it to resolve active clips) and the UI
  (which draws the cursor and time readout) observe a coherent value without
  locks, allocation, or I/O on the audio thread.
- **Play** starts advancing the playhead from its current position (play-from-
  position, not always from zero). **Pause** halts advancement and **holds** the
  current position. **Stop** halts advancement and resets to the transport
  origin (see §1.5.1). The state machine is `Stopped → Playing`,
  `Playing → Paused`, `Paused → Playing`, and any state `→ Stopped`.
- **Scrub** lets the message thread set the playhead to an arbitrary project
  sample (from a UI drag on the ruler / playhead, or a transport time field).
  A scrub while `Playing` keeps playing from the new position; a scrub while
  `Stopped`/`Paused` repositions silently. Every scrub re-primes the streaming
  layer (PRD-0080) off the audio thread so playback after the seek is click-free.
- **Region / loop review**: the DJ can select a timeline range
  `[loopStartSample, loopEndSample)` and arm loop mode. While looping is armed
  and `Playing`, when the playhead reaches `loopEndSample` it wraps seamlessly
  to `loopStartSample` (see §1.5.3) so the region repeats for listening.
- Transport control **buttons** (Play, Pause, Stop, and a loop-arm toggle) are
  rendered in the DAW panel (PRD-0066) as `DESIGN.md` tactile buttons: massive
  squares, mandatory `2px solid #2d2d2d` border, with the inverted active state
  (`#2d2d2d` fill / `#fdfdfd` glyph when engaged, inverse when idle).
- The transport is **completely independent** of the decks: starting, pausing,
  stopping, scrubbing, or looping the DAW transport has **no effect** on any
  deck transport, and vice versa. The decks may play live while the DAW plays
  the recorded arrangement at the same time.
- The arrangement playhead is a **distinct cursor** from the EPIC-0008 live
  now-line and the EPIC-0009 record playhead; all three may be visible at once
  and are visually differentiated (see §1.5.4).
- During playback the panel may **follow / auto-scroll** so the playhead stays
  in view, reusing the follow-mode behaviour delivered by PRD-0070 (see §1.5.5).
- No deck, mixer, automation, or clip-editing behaviour is added or modified by
  this PRD.

## 1.3. User Flow

1. The DJ has a recorded arrangement in the DAW panel and the renderer
   (PRD-0081) wired to the master output. The transport is `Stopped` with the
   playhead at the origin (sample 0). The Play button shows its idle state.

2. The DJ presses **Play**. The transport transitions to `Playing`. The audio
   thread begins advancing the playhead by the block size each block; PRD-0081
   reads the published position and sums the active clips. The DJ hears the
   recorded mix from sample 0. The Play button inverts to its active state
   (`#2d2d2d` fill). The playhead cursor scrubs left-to-right across the ruler.

3. The DJ presses **Pause**. Advancement halts; the playhead holds its current
   position. The transport is `Paused`; the Pause button shows active, Play
   reverts to idle. No audio is produced. Pressing **Play** again resumes from
   the held position.

4. The DJ drags the playhead (or edits the time field) to **scrub** to a new
   position mid-song. The streaming layer re-primes off the audio thread; on the
   next Play (or immediately, if already playing) audio resumes click-free from
   the scrubbed position.

5. The DJ selects a timeline range and presses the **loop-arm** toggle, then
   presses **Play**. The region between `loopStartSample` and `loopEndSample`
   plays and seamlessly repeats. The DJ auditions an edit candidate by listening
   to the looped region. Disarming the loop lets playback continue past
   `loopEndSample` on the next pass.

6. The DJ presses **Stop**. Advancement halts and the playhead returns to the
   transport origin (§1.5.1). All transport buttons return to idle; the renderer
   produces silence. Throughout steps 2–6 the live decks, if playing, are
   completely unaffected — the DJ can be cueing a live track while the recorded
   arrangement plays and loops independently.

## 1.4. Acceptance Criteria

- [ ] A `DawTransport` exists at
      `Source/Features/Daw/Playback/DawTransport.h/.cpp`, constructed with its
      dependencies injected (no singletons, per `CLAUDE.md`).
- [ ] The transport owns the authoritative arrangement playhead as a project
      sample position and a transport state enum (`Stopped`, `Playing`,
      `Paused`).
- [ ] While `Playing`, the playhead advances by exactly the block size on each
      `processBlock` call on the audio thread.
- [ ] The playhead position and transport state are published to other threads
      exclusively via `std::atomic` (a single `int64_t` sample position and an
      atomic state); no lock, allocation, `std::string`, `std::vector` growth,
      disk/network I/O, or console logging occurs on the audio-thread path.
- [ ] PRD-0081's renderer reads the published atomic playhead position to resolve
      the active clips for each block; the renderer is not modified to walk the
      `ValueTree` or to own playback state.
- [ ] **Play** from `Stopped` or `Paused` starts advancement from the current
      playhead position (not forced to zero).
- [ ] **Pause** halts advancement and holds the current position; a subsequent
      **Play** resumes from that held position.
- [ ] **Stop** halts advancement and resets the playhead to the transport origin
      (§1.5.1); the renderer produces silence while `Stopped`.
- [ ] **Scrub** sets the playhead to an arbitrary project sample from the message
      thread; a scrub while `Playing` continues playback from the new position,
      and a scrub while `Stopped`/`Paused` repositions without producing audio.
- [ ] Every scrub/seek triggers a re-prime of the PRD-0080 streaming layer **off
      the audio thread**, so audio after the seek is click-free.
- [ ] Region-loop state (`loopStartSample`, `loopEndSample`, armed flag) is owned
      by the transport; while armed and `Playing`, reaching `loopEndSample` wraps
      the playhead to `loopStartSample` seamlessly (§1.5.3) with no audible click
      or gap.
- [ ] Disarming the loop while `Playing` lets the playhead continue past
      `loopEndSample` on the next pass without interrupting playback.
- [ ] Transport buttons (Play, Pause, Stop, loop-arm) render in the DAW panel
      (PRD-0066) as `DESIGN.md` tactile buttons: massive squares, `2px solid
      #2d2d2d` border at all times, zero border-radius, with the active state
      drawn as `#2d2d2d` fill / `#fdfdfd` glyph and the idle state as `#fdfdfd`
      fill / `#2d2d2d` glyph.
- [ ] Engaging the DAW transport (Play/Pause/Stop/scrub/loop) has **no effect**
      on any deck transport, and operating a deck transport has no effect on the
      DAW transport; the two run concurrently and independently.
- [ ] The arrangement playhead is a distinct, visually differentiated cursor from
      the EPIC-0008 live now-line and the EPIC-0009 record playhead (§1.5.4);
      where more than one is present they are individually identifiable.
- [ ] During `Playing`, the panel follows/auto-scrolls to keep the playhead in
      view by reusing PRD-0070's follow-mode mechanism (§1.5.5), and follow is
      suspended when the user manually scrolls, consistent with PRD-0070.
- [ ] A test suite under `Tests/` (e.g. `DawTransportTests.cpp`) covers: the
      Play/Pause/Stop state machine and position semantics; block-accurate
      advancement; scrub repositioning and streamer re-prime triggering;
      region-loop wrap behaviour at `loopEndSample`; and independence from deck
      transports.
- [ ] No deck, mixer, automation, or clip-editing logic is added or modified by
      this PRD; the transport's only audio-side coupling is advancing the
      playhead that PRD-0081 consumes.

## 1.5. Grey Areas

### 1.5.1. Stop Semantics: Return to Zero vs Hold Position (vs Pause)

Three behaviours are in tension. **Pause** clearly holds position (that is its
defining purpose). **Stop** could either (a) return the playhead to sample 0
(the project origin), (b) return to a "transport origin" that the user set (the
position Play was last started from), or (c) simply hold position like Pause —
in which case Stop and Pause become nearly redundant.

**Resolution:** Stop returns the playhead to the **transport origin**, defined
as the position from which playback was last started (defaulting to sample 0
when nothing has been played yet, and updated whenever the user scrubs while
`Stopped`). This gives Stop and Pause clearly distinct, conventional meanings:
Pause = "freeze here, resume from here"; Stop = "I'm done with this take, snap
back to where I started so I can replay it from the top." It matches the mental
model from every DAW and from the decks' own Cue-point behaviour. Implementation:
the transport records `originSample` on each `Stopped → Playing` transition and
on each scrub-while-stopped; Stop sets the playhead to `originSample`. A future
preference (e.g. "Stop holds position") can be layered on without changing this
PRD's default, since the origin is already tracked.

### 1.5.2. Publishing the Playhead to the Audio Thread and Renderer

PRD-0081's renderer needs "where is the playhead now" every block, and the UI
needs it every frame, but the transport state lives conceptually on the message
thread (button presses, scrubs). The mechanism must be lock-free and must not
tear (a 64-bit sample position read mid-update must never be half-old/half-new).

**Resolution:** The playhead is a single `std::atomic<int64_t>` of the project
sample position, and the transport state is a separate `std::atomic` enum. The
**audio thread is the writer** of the playhead while `Playing` (it advances and
stores the new position each block); the **message thread is the writer** for
discontinuous moves (scrub, Stop-to-origin, Play-from-position, loop-wrap setup),
which it performs while advancement is paused or by handing the new position to
the audio thread via the same atomic. Because the position is a single aligned
64-bit atomic, reads never tear on the supported platforms. Command intent
(Play/Pause/Stop/scrub-target) flows message → audio thread via a small lock-free
command FIFO or atomic flags, never via a lock. This mirrors the master-clock
publication pattern already proven in EPIC-0003 and keeps the audio-thread path
allocation-free and lock-free per `CLAUDE.md`.

### 1.5.3. Region-Loop Seamlessness (Re-Priming Streamers at the Loop Point)

A naive loop sets the playhead back to `loopStartSample` when it crosses
`loopEndSample`, but the streaming layer (PRD-0080) has pre-rolled audio for the
*linear* continuation past `loopEndSample`, not for `loopStartSample`. Without
care, the loop wrap produces a gap or a click while the streamers re-prime.

**Resolution:** Loop seamlessness is achieved by **predictive re-priming**: when
loop mode is armed, the streaming layer is told the loop boundary so it pre-rolls
the `loopStartSample` region *ahead of time* (as the playhead approaches
`loopEndSample`), exactly as a deck loop pre-buffers its loop-in point. The audio
thread performs the wrap as a pure playhead reset to `loopStartSample`; the
samples it then reads are already resident in the ring buffers because the
background reader pre-primed them. The existing short anti-click ramp at clip
boundaries (PRD-0081 §1.3.1) covers any residual discontinuity at the seam. The
detailed pre-roll scheduling is delegated to PRD-0080's streamer contract; this
PRD's responsibility is to publish the armed loop boundaries early enough for the
streamers to act, and to perform the wrap as a lock-free playhead reset.

### 1.5.4. Three Distinct Cursors: Transport Playhead vs Now-Line vs Record Playhead

The timeline can simultaneously show this PRD's **arrangement playback playhead**,
EPIC-0008's **live now-line** (where the live deck projection currently sits),
and EPIC-0009's **record playhead** (the write cursor during capture). If all
three look alike the panel becomes unreadable.

**Resolution:** The three cursors are visually differentiated within the strict
monochrome palette using **weight, fill, and glyph** rather than colour (per
`DESIGN.md`). The **playback playhead** (this PRD) is a solid `#2d2d2d` vertical
line with a downward triangle cap at the ruler — the "where playback is" marker.
The **live now-line** (PRD-0070) keeps its existing treatment as the live-edge
indicator. The **record playhead** (EPIC-0009) is rendered with a distinct
dithered/hatched fill to read as "armed/writing." Only one of record vs playback
is typically active at a time (you record, then you play back), but the now-line
can coexist with either since the decks may always be live. Exact tick caps and
hatching are an implementation detail derived from `DESIGN.md`; the contract here
is that the playback playhead is unambiguously distinguishable from the other two.

### 1.5.5. Follow / Auto-Scroll During Playback

When the playhead advances past the right edge of the viewport, it should keep
the playhead visible. PRD-0070 already delivered a follow-mode / auto-scroll
mechanism for the live now-line. This PRD could either build its own follow loop
or reuse PRD-0070's.

**Resolution:** Reuse PRD-0070's follow mode. The follow mechanism is agnostic
about *which* cursor it tracks — it pages the `TimelineTransform` scroll offset
to keep a target sample position in view and suspends when the user scrolls
manually. This PRD feeds the **playback playhead** position to that same
mechanism while `Playing` (and the live now-line continues to feed it when the
decks are the focus). When both are relevant, the active transport's playhead is
the follow target; manual scroll suspends follow exactly as PRD-0070 specifies.
No new follow loop is written; this avoids two competing auto-scroll controllers
fighting over the viewport.

### 1.5.6. Spacebar / Keyboard Transport Control

Every DAW maps the spacebar to Play/Pause and often other keys to Stop, loop
toggle, and scrub. Wiring global keyboard shortcuts touches focus handling,
conflicts with the decks' own potential shortcuts, and risks a key press meant
for a text field (track search) toggling playback unexpectedly.

**Resolution:** Keyboard control is **deferred** out of this PRD. This PRD ships
the transport engine and its on-screen tactile buttons (mouse / MIDI-reachable
via the existing control surface). A dedicated later PRD will add keyboard
shortcuts with proper focus arbitration — deciding when the DAW transport owns
the spacebar versus a focused deck or a text field — because that arbitration is
a cross-cutting concern (decks, DAW, library search all want keys) that deserves
its own design rather than being bolted onto the transport here. The transport's
public API (`play()`, `pause()`, `stop()`, `scrubTo()`, `setLoop()`) is shaped so
a future keyboard layer can drive it without modification.

### 1.5.7. Interaction with Decks Playing Simultaneously

The decks have their own transports and may be playing live audio while the DJ
auditions the recorded arrangement. It must be unambiguous whether the DAW
transport and the deck transports interact, share a clock, or compete for output.

**Resolution:** They are **fully independent and additive**. The DAW transport
advances the arrangement playhead and PRD-0081 sums the reconstructed clips into
the master output; the decks independently sum their live audio into the same
master output (EPIC-0007). Both can be active at once and their audio simply
mixes — this is intentional, mirroring how a producer can monitor a live input
while a project plays. The DAW transport does **not** start, stop, or sync the
decks, and pressing a deck's Play does not move the arrangement playhead. The two
share only the project sample rate and the master output bus, not a transport
clock. If a future Epic wants the DAW transport to optionally drive or sync the
decks (a "play the whole session" master mode), that is an additive feature on
top of this independent baseline, not a change to it.
