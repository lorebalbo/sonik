---
status: Implemented
epic: EPIC-0003
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0004
  - PRD-0008
---

# 1. PRD-0026: Master Clock Core

## 1.1. Problem

When a DJ mixes two or more tracks simultaneously, each deck runs at its own independently computed BPM with no shared time reference. There is no mechanism to designate one deck as the authoritative tempo source that other decks should follow. Even if a future sync engine reads each deck's BPM and attempts to match speeds, without a single coherent clock publication all slave decks read tempo and phase data at different moments during an audio buffer cycle, producing inconsistent corrections and audible drift.

This incoherence is not merely a technical defect — it is a user-facing failure. A DJ who relies on sync to maintain a tight mix will hear the beatgrids slip apart within seconds. In a live performance setting, recovering from slipped grids requires manual intervention at exactly the moment the DJ's attention is on the crowd. Every downstream sync feature — tempo lock, phase correction, phase meters — is unreliable without a stable, race-free master clock at its foundation.

Additionally, without a defined master-election policy, the DJ has no clear mental model of which deck owns the reference tempo when tracks are loaded, stopped, or ejected. Professional DJ software resolves this with an explicit MASTER button and an automatic promotion rule so that the global tempo reference is always well-defined.

## 1.2. Objective

The system provides a lock-free master clock infrastructure that:
- Defines a `MasterClockSnapshot` struct as the canonical, atomic unit of master clock state, containing `masterBPM (double)`, `masterPhaseOriginSample (int64_t)`, and `masterIsPlaying (bool)`.
- Publishes the snapshot via a SeqLock double-buffer so that the audio thread always reads a coherent snapshot (all three fields from the same write) with zero allocations and zero locks on the audio thread.
- Manages master deck election through `MasterClockManager`, a `juce::ValueTree::Listener` that runs exclusively on the message thread and enforces mutual exclusion (exactly one `isMaster = true` at any time, or dormant state when no deck is playing).
- Provides automatic master promotion: when the master deck stops or its track is ejected, the lowest-index currently-playing deck is promoted; if no deck is playing, the clock enters a dormant state that preserves the last known BPM.
- Supports manual override: the user presses MASTER on any deck to immediately reassign the master role.
- Introduces the ValueTree sync identifiers `IDs::isMaster`, `IDs::isSynced`, and `IDs::masterDeckIndex` as the sole authoritative state for master and sync status across the application.

## 1.3. User Flow

1. The user launches Sonik. No tracks are loaded. `masterDeckIndex = -1`. The clock is in the dormant state. No deck shows MASTER as active.
2. The user loads a track onto Deck A. Deck A transitions from Empty to Stopped. Because no other master exists, `MasterClockManager` automatically promotes Deck A: `IDs::isMaster` is set to `true` on Deck A, `masterDeckIndex` is set to `0` (Deck A's index), and a `MasterClockSnapshot` is published with Deck A's detected BPM and phase origin. Deck A's MASTER button lights up.
3. The user loads a second track onto Deck B. Deck B transitions to Stopped. `MasterClockManager` takes no action — a master already exists (Deck A). Deck B's MASTER button remains unlit.
4. The user presses Play on Deck A. `MasterClockManager` observes the playback status change and writes `masterIsPlaying = true` into the published snapshot.
5. The user presses Play on Deck B and activates SYNC on Deck B. Deck B's `isSynced` is set to `true`. The sync engine (PRD-0027) reads the `MasterClockSnapshot` via `MasterClockPublisher::read()` on every audio block and begins adjusting Deck B's speed multiplier to match Deck A's BPM.
6. The user presses MASTER on Deck B. `MasterClockManager` immediately demotes Deck A (`isMaster = false`) and promotes Deck B (`isMaster = true`). A new snapshot is published with Deck B's BPM and phase origin. Deck A's MASTER button goes dark; Deck B's lights up.
7. The user pauses Deck B (the current master). `MasterClockManager` writes `masterIsPlaying = false` into the published snapshot. `masterPhaseOriginSample` and `masterBPM` are not changed. The sync engine on slave decks reads `masterIsPlaying = false` and suspends phase correction while continuing to hold the last tempo-matched speed.
8. The user resumes Deck B. `MasterClockManager` writes `masterIsPlaying = true`. The sync engine resumes phase correction from the preserved phase origin.
9. The user stops Deck B entirely (Stop command, not Pause). Deck B transitions to Stopped. `MasterClockManager` checks for the lowest-index currently-playing deck. Deck A is playing, so Deck A is promoted as master. Deck B's MASTER button goes dark; Deck A's lights up. A new snapshot is published with Deck A's BPM and phase origin.
10. The user ejects Deck A's track while Deck A is playing. Deck A transitions to Empty. `MasterClockManager` applies the same auto-promotion logic as a stop event. If no other deck is playing, the clock enters dormant state (`masterDeckIndex = -1`, `masterIsPlaying = false`, `masterBPM` holds Deck A's last known BPM).
11. The user loads a new track onto Deck B and presses Play. `MasterClockManager` detects that a deck has transitioned to Playing while `masterDeckIndex = -1`. Deck B is immediately promoted as master.

### 1.3.1. Edge Cases

- If all decks are simultaneously paused (not stopped), the clock holds the master deck's identity but writes `masterIsPlaying = false`. No auto-promotion occurs; resuming the master deck restores normal clock behavior.
- If all decks are stopped or empty at the same time, the clock enters dormant state. `masterDeckIndex` is set to `-1`. `masterBPM` retains the last published value so that if a deck is loaded and started, the display shows a meaningful reference before the new track's BPM is analyzed.
- If the master deck is removed from the deck layout entirely (not just stopped), `MasterClockManager` treats this as an eject event and applies auto-promotion.
- If two decks have identical BPM values, the promotion logic proceeds identically — the ratio computed by the sync engine is 1.0 and phase correction converges immediately. No special case is needed.
- If only one deck exists in the layout, it is always auto-promoted as master. Pressing MASTER on it is a no-op. There is no slave to sync; `isSynced` on the only deck is a valid but inert state.
- If the user presses MASTER on the deck that is already the master and it is currently playing, the action is a no-op and no state mutation occurs.
- If a deck is mid-load (decoding in progress) when auto-promotion fires, `MasterClockManager` defers publishing the snapshot until the deck's BPM is available (non-zero). The clock briefly remains in the previous state.

## 1.4. Acceptance Criteria

- [ ] `MasterClockSnapshot` is a plain struct in `Source/Features/Sync/MasterClockSnapshot.h` containing exactly three fields: `double masterBPM`, `int64_t masterPhaseOriginSample`, and `bool masterIsPlaying`.
- [ ] `MasterClockPublisher` wraps a SeqLock consisting of a `std::atomic<uint32_t> sequence` counter and a `MasterClockSnapshot` buffer; both are stored as non-heap members (no dynamic allocation at construction).
- [ ] `MasterClockPublisher::publish(const MasterClockSnapshot&)` increments `sequence` to an odd value before writing the snapshot fields, then increments `sequence` to an even value after writing.
- [ ] `MasterClockPublisher::read(MasterClockSnapshot&)` reads `sequence` before and after copying the snapshot; if either value is odd or the two values differ, it retries. It does not allocate memory, take a lock, or perform I/O.
- [ ] Under normal operation (no concurrent write), `MasterClockPublisher::read()` completes in exactly one iteration.
- [ ] `IDs::isMaster` is defined as a `juce::Identifier` for a per-deck boolean property in the deck ValueTree subtree, defaulting to `false`.
- [ ] `IDs::isSynced` is defined as a `juce::Identifier` for a per-deck boolean property in the deck ValueTree subtree, defaulting to `false`.
- [ ] `IDs::masterDeckIndex` is defined as a `juce::Identifier` for an integer property in the root ValueTree, defaulting to `-1`.
- [ ] `MasterClockManager` is a `juce::ValueTree::Listener` that listens to the root state ValueTree; all of its callback methods execute exclusively on the JUCE message thread.
- [ ] At most one deck has `IDs::isMaster = true` at any time; `MasterClockManager` enforces this invariant on every master-election transition.
- [ ] When `IDs::isMaster` is set to `true` on a deck, `MasterClockManager` simultaneously sets `IDs::isMaster = false` on every other deck within the same synchronous operation on the message thread.
- [ ] When the master deck's `playbackStatus` transitions to Stopped, `MasterClockManager` promotes the lowest-index currently-playing deck within one message loop cycle, or enters dormant state if no deck is playing.
- [ ] When the master deck's `playbackStatus` transitions to Empty (track ejected), `MasterClockManager` applies the same promotion logic as a Stopped transition.
- [ ] When the master deck's `playbackStatus` transitions to Paused, `MasterClockManager` publishes a new snapshot with `masterIsPlaying = false` and unchanged `masterBPM` and `masterPhaseOriginSample`.
- [ ] Dormant state is represented as `IDs::masterDeckIndex = -1` and `masterIsPlaying = false` in the published snapshot. `masterBPM` retains the last non-zero BPM value from the previous master.
- [ ] When the user presses MASTER on Deck N and Deck N is not the current master, `MasterClockManager` demotes the current master deck and promotes Deck N in the same message-thread operation, then publishes a new snapshot using Deck N's BPM and a phase origin derived from Deck N's current beatgrid anchor.
- [ ] Pressing MASTER on the already-master deck while it is in Playing state produces no state mutation and no new snapshot publication.
- [ ] The first deck to transition from Empty to Stopped while `IDs::masterDeckIndex = -1` is automatically promoted as master by `MasterClockManager`.
- [ ] A deck that transitions to Playing while `IDs::masterDeckIndex = -1` (dormant) is automatically promoted as master by `MasterClockManager`.
- [ ] `MasterClockManager` receives a `MasterClockPublisher&` reference via constructor injection; it does not instantiate or own the publisher.
- [ ] No ValueTree property is read or written from the audio thread by any code introduced in this PRD.
- [ ] `MasterClockPublisher` is accessible to `DeckAudioSource` instances via constructor injection; no singleton or global state is used.
- [ ] All new source files are located under `Source/Features/Sync/` as specified in EPIC-0003's file structure.

## 1.5. Grey Areas

1. **BPM value in dormant state.** When the clock enters dormant state (no deck playing), `masterBPM` in the published snapshot has no live source. Setting it to `0.0` would signal "no tempo" but could cause a division-by-zero in sync consumers that compute `masterBPM / deckBPM` ratios. Displaying `0.0` in the Master BPM UI readout is also confusing for the DJ. Resolution: `masterBPM` retains the last non-zero published value when transitioning to dormant state. Sync consumers must check `masterIsPlaying` before applying speed corrections; a `masterIsPlaying = false` snapshot means "hold current tempo, cease phase correction," not "set speed to zero."

2. **Phase origin value on manual master reassignment.** When the user presses MASTER on Deck N, a new `masterPhaseOriginSample` must be published for Deck N. The phase origin should be the sample index of a known beat anchor from Deck N's beatgrid, not the current playhead position, so that phase calculations by slave decks remain beatgrid-relative. However, Deck N's beatgrid analysis (PRD-0008) may not yet be complete when the user presses MASTER. Resolution: `MasterClockManager` reads the beatgrid anchor sample from Deck N's state if available. If the beatgrid is not yet computed (BPM = 0), the master promotion is deferred and `MasterClockManager` re-evaluates when the deck's BPM value is written to the ValueTree. An in-progress state indicator is shown in the UI (implemented in the Master BPM display PRD, not this PRD).

3. **Pressing MASTER on a stopped (not playing) deck.** The current master is playing. The user presses MASTER on a stopped deck with a loaded track. The stopped deck becomes master, which means `masterIsPlaying = false` is immediately published. All slave decks suspend phase correction. This may be intentional (the DJ wants to pre-cue the next master without disrupting the current sync), or accidental (mis-press). Resolution: allow the operation — the manual MASTER button is an explicit user action and professional DJ software (Traktor, Serato) permit master assignment on non-playing decks. The resulting dormant-like snapshot is correct: slaves hold tempo, phase correction suspends, and sync resumes the moment the new master starts playing.

4. **Lowest-index promotion when deck indices are non-contiguous.** Sonik supports 1 to 4 decks. Decks are assigned permanent letter identities (A, B, C, D) but their internal indices can be non-contiguous if a middle deck is removed (e.g., Deck B removed while Decks A and C remain, leaving indices 0 and 2). The "lowest-index currently-playing deck" rule must operate on the surviving deck indices in their original order. Resolution: `MasterClockManager` iterates over the root ValueTree's child decks in their stored order (which mirrors insertion order and matches Deck A < B < C < D priority) and selects the first child whose `playbackStatus = Playing`. This is unambiguous regardless of gaps in the index sequence.

5. **Coherence window during master transition.** When `MasterClockManager` demotes the old master and promotes a new one, there is a brief interval (one message loop cycle, approximately 16 ms) during which the SeqLock buffer still holds the old master's snapshot. Slave decks on the audio thread that run during this window apply the old BPM and phase origin for one or two audio blocks before the new snapshot becomes visible. Resolution: this is acceptable and by design. The SeqLock guarantees that slave decks always read a fully coherent snapshot — either the old one or the new one, never a partial mix. A 16 ms window of stale-but-coherent data produces at most one buffer-cycle of incorrect phase correction, which is inaudible. No additional synchronization is needed.