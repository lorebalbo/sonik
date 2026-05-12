---
status: Not Implemented
epic: EPIC-0005
depends-on: [PRD-0042, PRD-0044]
---

# 1. PRD-0045: Soft-Takeover (Pickup Mode) for Continuous Controls

## 1.1. Problem

After PRD-0044, every continuous hardware control on the Reloop Contour Interface Edition — pitch fader, gain knob, EQ knobs, crossfader — writes its physical position directly to the corresponding software value via the existing ValueTree setters. This works fine when the hardware and software are in agreement, but it produces violent, audible jumps in every other scenario:

- **Load track scenario.** Deck A is playing at +6% pitch. The user loads a new track, which resets `pitch` to 0%. The hardware fader is still physically at +6%. The next time the user *touches* the fader to nudge by 0.1%, the value jumps from 0% back to +6% — an instant 6% BPM jump audible to the audience as a tempo glitch.
- **Profile change scenario.** The user switches from a Reloop profile to a custom profile mid-set. The new profile maps the same fader to gain instead of pitch. The hardware position no longer corresponds to the software value; the first move of the fader produces an instant gain jump.
- **Multi-deck mixing scenario.** With two decks A and B both mapped to the same physical pitch fader (via different profiles or different focus models), the user mixes from A to B. Deck B's software pitch is wherever it was last; the hardware is at whatever position it last touched. Without soft-takeover, every switch produces an audible jump.

Every professional DJ software (Traktor, Serato, Rekordbox, Mixxx) solves this with **soft-takeover (pickup mode)**: hardware moves are suppressed until the hardware *crosses* the current software value, at which point the binding "engages" and subsequent moves pass through. The fader has to physically pass through the software's current position before it takes control. This is the **only acceptable** behaviour for live mixing — without it, the application is unusable for any DJ who loads tracks during a set.

PRD-0044 deliberately leaves this gap unfilled. This PRD fills it.

## 1.2. Objective

The system must implement soft-takeover (pickup mode) for every continuous binding declared with `softTakeover: pickup`, suppressing hardware writes until the hardware value crosses the current software value. Specifically:

- The system ensures that for every continuous binding (`TargetValueKind::Continuous`) with `softTakeover: pickup`, the system tracks a per-binding `TakeoverState` of `Disengaged` or `Engaged`.
- The system ensures that while `Disengaged`, hardware writes are suppressed (the ValueTree / feature-state setter is not called); the only visible side effect of the hardware move is internal tracking of the last hardware value.
- The system ensures that on each new hardware value, the system computes the signed difference between hardware and current software values and detects a sign flip (crossing) compared to the previous sample. On crossing, the state advances to `Engaged` and the *current* hardware value is written through.
- The system ensures that once `Engaged`, all subsequent writes pass through directly until the state is explicitly reset.
- The system ensures that the state is reset to `Disengaged` on (a) profile activation/change for the device, (b) a track load on the affected deck that resets the relevant software value (e.g., load resets pitch to 0), (c) an `engage` request from PRD-0048's UI ("force engage" button).
- The system ensures that bindings declared with `softTakeover: always` or `softTakeover: never` skip the state machine entirely and behave as in PRD-0044 (pass-through). These two policy values are functionally identical and exist for forward-compatibility / explicitness in mapping files.
- The system ensures that toggle, momentary, and relative-delta bindings ignore soft-takeover entirely (the policy is meaningless for them) and pass through unconditionally, regardless of the declared `softTakeover` value.
- The system ensures that the soft-takeover state machine runs on the Message thread (continuous bindings always route through PRD-0041's Message-thread path), with no audio-thread interaction.
- The system ensures that the soft-takeover state is observable from PRD-0048's UI so the user can see which continuous controls are currently `Disengaged` and waiting for pickup.

## 1.3. User Flow

This PRD has no UI of its own; its observable behaviour is that hardware moves are "soaked up" until pickup occurs. PRD-0048 will surface the state.

### 1.3.1. Track Load Resets a Continuous Binding

1. Deck A is playing. The user has the pitch fader at +6% on the Reloop Contour CE; the software pitch is also +6%. The binding state is `Engaged`.
2. The user loads a new track to Deck A. The existing track-load flow resets `pitch` to 0% via a ValueTree write.
3. The `DeckMidiHandler` (or a dedicated `SoftTakeoverManager` listening to the deck's pitch ValueTree property) detects that the software value has changed *without* the change originating from the hardware. It resets the soft-takeover state for the `(deviceId, target=deck.A.pitchFader)` binding to `Disengaged`.
4. The user moves the hardware fader by 0.1%. The router resolves the binding and forwards it to `DeckMidiHandler`. The handler consults the `SoftTakeoverManager` for the binding's current state.
5. State is `Disengaged`. The manager records the new hardware sample, compares to software (sw=0%, hw=6.1%, sign positive). No crossing detected. The write is **suppressed**.
6. The user moves the hardware fader down through 0%. The manager records hw=-0.05%, sw=0%, sign now negative — a crossing has occurred since the previous sample. The manager transitions to `Engaged` and writes hw=-0.05% to the ValueTree. The deck pitch updates audibly for the first time since the load.
7. All subsequent moves of the fader pass through without further suppression.

### 1.3.2. Profile Change Resets All Continuous Bindings

1. The user has been performing with the Reloop profile. The pitch fader is `Engaged`.
2. The user switches via PRD-0048's UI to a different profile that maps the same physical fader to a different target.
3. `MappingStore` fires `activeMappingChanged(deviceId)`. The `SoftTakeoverManager` listens and resets the state for every `pickup` binding on that device to `Disengaged`.
4. The user moves the hardware fader. The new binding's target value is some software value (probably not 0). The fader must cross that value before the new binding engages.

### 1.3.3. Force-Engage From the UI

1. The user wants to "jam" the hardware fader's current position to the software without waiting for a crossing (e.g., for fine adjustments at the extremes of the fader).
2. PRD-0048's UI exposes a "Force Engage" button per continuous binding.
3. The button calls `SoftTakeoverManager::forceEngage(deviceId, target)` on the Message thread.
4. The manager records the current hardware sample, transitions to `Engaged`, and writes hw to the software immediately. Subsequent moves pass through.

### 1.3.4. `softTakeover: always` or `never` Skip the Machine

1. A binding is declared `softTakeover: always` (or `never`).
2. The manager sees the policy on first dispatch and immediately marks the binding as `Engaged` without recording hardware history.
3. Every hardware move passes through immediately. The user gets jumps; this is what they asked for. Useful for setup-time controls (master gain, channel trim) where instant alignment is preferable to pickup.

### 1.3.5. Initial Connection of a Device

1. The Reloop Contour CE is connected. The Reloop profile is resolved and activated.
2. The `SoftTakeoverManager` initialises the state for every continuous `pickup` binding to `Disengaged`.
3. Hardware moves are suppressed on every continuous control until each one is crossed once. This is correct: the application has no idea where the user's hardware is physically positioned at startup, so until the user moves a fader through the software value, the binding remains disengaged.

## 1.4. Acceptance Criteria

- [ ] The system defines `SoftTakeoverManager` under `Source/Features/Midi/` on the Message thread.
- [ ] The system stores soft-takeover state in a `std::unordered_map<std::pair<uint64_t /*deviceId*/, TargetIndex>, TakeoverEntry, …>` (or equivalent flat structure) guarded by Message-thread invariant (no cross-thread access).
- [ ] The system defines `TakeoverEntry` as a struct holding: `TakeoverState state` (enum class with values `Disengaged`, `Engaged`), `float lastHardwareValue` (range `[0, 1]`, sentinel `-1` for "no sample yet"), `float lastSoftwareValueAtCheck` (used for sign-flip detection).
- [ ] The system defines `TakeoverState` as an enum class with values `Disengaged` and `Engaged`.
- [ ] The system exposes `SoftTakeoverManager::shouldPassThrough(uint64_t deviceId, TargetIndex target, float hardwareValue, float currentSoftwareValue, SoftTakeoverPolicy policy) -> bool` callable on the Message thread.
- [ ] The system's `shouldPassThrough`, for `policy == Always` or `policy == Never`, returns `true` and marks the entry `Engaged` without recording history.
- [ ] The system's `shouldPassThrough`, for `policy == Pickup`, on the first call after a reset, records `lastHardwareValue = hardwareValue` and returns `false` (no crossing yet possible).
- [ ] The system's `shouldPassThrough`, for `policy == Pickup` with state `Disengaged`, detects a crossing when `sign(hardwareValue - currentSoftwareValue) != sign(lastHardwareValue - currentSoftwareValue)` (using a small epsilon for the equality case). On detection: transitions state to `Engaged`, updates `lastHardwareValue`, returns `true`.
- [ ] The system's `shouldPassThrough`, for `policy == Pickup` with state `Disengaged` and no crossing, updates `lastHardwareValue` and returns `false`.
- [ ] The system's `shouldPassThrough`, for state `Engaged`, returns `true` unconditionally and updates `lastHardwareValue`.
- [ ] The system exposes `SoftTakeoverManager::resetForDevice(uint64_t deviceId)` callable on the Message thread, which sets every entry for the device to `Disengaged` and clears the `lastHardwareValue` sentinel.
- [ ] The system exposes `SoftTakeoverManager::resetForBinding(uint64_t deviceId, TargetIndex target)` callable on the Message thread, which resets a single entry.
- [ ] The system exposes `SoftTakeoverManager::forceEngage(uint64_t deviceId, TargetIndex target, float currentHardwareValue, float currentSoftwareValue)` callable on the Message thread, which transitions the entry to `Engaged` and records the current hardware value.
- [ ] The system exposes `SoftTakeoverManager::getState(uint64_t deviceId, TargetIndex target) const -> TakeoverState` for PRD-0048's UI to render disengaged-indicator badges.
- [ ] The system registers `SoftTakeoverManager` as a `MappingStoreListener` and resets every continuous-pickup entry for a device on `activeMappingChanged(deviceId)`.
- [ ] The system registers `SoftTakeoverManager` as a listener on the root `juce::ValueTree` for property changes to continuous-targeted properties (e.g., `IDs::pitch`, `IDs::gain`, `IDs::keyShift`). When a property changes from a source other than this manager itself, the manager calls `resetForBinding` for every device + target mapped to that property.
- [ ] The system extends the `DeckMidiHandler` (and other continuous-target-handling code introduced in PRD-0044) to consult `SoftTakeoverManager::shouldPassThrough` before writing the software value; on `false`, the write is suppressed entirely.
- [ ] The system tags writes performed by `DeckMidiHandler` itself with a small "midi-originated" marker (e.g., a thread-local flag or a ValueTree property metadata) so the manager's own ValueTree listener can distinguish MIDI-originated changes (which must NOT trigger reset) from non-MIDI changes (track load, mouse drag, UI input — which MUST trigger reset).
- [ ] The system's MIDI-originated marker is set on the Message thread immediately before the ValueTree write and cleared immediately after, in a RAII guard.
- [ ] The system passes through every non-continuous binding (`TargetValueKind::Momentary | Toggle | RelativeDelta`) unconditionally, regardless of the declared `softTakeover` value.
- [ ] The system's `shouldPassThrough` performs zero heap allocations on the hot path (lookup in flat hash map, in-place state mutation only).
- [ ] The system asserts via `JUCE_ASSERT_MESSAGE_THREAD` on every public method in Debug builds.
- [ ] The system lives entirely under `Source/Features/Midi/` and depends only on `Source/Features/Midi/` headers (`ControlTargetRegistry.h`, `Mapping.h`) and `juce::ValueTree` for the property-change listener.
- [ ] The system is covered by `SoftTakeoverManagerTests.cpp` in `Tests/` verifying: (a) first hardware sample under `Pickup` returns false; (b) subsequent samples without crossing return false; (c) crossing produces transition to `Engaged` and pass-through; (d) `Always` and `Never` policies skip the machine; (e) `resetForBinding` puts the entry back to `Disengaged`; (f) `forceEngage` transitions immediately; (g) non-MIDI ValueTree writes correctly trigger `resetForBinding`, while MIDI-originated writes (marked via the RAII guard) do not.
