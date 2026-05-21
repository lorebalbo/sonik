---
status: Implemented
epic: EPIC-0007
depends-on:
  - PRD-0040
  - PRD-0042
  - PRD-0043
  - PRD-0044
  - PRD-0047
  - PRD-0052
  - PRD-0053
  - PRD-0054
  - PRD-0055
  - PRD-0056
  - PRD-0057
  - PRD-0058
  - PRD-0059
  - PRD-0060
---

# 1. PRD-0061: Mixer MIDI Wiring, Validation, and DDM4000 Profile Activation

## 1.1. Problem

The Mixer Epic stack is complete in its own right by the time this PRD is reached: state (PRD-0052), routing (PRD-0053), DSP (PRD-0054 through PRD-0058), UI atoms (PRD-0059), and the channel-strip / mixer-shell organism (PRD-0060) all work end to end from mouse input. But the Sonik Epic 0006 (MIDI subsystem) bundle includes a complete DDM4000 mapping (PRD-0043) whose binding targets reference paths like `mixer.channel.A.gain`, `mixer.channel.B.eq.high`, `mixer.crossfader`, `mixer.channel.C.assignA`, and the new `mixer.channel.{A,B,C,D}.filter` that PRD-0052 introduced. Until every one of those targets is verified to be reachable end-to-end (controller knob turn → atomic → DSP → audible change → LED feedback), the DDM4000 profile is provisionally bundled but practically untested against the new mixer.

In addition, the DDM4000 has per-channel cue buttons that are wired by the bundled profile to binding targets `mixer.channel.{A,B,C,D}.cue`, but cue / headphone routing is explicitly out of scope for EPIC-0007 (§1.2.2). The cue button presses must not (a) cause MIDI binding errors, (b) silently no-op without acknowledgement, or (c) corrupt the mixer state. The MIDI feedback engine (PRD-0047) must also drive the cue button LEDs correctly so that the user is not led to believe the button is broken — even though the audio behaviour is deferred.

Finally, every continuous mixer control benefits from soft-takeover behaviour when first touched mid-session (so a knob at physical 80% does not snap the software value from its current 0% to 80% instantly). PRD-0044's continuous-binding contract supports soft-takeover; this PRD ensures the per-channel filter (newly introduced) and the master gain are correctly configured to use it.

## 1.2. Objective

The system validates and activates the DDM4000 MIDI profile against the complete mixer such that:

- Every binding target listed in the bundled DDM4000 mapping that resolves to a `mixer.*` identifier is reachable end-to-end: a physical controller change produces the documented DSP behaviour, and a software state change produces the documented LED / motorised-control feedback (per PRD-0047).
- The newly introduced `mixer.channel.{A,B,C,D}.filter` target (registered by PRD-0052's amendment of PRD-0042) is bound in the DDM4000 mapping to the controller's per-channel filter knob, configured as a continuous target with soft-takeover enabled.
- The master gain knob (`mixer.master.gain`) is bound to the DDM4000's master knob with soft-takeover enabled.
- Per-channel cue buttons (`mixer.channel.{A,B,C,D}.cue`) are wired as no-op binding targets: the audio engine ignores them entirely, but they (a) register correctly in PRD-0042's binding target registry, (b) accept MIDI input without error, (c) drive the controller's cue LED via PRD-0047 to reflect a UI-only "armed / disarmed" boolean that has no audio effect.
- All `mixer.*` continuous targets in the registry have `softTakeover = true`; all `mixer.*` boolean / momentary targets have `softTakeover = false`.
- A startup-time validation pass checks every binding target referenced by the bundled DDM4000 profile against the registry, fails loudly (logs an error and refuses to activate the profile) if any unknown target is referenced, and warns if a registered target is not bound by the profile.
- An integration test suite covers the full MIDI → DSP → LED-feedback loop for at least one control of each binding category (continuous, momentary, toggle, LED-mirrored boolean) across the four mixer channels.

## 1.3. Developer / Integration Flow

1. PRD-0042's control target registry is amended (by this PRD) to add: `mixer.channel.{A,B,C,D}.cue` as `Momentary` (a no-op binding category, see §1.5.3) with `softTakeover = false`. The `mixer.channel.{A,B,C,D}.filter` target was already added by PRD-0052's amendment. The master-gain target `mixer.master.gain` is verified to exist with `softTakeover = true`; if not, its registry entry is amended in this PRD.
2. PRD-0043's bundled DDM4000 mapping file (`Resources/MidiMappings/ddm4000.midi.xml` or equivalent) is amended to bind: per-channel filter knobs to `mixer.channel.{A,B,C,D}.filter`, per-channel cue buttons to `mixer.channel.{A,B,C,D}.cue`, master knob to `mixer.master.gain`. Every existing `mixer.*` binding in the file is verified against the amended registry; any binding referencing an unknown target is removed or corrected.
3. A new validation step in the mapping-loader code path (called by PRD-0043's profile-load logic) iterates every binding in the loaded profile, looks up each binding target in PRD-0042's registry, and: emits an ERROR-level log message and refuses to activate the profile if any binding's target is `UnknownTarget`; emits a WARNING-level log message for each registered `mixer.*` target that no binding in the profile addresses (informational, does not block activation).
4. PRD-0047's MIDI feedback engine is configured to drive the cue-button LEDs from a `MixerCueState` UI-only boolean field per channel. This field is added to the ValueTree as `mixer.channel.{A,B,C,D}.cue` (boolean, default false). The audio engine ignores this field entirely; only the feedback engine and the UI read it.
5. A new test file under `Tests/` (`MixerMidiIntegrationTests.cpp` or similar) injects synthetic MIDI input through `MidiInboundRouter` (PRD-0044), verifies that the resolved binding writes the expected value to the expected ValueTree path, and (where the binding is a continuous control) verifies that soft-takeover behaviour engages correctly when the controller value differs from the current software value on first touch.
6. At application startup, after the audio engine and MIDI subsystem are both initialised, the DDM4000 profile is auto-activated if the matching controller is present (existing PRD-0040 behaviour) and the validation step from §3 is invoked. If the validation succeeds, the profile is bound; if it fails, the profile is left inactive and an error is logged.

## 1.4. Acceptance Criteria

- [ ] PRD-0042's control target registry contains entries for every binding target referenced by the bundled DDM4000 profile: `mixer.channel.{A,B,C,D}.{gain, eq.high, eq.mid, eq.low, eq.killHigh, eq.killMid, eq.killLow, filter, fader, assignA, assignB, cue}`, `mixer.crossfader`, `mixer.crossfader.curve`, and `mixer.master.gain`.
- [ ] All `mixer.channel.*.gain`, `mixer.channel.*.eq.{high,mid,low}`, `mixer.channel.*.filter`, `mixer.channel.*.fader`, `mixer.crossfader`, and `mixer.master.gain` registry entries have `kind = Continuous` and `softTakeover = true`.
- [ ] All `mixer.channel.*.eq.kill{High,Mid,Low}`, `mixer.channel.*.assignA`, `mixer.channel.*.assignB`, and `mixer.channel.*.cue` registry entries have `kind = Momentary` or `Toggle` (the implementation chooses the canonical kind per behaviour: kills and assigns are toggles, cue is momentary — see §1.5.2 and §1.5.3) with `softTakeover = false`.
- [ ] `mixer.crossfader.curve` has `kind = Enum` (a closed string set: `"sharp"`, `"smooth"`) with `softTakeover = false`.
- [ ] The bundled DDM4000 mapping file (`Resources/MidiMappings/ddm4000.midi.xml` or the bundled file path) contains exactly one binding per available DDM4000 physical control that corresponds to a mixer parameter; in particular, per-channel filter knobs are bound to `mixer.channel.{A,B,C,D}.filter`, per-channel cue buttons are bound to `mixer.channel.{A,B,C,D}.cue`, and the master knob is bound to `mixer.master.gain`.
- [ ] A validation pass runs at profile-load time, iterates every binding in the profile, and verifies its target exists in the registry; if any unknown target is found, the profile activation is aborted and an ERROR is logged with the offending binding's controller location and target identifier.
- [ ] The same validation pass emits a WARNING for any registered `mixer.*` target that no binding in the profile addresses (informational only).
- [ ] Cue buttons on the DDM4000 are bound to `mixer.channel.{A,B,C,D}.cue` (a boolean ValueTree property). Pressing a cue button toggles the boolean. The audio engine does not read this property; no audio behaviour changes when it toggles.
- [ ] PRD-0047's MIDI feedback engine drives the cue-button LED state from the `mixer.channel.{A,B,C,D}.cue` ValueTree boolean. Pressing the cue button toggles the LED visibly on the controller.
- [ ] Soft-takeover is verified end-to-end for at least one continuous control per category: a channel gain (`mixer.channel.A.gain`), an EQ band (`mixer.channel.B.eq.mid`), the filter (`mixer.channel.C.filter`), the crossfader (`mixer.crossfader`), the master gain (`mixer.master.gain`), and the channel fader (`mixer.channel.D.fader`). In each case, an integration test (see below) confirms that the software value does not jump on first MIDI touch when the controller's position differs from the current software value beyond the soft-takeover threshold defined by PRD-0044.
- [ ] At least one integration test under `Tests/MixerMidiIntegrationTests.cpp` injects synthetic MIDI through `MidiInboundRouter` (PRD-0044) for each of: a continuous knob (verifies smooth-takeover and atomic write), a toggle button (kill or assign; verifies boolean flip), a momentary button (cue; verifies boolean flip with no audio side effect), and the crossfader. The tests assert correct ValueTree state mutations and, where applicable, the correct corresponding `MixerAtomicSnapshot` value via the PRD-0052 bridge.
- [ ] The MIDI feedback round-trip is verified for at least one LED-mirrored boolean: writing `true` to `mixer.channel.A.eq.killHigh` produces a MIDI Note-On (or CC, per the profile) on the DDM4000's corresponding LED control output. This integration test uses a synthetic MIDI output capture.
- [ ] No new audio-thread code is added by this PRD; all audio-thread paths involved (the existing MixerAtomicSnapshot reads, the existing MixerStateBridge writes, the existing per-DSP-stage atomic loads) continue to perform no allocations, take no locks, and perform no I/O, and continue to communicate cross-thread exclusively via `std::atomic` or lock-free FIFOs.
- [ ] No new UI atom or organism is added by this PRD; the cue-button visual (if it eventually surfaces in the channel strip) is a future-Epic concern. This PRD only validates that the MIDI controller's cue button input round-trips correctly to the ValueTree and back to the controller's LED.
- [ ] No DSP block is added or modified by this PRD; the audio engine continues to ignore `mixer.channel.{A,B,C,D}.cue` exactly as it does today.

## 1.5. Grey Areas

### 1.5.1. Validation Failure Mode: Abort vs Skip-Bad-Binding

When the startup validation finds an unknown target in the bundled profile, the implementation can either (a) abort profile activation entirely (DJ has no MIDI control until the issue is fixed), or (b) skip the offending binding and activate the rest (DJ has partial MIDI control with silent failures).

**Resolution:** Abort. A bundled profile that references an unknown target is a build / packaging defect, not a recoverable runtime condition. Partial activation hides the bug, leaves the DJ confused about why one knob doesn't work, and creates a poor first-run experience. An aborted activation with a clear error log message is the correct failure mode: it surfaces the issue at the earliest possible moment (developer's local build) and forces a real fix rather than a silent degradation. Community-supplied profiles loaded by the user (a future Epic) may use a different policy; for the bundled profile, abort is the only acceptable behaviour.

### 1.5.2. Kill and Assign Buttons: Toggle vs Momentary

EQ kill buttons and A/B assign buttons could be implemented as toggles (press latches, press again unlatches) or momentaries (press latches while held, release unlatches). Hardware varies: Pioneer DJM kills are usually toggles; DDM4000 kills are toggles; some Allen & Heath kills support both via per-button preference. Assigns are universally toggles.

**Resolution:** Kills and assigns are both implemented as toggles. This matches the DDM4000 (the bundled profile target), matches every DJ's expectation of a kill switch ("press once = on; press again = off"), and is the simpler binding kind for PRD-0042's registry. A momentary-kill behaviour is a creative-mixing technique (hold to kick the band out, release to bring it back) that some DJs prefer; if a future Epic adds a per-channel "momentary kill" preference, it can do so by adding a separate `mixer.channel.*.eq.killHighMomentary` registry entry (or a per-binding-kind override on the existing entry) without breaking this PRD's contract.

### 1.5.3. Cue Buttons as No-Op vs Reserved

The cue buttons are bound but have no audio behaviour because cue / headphone routing is out of scope for EPIC-0007. Three approaches: (a) no-op binding (button press flips a UI-only boolean), (b) reserved binding (button press is ignored silently, target is `UnknownTarget` until cue Epic ships), (c) absent from the bundled profile (button is unbound, controller LED stays dark).

**Resolution:** Approach (a). The cue buttons are bound to a real `mixer.channel.{A,B,C,D}.cue` boolean target that the registry knows about and that PRD-0047 drives an LED from. The audio engine deliberately ignores the boolean. This is the only approach that (i) avoids surprising the DJ with a "broken" button whose press does nothing visible, (ii) doesn't require the bundled DDM4000 profile to omit a control that the controller physically has, and (iii) doesn't require future regeneration of the profile when the cue Epic ships. When the future cue Epic implements headphone routing, it will simply add audio-engine consumption of the boolean; the binding wiring is already in place. The cost is that a DJ pressing the cue button sees the LED toggle but hears nothing — which is honest about the current state ("cue is armed in the UI but headphone routing isn't built yet") and is preferable to silent unresponsiveness.

### 1.5.4. Soft-Takeover Threshold for the Filter Knob

The bipolar filter parameter has a centre detent (PRD-0056 §1.5.6) that snaps writes inside `±0.02` to exactly `0.0`. Soft-takeover defined by PRD-0044 typically uses a small threshold (e.g. `0.05`) to decide when the controller's value is "close enough" to the software value to engage. The interaction between detent snap and soft-takeover threshold is non-obvious: a controller parked at `+0.03` and software at `0.0` would have a delta of `0.03`, within the soft-takeover threshold, but the detent snap would force the controller write to `0.0` even if takeover engaged.

**Resolution:** Use the standard PRD-0044 soft-takeover threshold (`0.05` normalised) for the filter knob without special-casing the detent. The detent snap operates on the value being written, not on the comparison delta; if the controller is at `+0.03` and the software is at `0.0`, soft-takeover engages (delta `0.03` < `0.05`), and the first write is `+0.03` which the state setter snaps to `0.0` — a no-op. If the controller is at `+0.10` and the software is at `0.0`, soft-takeover does not engage immediately (delta `0.10` > `0.05`); the user has to sweep the controller through `+0.05` of the software value before takeover engages, at which point subsequent writes pass through the detent snap normally. This is the correct behaviour and requires no special code path.

### 1.5.5. Validation Severity for Unbound Registered Targets

When a registered `mixer.*` target has no binding in the bundled profile (e.g. if the DDM4000 lacks a physical control that maps cleanly to `mixer.crossfader.curve`), the validation step should warn the developer but should not block activation, since the user can always reach the parameter via the UI.

**Resolution:** WARNING-level log message, no activation block. The contract is: every binding in the profile must reference a known target (else ABORT), but not every registered target must be addressed by the profile (warning only). The unbound `mixer.crossfader.curve` is an explicit accepted outcome: the DDM4000 has no curve-selector control, and the curve is a relatively infrequent UI-only setting. The warning is informational and aids the developer in keeping the profile up to date as the registry grows; it does not punish the user.

### 1.5.6. PRD-0047 Feedback for the Cue LED

PRD-0047's MIDI feedback engine drives LEDs from ValueTree state. The cue LED is unusual: the underlying boolean (`mixer.channel.{A,B,C,D}.cue`) is UI-only, and PRD-0047 was designed primarily for LEDs that mirror audio-thread-visible state.

**Resolution:** PRD-0047's contract is "drive LED from ValueTree boolean," which is exactly satisfied here. The cue boolean is a ValueTree property; PRD-0047 listens for changes and emits the configured MIDI feedback message; no special case is needed. The audio engine's non-consumption of the property is irrelevant to PRD-0047 — feedback wiring is independent of audio-engine wiring. This PRD verifies (via integration test) that toggling `mixer.channel.A.cue` produces the expected DDM4000 cue-LED MIDI output, confirming PRD-0047's general contract works for UI-only booleans as well as audio-coupled ones.
