---
name: "EPIC-0003: Master Clock & Sync System"
status: Open
---

# 1. EPIC-0003: Master Clock & Sync System

## 1.1. Goal and Vision

Introduce a global synchronization system across all active decks (1 to 4) that enables automatic, persistent tempo and phase matching between a designated Master deck and one or more Slave decks. The system must be transparent during a live performance: once Sync is engaged it stays latched, continuously correcting phase drift through micro-tempo nudges rather than hard seeks so that the beatgrids of all synced decks stay locked together without audible artifacts.

The target experience is parity with Traktor Pro 3's Sync engine: a DJ should be able to activate SYNC on a second deck mid-mix and hear the beatgrids lock together within one bar, with the phase meter confirming perfect alignment, without any click, stutter, or key shift.

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:
- MASTER toggle button per deck (mutual exclusion across decks, auto-master promotion on eject/stop)
- SYNC latch button per deck (stays engaged until manually deactivated)
- Tempo sync: slave deck continuously tracks master BPM via atomic speedMultiplier adjustment
- Phase lock: continuous micro-tempo correction (±0.5% speed nudge over a ramp window) to converge and hold beatgrid phase; no hard seek
- BPM 2:1 normalization: fold ratio into the [0.667×, 1.5×] range before applying (prevents half/double-speed mismatches)
- Phase meter UI component per deck: narrow horizontal bar displaying ±0.5-beat offset from the master phase
- Master BPM display: global readout in the mixer header area showing the current master clock BPM

Foundational systems (non-user-facing):
- `MasterClockSnapshot` struct: coherent atomic-unit publication of master BPM, phase origin, and playing state via a SeqLock double-buffer
- `MasterClockManager`: message-thread-only Auto-Master state machine (ValueTree listener), enforces mutual exclusion and promotes the next playing deck on eject/stop
- Per-deck `phaseOffset` atomic published from audio thread for UI consumption

### 1.2.2. Out of Scope

- Tap Tempo (deferred to a later Epic)
- Phrase Sync (32-beat phrase alignment — deferred)
- MIDI clock send/receive (separate Epic)
- Ableton Link integration (separate Epic)
- Mixer crossfader automation driven by sync state (separate Epic)

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Coherent Clock Publication — SeqLock Pattern

The master clock state comprises at minimum three fields that must be read as a coherent unit by every slave deck's `processBlock`: `masterBPM (double)`, `masterPhaseOriginSample (int64_t)`, and `masterIsPlaying (bool)`. Publishing these as independent `std::atomic` members creates a TOCTOU race: a slave could read a new BPM with an old phase origin, producing a wildly incorrect correction delta.

The solution is a **SeqLock** (sequence-lock) — a lock-free, writer-priority reader pattern standard in real-time audio:
- A `std::atomic<uint32_t> sequence` counter is incremented (odd = write in progress, even = stable) by the message thread before and after writing the snapshot into a plain `MasterClockSnapshot` struct stored in a `std::atomic`-fenced buffer.
- The audio thread spins reading `sequence` before and after its memcpy of the snapshot; if both reads return the same even value, the snapshot is consistent. The spin resolves in 1–2 iterations under normal conditions (the message thread holds the write lock for nanoseconds).
- This adds zero allocation and zero lock overhead to the audio thread.

### 1.3.2. Stretcher Latency Offset in Phase Calculation

The `DeckAudioSource` already tracks `stretcherLatency` (samples). The audible output of a slave deck lags its internal `playheadAccumulator` by exactly `stretcherLatency` samples when key lock is engaged. Phase delta must therefore be calculated as:

```
effectivePlayhead = playheadAccumulator - stretcherLatency
phaseOffsetBeats  = ((effectivePlayhead - masterPhaseOriginSample) % beatInterval) / beatInterval
```

Omitting this offset causes the corrected beat to arrive consistently late, making the sync sound slightly off even when the phase meter reads zero.

### 1.3.3. Micro-Tempo Correction — No Hard Seeks

Phase correction must never call `seekDeck`. A hard seek causes a click and destroys the stretcher's internal state. Instead, the slave deck applies a fractional speed adjustment:

- If `phaseOffsetBeats > convergenceThreshold` (e.g., 0.02 beats): apply `correctionMultiplier = 1.0 ± correctionRate` (e.g., ±0.005) on top of the tempo-sync speed.
- Ramp the correction over `correctionWindowBlocks` (e.g., 64 blocks ≈ 375 ms at 128-sample/44.1 kHz) to avoid an abrupt speed change.
- Once `|phaseOffsetBeats| < convergenceThreshold`, return `correctionMultiplier` to 1.0.
- This is a proportional controller (P-controller). No integral or derivative term is needed for single-beat phase lock.

### 1.3.4. Auto-Master State Machine

The `MasterClockManager` runs exclusively on the message thread as a `juce::ValueTree::Listener`. It enforces:
- Mutual exclusion: only one deck may have `isMaster = true` at any time.
- Auto-promotion on master loss: when the master deck's `playbackStatus` transitions to Stopped or when its track is ejected, `MasterClockManager` promotes the lowest-index currently-playing deck. If no deck is playing, the clock enters a dormant state holding the last known BPM.
- Clock freeze on master pause: phase origin is preserved; slaves remain tempo-matched but accumulate no phase drift while the master is paused.
- Manual override: user pressing MASTER on deck N immediately demotes the current master and promotes deck N.

The audio thread has no role in master election. It only reads the published `MasterClockSnapshot`.

### 1.3.5. BPM 2:1 Normalization

Before computing `targetSpeedMultiplier = masterBPM / deckBPM`, the ratio must be folded:

```cpp
double ratio = masterBPM / deckBPM;
while (ratio > 1.5)  ratio *= 0.5;
while (ratio < 0.667) ratio *= 2.0;
```

This prevents a 128 BPM master from driving a 64 BPM slave at 2× speed (double-time) or a 170 BPM track from being halved to match a 85 BPM master.

### 1.3.6. Audio Thread Safety

All rules from AGENTS.md apply without exception:
- `processBlock` reads only the SeqLock snapshot and per-deck atomics — no locks, no allocation, no I/O.
- `MasterClockManager` runs only on the message thread.
- Per-deck `phaseOffset` is published from the audio thread via `std::atomic<float>` for the phase meter UI.

### 1.3.7. File Structure

New code is organized under:
```text
Source/Features/Sync/
├- MasterClockSnapshot.h
├- MasterClockPublisher.h
├- MasterClockPublisher.cpp
├- MasterClockManager.h
├- MasterClockManager.cpp
├- SyncEngine.h
├- SyncEngine.cpp
└- UI/
   ├- MasterButton.h
   ├- MasterButton.cpp
   ├- SyncButton.h
   ├- SyncButton.cpp
   ├- PhaseMeterComponent.h
   └- PhaseMeterComponent.cpp
```

## 1.4. PRD Roadmap

- [ ] PRD-0026: Master Clock Core (MasterClockSnapshot, SeqLock publisher, Auto-Master state machine, ValueTree sync identifiers)
- [ ] PRD-0027: Tempo Sync Engine (slave processBlock reads master BPM, adjusts speedMultiplier, BPM normalization, MASTER/SYNC buttons)
- [ ] PRD-0028: Continuous Phase Lock (per-processBlock phase delta, micro-tempo correction ramp, stretcher latency offset compensation)
- [ ] PRD-0029: Sync UI — Phase Meter and Master BPM Display (PhaseMeterComponent, master BPM readout, phase offset atomic publication)