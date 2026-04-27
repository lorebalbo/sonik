---
status: Implemented
epic: EPIC-0003
depends-on:
  - PRD-0001
  - PRD-0002
  - PRD-0004
  - PRD-0008
  - PRD-0010
  - PRD-0026
---

# 1. PRD-0027: Tempo Sync Engine

## 1.1. Problem

When a DJ blends two tracks simultaneously, keeping the beatgrids locked together requires continuous manual attention: the DJ must listen for phase drift, calculate the required correction, and apply it via jog wheel nudges or pitch fader adjustments while simultaneously managing EQ, gain, and crowd interaction. Even a lapse of two or three seconds allows the beatgrids to drift enough to produce an audible flamming effect that signals a loss of control to attentive listeners.

This manual correction burden is especially heavy during extended blends (more than eight bars), during multi-deck mixes where three or four decks are playing simultaneously, or when mixing tracks from adjacent sub-genres with BPMs that differ by only 0.5–2 BPM. In all of these scenarios the DJ is spending most of their cognitive budget on mechanical tempo maintenance rather than on musical or expressive decisions.

Sonik already computes a precise BPM for each loaded track (PRD-0008), maintains a per-deck transport with a stable playhead (PRD-0004), and publishes a coherent master clock snapshot that any deck can read in real time without locks or allocations (PRD-0026). All of the raw inputs for automated tempo synchronisation are present in the system. Without a Tempo Sync Engine to connect these inputs into a feedback loop that drives each slave deck's `speedMultiplier`, the BPM analysis and master clock infrastructure are operationally inert and provide no relief to the DJ.

## 1.2. Objective

The system provides an automated Tempo Sync Engine such that:
- When the SYNC latch is engaged on a deck, that deck's `speedMultiplier` is continuously and automatically set to the value required for it to play at `masterBPM`, derived from the master's published `MasterClockSnapshot` and the deck's detected `deckBPM`.
- The BPM ratio is normalised into the window `[0.667×, 1.5×]` before application, preventing half-time or double-time speed mismatches when master and slave BPMs are in a 2:1 or 3:2 relationship.
- The SYNC button is a persistent latch per deck: once engaged it remains active until the user explicitly disengages it, surviving play/pause transitions and track changes on other decks.
- Each deck has a MASTER button that designates it as the exclusive tempo reference; the MASTER button is backed by `MasterClockManager` (PRD-0026), which enforces mutual exclusion and handles automatic promotion on eject or stop.
- SYNC overrides the pitch fader's contribution to `speedMultiplier` completely while engaged. The pitch fader position is stored in `IDs::pitchFaderPercent` but has no effect on playback speed until SYNC is disengaged.
- All ValueTree writes (SYNC toggle, MASTER toggle) occur exclusively on the JUCE message thread. The audio thread reads only lock-free atomics and the SeqLock snapshot.

## 1.3. User Flow

1. Deck A is playing. It was automatically promoted to master by `MasterClockManager` on track load (PRD-0026). Deck A's MASTER button is lit. Its detected BPM is 128.0. The published `MasterClockSnapshot` holds `masterBPM = 128.0` and `masterIsPlaying = true`.
2. The user loads a new track onto Deck B. The track's detected BPM is 130.2. Deck B is stopped. The SYNC button on Deck B is unlit (`IDs::isSynced = false`). Deck B's pitch fader is at 0%.
3. The user presses Play on Deck B. Deck B begins playing at its natural 130.2 BPM. The beatgrids of Deck A and Deck B immediately begin to drift apart.
4. The user presses the SYNC button on Deck B. `IDs::isSynced` is set to `true` on Deck B's ValueTree from the message thread. The SYNC button lights up. On the next audio block, `SyncEngine` reads the `MasterClockSnapshot` (`masterBPM = 128.0`) and Deck B's `deckBPM` (130.2). It computes `ratio = 128.0 / 130.2 ≈ 0.9831`. The ratio lies within `[0.667, 1.5]` so no normalization fold fires. `DeckAudioSource` applies `speedMultiplier = 0.9831`. Deck B now plays at 128.0 BPM, matching Deck A.
5. The user moves Deck B's pitch fader to +5%. Because SYNC is engaged, the pitch fader change is stored in `IDs::pitchFaderPercent` but does not affect `speedMultiplier`. Deck B continues playing at 128.0 BPM. The pitch fader position indicator moves visually but the displayed BPM on Deck B remains 128.0.
6. The user presses the SYNC button on Deck B again to disengage. `IDs::isSynced` is set to `false` on the message thread. On the next audio block, `SyncEngine` detects `isSynced = false` and the audio engine sets `speedMultiplier` to the pitch fader's contribution: `pitchFaderMultiplier = 1.05`. Deck B now plays at 130.2 × 1.05 ≈ 136.7 BPM. This tempo jump is immediate and intentional — the DJ moved the pitch fader while SYNC was active and must account for the resulting offset on disengage.
7. The user presses the MASTER button on Deck B. `MasterButton` calls `MasterClockManager::setMaster(deckB)` on the message thread. `MasterClockManager` demotes Deck A (`IDs::isMaster = false`) and promotes Deck B (`IDs::isMaster = true`). A new `MasterClockSnapshot` is published with Deck B's current effective BPM (≈136.7). Deck A's MASTER button goes dark; Deck B's lights up. If Deck A has SYNC engaged, it reads the new snapshot on its next audio block and re-computes its `speedMultiplier` relative to 136.7 BPM.
8. The user presses the MASTER button on Deck B a second time. Because a deck cannot deactivate its own master status — doing so would leave the clock without a reference — the button press has no effect. Master ownership is only transferred by pressing MASTER on a different deck.

### 1.3.1. Edge Cases

- If the SYNC button is pressed on a stopped (non-playing) deck, `IDs::isSynced = true` is stored immediately. When the deck is subsequently started, the first audio block applies the sync computation and `speedMultiplier` is set before any audio is produced, so the deck begins playback already in sync.
- If the master deck is paused (`masterIsPlaying = false` in the snapshot), `SyncEngine` suspends tempo adjustment on slave decks: `speedMultiplier` holds its last computed value. Tempo sync resumes on the first audio block where `masterIsPlaying` returns to `true`.
- If the master deck is stopped and ejected, `MasterClockManager` promotes the next playing deck automatically (PRD-0026). Slave `SyncEngine` instances read the updated snapshot and re-compute `speedMultiplier` against the new master BPM within one audio block.
- If the slave deck's `deckBPM` is 0.0 (beatgrid analysis not yet complete), `SyncEngine` skips the computation and leaves `speedMultiplier` unchanged. When analysis completes and `IDs::deckBPM` is written to ValueTree, `AudioStateSync` propagates the value and the next audio block applies the sync correctly.
- If the `MasterClockSnapshot` reports `masterBPM = 0.0` (clock in dormant state), `SyncEngine` skips the computation and leaves `speedMultiplier` unchanged. No floating-point division by zero or undefined behaviour occurs.
- If SYNC is activated while no master exists (dormant state), `IDs::isSynced = true` is stored but produces no speed change until a valid `masterBPM > 0.0` appears in the snapshot.
- A deck with SYNC engaged cannot simultaneously be master. Pressing MASTER on a synced deck disengages SYNC first (`IDs::isSynced = false`), then promotes the deck to master within a single `MasterClockManager` call.

## 1.4. Acceptance Criteria

- [ ] `SyncEngine` is implemented in `Source/Features/Sync/SyncEngine.h` and `Source/Features/Sync/SyncEngine.cpp`. It receives a `MasterClockPublisher` reference via constructor injection and exposes a `process(DeckAudioState& state)` method called from `DeckAudioSource::processBlock`.
- [ ] Inside `SyncEngine::process`, if `state.isSynced` is `false`, the method returns immediately without reading the snapshot or modifying `speedMultiplier`.
- [ ] Inside `SyncEngine::process`, if `state.isSynced` is `true` and `snapshot.masterBPM > 0.0` and `state.deckBPM > 0.0` and `snapshot.masterIsPlaying` is `true`, the engine computes `ratio = snapshot.masterBPM / state.deckBPM`, applies the 2:1 normalization fold, and writes the result to `state.speedMultiplier` via `std::atomic<double>::store` with `std::memory_order_relaxed`.
- [ ] The 2:1 normalization fold is implemented exactly as `while (ratio > 1.5) ratio *= 0.5; while (ratio < 0.667) ratio *= 2.0;` and is applied before writing `speedMultiplier`.
- [ ] `SyncEngine::process` reads the `MasterClockSnapshot` exclusively via `MasterClockPublisher::read()` (the SeqLock path). It never acquires a mutex, allocates memory, reads or writes files, or calls any JUCE API that is not documented as audio-thread safe.
- [ ] `AudioStateSync` propagates `IDs::isSynced` from the deck's ValueTree to `DeckAudioState::isSynced` as a `std::atomic<bool>`, written on the message thread via a `juce::ValueTree::Listener` callback.
- [ ] `AudioStateSync` propagates `IDs::deckBPM` from the deck's ValueTree to `DeckAudioState::deckBPM` as a `std::atomic<double>`, written on the message thread.
- [ ] `AudioStateSync` maintains a `pitchFaderMultiplier` atomic (derived from `IDs::pitchFaderPercent`) that is kept current on the message thread at all times, regardless of SYNC state.
- [ ] When `state.isSynced` is `true`, `DeckAudioSource::processBlock` uses the `speedMultiplier` value written by `SyncEngine` and does NOT apply the pitch fader's contribution on top of it.
- [ ] When `state.isSynced` transitions from `true` to `false` (detected on the audio thread by comparing a `prevIsSynced` local variable to the current `isSynced` atomic), `DeckAudioSource::processBlock` immediately sets `speedMultiplier` to `pitchFaderMultiplier`. No ramp or interpolation is applied; the change is instantaneous on the block boundary.
- [ ] `SyncButton` is implemented in `Source/Features/Sync/UI/SyncButton.h` and `Source/Features/Sync/UI/SyncButton.cpp`. It is a `juce::Button` subclass that observes `IDs::isSynced` on the deck's ValueTree and repaints on every change.
- [ ] `SyncButton` renders as lit (background fill `#f9f9f9`, label colour `#000000`) when `IDs::isSynced = true` and unlit (background fill `#000000`, label colour `#f9f9f9`) when `false`. Zero border-radius. No gradient or shadow.
- [ ] `SyncButton::clicked()` writes `IDs::isSynced = !currentValue` to the deck's ValueTree exclusively on the JUCE message thread.
- [ ] `MasterButton` is implemented in `Source/Features/Sync/UI/MasterButton.h` and `Source/Features/Sync/UI/MasterButton.cpp`. It is a `juce::Button` subclass that observes `IDs::isMaster` on the deck's ValueTree and repaints on every change.
- [ ] `MasterButton` renders as lit when `IDs::isMaster = true` and unlit when `false`, using the same monochrome fill convention as `SyncButton`.
- [ ] `MasterButton::clicked()` calls `MasterClockManager::setMaster(deckIndex)` on the message thread. The button does NOT write directly to `IDs::isMaster`; mutual exclusion is enforced by `MasterClockManager` (PRD-0026).
- [ ] When `MasterButton::clicked()` is called on a deck that already has `IDs::isSynced = true`, `MasterClockManager::setMaster` writes `IDs::isSynced = false` on that deck's ValueTree before promoting it to master. The intermediate state where the deck is neither synced nor master exists for zero audio blocks.
- [ ] Pressing the active MASTER button (the deck that is currently master) has no effect. The button does not toggle off.
- [ ] When `snapshot.masterIsPlaying = false`, `SyncEngine::process` returns without modifying `speedMultiplier`.
- [ ] When `state.deckBPM = 0.0` or `snapshot.masterBPM = 0.0`, `SyncEngine::process` returns without computing a ratio. No floating-point exception or undefined behaviour occurs in any of these guard conditions.
- [ ] All source files introduced by this PRD are confined to `Source/Features/Sync/` as defined below:

```text
Source/Features/Sync/
├- SyncEngine.h
├- SyncEngine.cpp
└- UI/
   ├- MasterButton.h
   ├- MasterButton.cpp
   ├- SyncButton.h
   └- SyncButton.cpp
```

- [ ] No new global variables, static mutable state, or singletons are introduced. `SyncEngine` receives `MasterClockPublisher` and `DeckAudioState` as constructor or call-site dependencies.

## 1.5. Grey Areas

### 1.5.1. SYNC Interaction with the Pitch Fader

While SYNC is active the pitch fader and `SyncEngine` both assert control over `speedMultiplier`. Two interpretations exist:
- **Multiplicative model:** `speedMultiplier = (masterBPM / deckBPM) × pitchFaderMultiplier`. The pitch fader acts as an offset on top of the sync target. At a +5% fader the deck plays 5% faster than master BPM, not at master BPM. This violates the core contract of tempo sync and produces continuous drift rather than lock.
- **Override model:** `speedMultiplier = masterBPM / deckBPM` (normalised). The pitch fader position is stored but ignored for speed computation while SYNC is engaged.

**Resolution:** The Override model is adopted. SYNC fully overrides the pitch fader while engaged. This is the only model that guarantees the deck remains at master BPM and is consistent with Traktor Pro 3's behaviour. Moving the pitch fader while SYNC is active is treated as preparing the speed that will be active after SYNC is disengaged — a common DJ workflow where the DJ parks the fader at the intended post-blend BPM before dropping SYNC.

### 1.5.2. Speed on SYNC Disengage

When the user disengages SYNC mid-mix, two approaches are possible:
- **Snap to fader:** `speedMultiplier` immediately becomes `pitchFaderMultiplier`. If the pitch fader was moved while SYNC was active this can produce an abrupt BPM jump, but the position is always visible and under the DJ's control.
- **Hold current:** `speedMultiplier` remains at the last sync-computed value. This avoids a jump but severs the link between fader position and actual speed, creating a hidden offset that is difficult to recover from without visual feedback.

**Resolution:** Snap to fader is adopted. The DJ is responsible for the pitch fader position at all times; moving it while SYNC is active is a deliberate preparation for the post-sync speed. An immediate snap is the most predictable and recoverable outcome. The transition is instantaneous on the audio block boundary, with no ramp.

### 1.5.3. SYNC on a Deck with No Detected BPM

If beatgrid analysis has not completed when the user engages SYNC, `deckBPM = 0.0` and no valid ratio can be computed. Two options exist:
- **Block engagement:** The SYNC button is disabled (dimmed, non-interactive) while `deckBPM = 0.0`.
- **Allow engagement, defer execution:** `IDs::isSynced = true` is stored, but the audio thread guards against `deckBPM = 0.0` and skips computation until a valid value arrives.

**Resolution:** Allow engagement, defer execution. Gating the SYNC button on analysis state introduces a timing dependency between the analysis pipeline and UI interactivity that creates subtle race conditions (e.g., BPM arriving between a button press and the next repaint). It is cleaner for `SyncEngine::process` to guard against `deckBPM = 0.0` explicitly. When analysis completes, `AudioStateSync` propagates the new BPM atomic and the next audio block applies the correct multiplier with no additional UI logic required.

### 1.5.4. SYNC Activated on a Stopped Deck

If SYNC is engaged on a deck that is not currently playing, two options exist:
- **Reject the engagement:** SYNC cannot be activated unless the deck is playing, preventing a confusing latent state.
- **Accept and latch:** `IDs::isSynced = true` is stored immediately. When the deck starts playing, the first audio block applies the sync computation.

**Resolution:** Accept and latch. A DJ routinely sets up a deck — including enabling SYNC — before pressing Play so the deck enters the mix already locked to master BPM. Preventing this workflow would force an awkward sequence of Press Play → Enable SYNC with an audible unsynchronised moment in between. The `masterIsPlaying` guard in `SyncEngine::process` naturally handles the case where SYNC is active but neither deck is playing.

### 1.5.5. MASTER Pressed on a Synced Deck

A deck cannot simultaneously be master and slave. If a deck with `isSynced = true` is promoted to master, it would read its own published BPM as the master BPM, compute a ratio of 1.0, and apply it — harmless numerically but conceptually incoherent, and it silently removes the deck from the sync network without feedback. Two options exist:
- **Prevent the action:** The MASTER button is disabled while SYNC is active on that deck.
- **Auto-disengage SYNC then promote:** `IDs::isSynced` is set to `false` and `IDs::isMaster` is set to `true` within a single `MasterClockManager` call before a new snapshot is published.

**Resolution:** Auto-disengage SYNC then promote. Blocking the MASTER button while SYNC is active is unnecessarily restrictive — the DJ may deliberately want to promote a deck mid-sync without a two-step button sequence. The auto-disengage is atomic from the message thread's perspective: both ValueTree writes happen before `MasterClockPublisher::write()` is called, so no audio block ever observes an intermediate state where the deck is neither synced nor master.