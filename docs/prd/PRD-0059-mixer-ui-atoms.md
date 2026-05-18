---
status: Not Implemented
epic: EPIC-0007
depends-on:
  - PRD-0010
  - PRD-0052
  - PRD-0058
---

# 1. PRD-0059: Mixer UI Atoms

## 1.1. Problem

EPIC-0007's channel-strip organism (PRD-0060) and master-section organism compose UI atoms — rotary knobs, kill buttons, A/B assign buttons, and stereo level meters — that do not yet exist anywhere in the codebase. Without a shared set of mixer-specific atoms, every organism PRD would re-roll its own knob and meter visuals, producing inconsistent stroke weights, inconsistent active/inactive fill rules, inconsistent typography, and inconsistent meter ballistics. That is exactly the failure mode `DESIGN.md` exists to prevent.

The atoms must satisfy two constraints simultaneously: (a) strict compliance with `DESIGN.md` (monochrome `#2d2d2d` / `#fdfdfd`, Space Mono Regular, 2-px solid borders, zero border-radius, dithered patterns instead of gradients, pixel-art icons, tonal layering), and (b) compatibility with the data sources PRD-0052 (ValueTree state) and PRD-0058 (MixerMeterSnapshot atomics) already publish. A naive atom library that polls the audio thread for parameter values would violate `AGENTS.md`; a meter atom that uses `ValueTree::Listener` to observe meter updates would create the listener storm PRD-0052 §1.5.4 explicitly designed against.

A new fader atom is explicitly not in scope: PRD-0060 reuses PRD-0010's pitch fader (generalised with horizontal orientation and detent contract) for both the channel volume fader and the crossfader. This PRD's atom set is exactly four: `MixRotaryKnob`, `MixKillButton`, `MixAssignButton`, `MixLevelMeter`.

## 1.2. Objective

The system provides four reusable mixer UI atoms under `Source/Features/Mixer/Ui/Atoms/`, each of which:

- Complies with `DESIGN.md`: monochrome palette, Space Mono Regular typography, 2-px borders with explicit active/inactive fill inversion, zero `border-radius`, dithered patterns, pixel-art icons, tonal layering for depth.
- Binds to a single ValueTree path (PRD-0052) for two-way state synchronisation, via `juce::ValueTree::Listener` registered in `componentParentHierarchyChanged` (or at construction with explicit lifetime management).
- Exposes no audio-thread interaction: no `processBlock` call, no atomic write, no polling timer reading audio state — except `MixLevelMeter`, which reads from the `MixerMeterSnapshot` lock-free atomic block (PRD-0058) at a 60 Hz `juce::Timer` interval.
- Provides standard interaction affordances: knob/fader double-click to reset to default, mouse-wheel for fine adjustment, click-drag for continuous motion, modifier keys (shift = fine, cmd/ctrl = bypass).
- Renders the rotary knob visual derived from the Figma "Rotary Knob - MIX" component (fetched during implementation, not at PRD-draft time).

## 1.3. User Flow

1. The user sees a `MixRotaryKnob` for the channel gain control. It is rendered as a circle with a pixel-art tick mark indicating current value, a 2-px solid border in `#2d2d2d`, and a tonal-layered fill matching `DESIGN.md` for "interactive control." A small Space Mono label above shows the parameter name ("GAIN"); a small label below shows the current value ("+0.0 dB"), formatted per the knob's value-display contract.
2. The user clicks and drags the knob vertically. The bound ValueTree property updates continuously; the knob's tick mark rotates to match. Releasing the mouse stops the gesture; the value persists.
3. The user double-clicks the knob. It snaps to its declared default value (0 dB for gain, 0 dB for EQ bands, 0.0 for filter, 0 dB for master gain). The bound ValueTree property updates.
4. The user scrolls the mouse wheel over the knob. The value adjusts by a small step (default 1 LSB of the knob's normalised range; ~0.5 dB for gain knobs).
5. The user holds shift while dragging. The drag sensitivity drops 4×, allowing fine adjustment.
6. The user clicks a `MixKillButton` next to the MID EQ knob. The button visually inverts: the border-fill swap defined in `DESIGN.md` makes the active button appear as a solid `#2d2d2d` rectangle with `#fdfdfd` label text. The bound `eq.killMid` boolean flips to true; the audio engine smoothly kills the band (PRD-0055).
7. The user clicks the same button again. The button reverts to inactive (border-only fill); the boolean flips back; the audio engine ramps the band back to the user's last knob value.
8. The user clicks a `MixAssignButton` (the A or B button). Its active/inactive visual is identical to `MixKillButton` (both are binary latching buttons); the only difference is the label ("A" or "B") and the bound ValueTree path.
9. The user observes a `MixLevelMeter` while audio plays. Two vertical bars (L and R stereo channels) update at 60 Hz, showing instantaneous peak (as a filled bar from bottom up to the current peak level) with a separate "tick" marker showing the peak-hold position decaying linearly over 1.5 s. A clip indicator (a small square in the meter's top-left) lights up `#fdfdfd` when the channel has latched a clip; clicking the meter clears the clip.
10. The user resizes the parent organism, shrinking the channel strip. Each atom honours its minimum size (knob 24×24 px, button 16×16 px, meter 24 px wide × 80 px tall) by clipping visual decoration before clipping interaction surface (see PRD-0060 §1.5.7 for fallback rules at the strip level).

## 1.4. Acceptance Criteria

- [ ] `Source/Features/Mixer/Ui/Atoms/MixRotaryKnob.h/.cpp` exists. Constructor accepts a `juce::ValueTree`, a property `juce::Identifier`, a normalisation map (linear / dB-tapered / bipolar with detent), a default value, and an optional display formatter.
- [ ] `Source/Features/Mixer/Ui/Atoms/MixKillButton.h/.cpp` exists. Constructor accepts a `juce::ValueTree`, a property `juce::Identifier` (boolean), an optional label string (defaults to "KILL"). The button is a binary latch.
- [ ] `Source/Features/Mixer/Ui/Atoms/MixAssignButton.h/.cpp` exists. Constructor accepts a `juce::ValueTree`, a property `juce::Identifier` (boolean), and a label string ("A" or "B"). Visual behaviour is identical to `MixKillButton`; the only difference is the bound property and label semantic.
- [ ] `Source/Features/Mixer/Ui/Atoms/MixLevelMeter.h/.cpp` exists. Constructor accepts a `MixerMeterSnapshot` reference (PRD-0058) and a slot identifier prefix (e.g. `mixer.channel.A` or `mixer.master`). Reads `levelPeakL`, `levelPeakR`, `levelPeakHoldL`, `levelPeakHoldR`, and `clip` from the snapshot via `std::atomic::load(memory_order_relaxed)` at 60 Hz via `juce::Timer::startTimerHz(60)`.
- [ ] `MixRotaryKnob` supports: click-drag (vertical and rotational; configurable, default vertical), mouse-wheel adjustment, double-click reset to declared default, shift-modifier fine adjustment (4× lower sensitivity), and bipolar detent behaviour (snap to centre when the underlying value is `0.0` for bipolar parameters such as the filter knob).
- [ ] `MixRotaryKnob` reads / writes its bound ValueTree property via a `juce::ValueTree::Listener` registered on construction and removed on destruction. No knob calls into the audio thread.
- [ ] `MixRotaryKnob` visual rendering derives from the Figma "Rotary Knob - MIX" component (the Figma fetch is an implementation-time activity, not a PRD-draft activity; this PRD requires only that the atom is structured to accept a per-style rendering hook that the implementation populates from the Figma asset).
- [ ] `MixKillButton` and `MixAssignButton` render exactly per `DESIGN.md` active/inactive button rules: inactive = `#2d2d2d` 2-px border on `#fdfdfd` fill with `#2d2d2d` label; active = `#2d2d2d` 2-px border on `#2d2d2d` fill with `#fdfdfd` label. Zero border-radius. Space Mono Regular for the label.
- [ ] `MixLevelMeter` renders two vertical stereo bars (left, right) using a dithered pattern for the filled portion (per `DESIGN.md`, no gradients). Peak position is shown as the top edge of the filled bar; peak-hold position is shown as a separate 1-px-tall tick on top of the bar; clip is shown as a small filled square in the meter's top-left corner.
- [ ] Clicking `MixLevelMeter` clears the clip latch by writing `false` to the underlying clip atomic. No other meter interaction is exposed.
- [ ] `MixLevelMeter`'s 60 Hz timer is started in `parentHierarchyChanged` when first added to a visible parent, and stopped when removed. The timer does no work other than `repaint()`-with-coalescing after loading atomic values into local cache.
- [ ] No UI atom polls the audio thread or any `ValueTree` property at a timer; ValueTree-bound atoms are reactive (`valueTreePropertyChanged`).
- [ ] All four atoms respect the `DESIGN.md` constraint set: monochrome `#2d2d2d` / `#fdfdfd` palette, Space Mono Regular typography, 2-px borders, zero border-radius, dithered patterns, pixel-art ticks/icons, tonal layering for depth.
- [ ] The four atoms have minimum sizes: `MixRotaryKnob` 24×24 px, `MixKillButton` 16×16 px, `MixAssignButton` 16×16 px, `MixLevelMeter` 24×80 px. Below the minimum, the atom clips visual decoration (tick density, label visibility) before clipping interaction surface.
- [ ] No audio-thread code is added by this PRD; all audio-thread interaction is mediated via PRD-0052's ValueTree → atomic snapshot and PRD-0058's `MixerMeterSnapshot`. The audio-thread paths read by these atoms perform no allocation, take no locks, and perform no I/O, as guaranteed by their upstream PRDs.
- [ ] At least one unit test in `Tests/` (e.g. `MixerAtomsTests.cpp`) verifies: (a) `MixRotaryKnob` two-way binding (ValueTree write reflects in knob value; knob drag updates ValueTree), (b) `MixKillButton` toggle flips ValueTree bool and visually inverts, (c) `MixLevelMeter` reads from the meter snapshot and triggers `repaint()` at the configured 60 Hz, (d) double-click reset behaviour for the knob.

## 1.5. Grey Areas

### 1.5.1. Rotary Knob Drag Axis

DJ users from a hardware background often expect rotary drag (drag along an arc around the knob centre); DAW users expect vertical drag (drag up = increase). Software DJ apps split the difference. Choosing one as default and ignoring the other risks alienating half the audience.

**Resolution:** Default is vertical drag (drag up = increase). Vertical drag is the more discoverable interaction on a touchpad or mouse without a scroll wheel, is the convention every reference DJ software (Traktor, Serato, Rekordbox) uses for on-screen knobs, and avoids the well-known "rotational drag direction reversal" bug that plagues arc-drag implementations near the 6 o'clock position. The atom exposes an `interactionMode` enum (`Vertical` default, `Rotational`) that an organism or app-wide settings can override; the default applies unless explicitly changed at the atom-level call site. No app-wide setting is shipped by this PRD; that is a future preferences-Epic concern.

### 1.5.2. Knob Value Display Behaviour

A knob can display its value (a) always (label below), (b) only while dragging (tooltip / overlay), (c) never (rely on the tick visual). Always-visible competes for vertical space and clutters the strip; never makes precise adjustment impossible.

**Resolution:** Always display a small label below the knob, in Space Mono Regular at 9 pt. The label uses the formatter supplied to the constructor (default: dB-with-sign for dB-tapered parameters, two-decimal float for normalised parameters, `KILL` text overlay when the related kill bool is true for EQ bands). This consumes ~12 px of vertical space per knob; PRD-0060's strip layout reserves it. The cost is small and the discoverability win is large: a DJ can see at a glance what every knob is set to, which matters more in software than on hardware (where finger position reveals value).

### 1.5.3. Meter Bar Render: Continuous Fill vs Segmented

Hardware meters are often segmented (discrete LED steps); software meters more often render a continuous filled bar. `DESIGN.md` explicitly prefers dithered patterns over gradients but says nothing about segmentation.

**Resolution:** Continuous-fill bar rendered with a dithered pattern. The bar's height represents instantaneous peak; the dithering provides the texture `DESIGN.md` requires without segmenting. A separate 1-px tick line on top of the bar marks peak-hold. Segmentation is rejected because it (a) introduces an arbitrary discrete resolution (how many steps?) that conflicts with the meter's underlying continuous data, (b) makes peak-hold visualisation harder (the tick has to align with a segment edge), and (c) requires another colour/state encoding to distinguish "current" from "decaying" segments. A continuous bar is simpler and more honest to the data.

### 1.5.4. Meter Polling Rate

PRD-0058 publishes meter atomics at the audio block rate (~172 Hz at 256-sample / 44.1 kHz blocks). UI polls below that rate; 30 Hz feels choppy at fast transients; 60 Hz is smooth; 120 Hz wastes CPU on most displays.

**Resolution:** 60 Hz polling via `juce::Timer::startTimerHz(60)`. This matches typical display refresh, is well below the audio update rate (so no information is missed unless multiple peaks occur within one frame, which is acceptable for a meter), and is the convention every reference DJ software uses. The timer is started in `parentHierarchyChanged` (when the meter is first attached to a visible parent) and stopped on removal, to avoid running timers for off-screen meters.

### 1.5.5. Modifier Keys for Knob Adjustment

Common conventions: shift = fine adjustment (4× lower sensitivity), cmd/ctrl = bypass / momentary (hold to temporarily zero or reset), alt = reset to default. Mac vs Windows modifier mapping is a common source of bugs.

**Resolution:** Shift = fine adjustment (4× lower sensitivity) is universal across platforms. Double-click = reset to default, replacing the alt-modifier convention (which conflicts with macOS alt-click drag conventions in some host apps). Cmd/ctrl is reserved and unbound by this PRD (a future Epic may bind it to "preview / bypass" behaviour). Mouse-wheel = standard step adjustment. These four interactions cover the realistic use cases without introducing platform-specific modifier handling that would diverge between macOS and Windows builds.

### 1.5.6. Fader Atom Reuse vs New Atom

The Mixer Epic explicitly forbids creating a second fader atom; PRD-0060 reuses PRD-0010's pitch fader (generalised with `Orientation` and `detentValue` parameters) for both the channel volume fader and the crossfader. The question for this PRD is whether to introduce a thin `MixFader` wrapper that pre-configures the PRD-0010 atom for mixer use.

**Resolution:** No wrapper. PRD-0060 instantiates the PRD-0010 fader atom directly with the appropriate orientation and detent parameters per call site. A thin wrapper would add a file, a class, and a layer of indirection that obscures the underlying atom and tempts future contributors to add wrapper-specific behaviour, which would re-fork the atom in spirit if not in code. The Mixer's atom set is therefore exactly four (`MixRotaryKnob`, `MixKillButton`, `MixAssignButton`, `MixLevelMeter`); the fader is borrowed from PRD-0010 unchanged.

### 1.5.7. Figma Asset Fetch Timing

The Mixer Epic specifies that rotary knob visuals derive from the Figma "Rotary Knob - MIX" component. Fetching Figma assets during PRD drafting is impractical (the asset evolves, the design language tools may not be available); fetching during implementation is the conventional choice.

**Resolution:** Defer the Figma fetch to the implementation phase of this PRD. The atom is structured to accept a per-style rendering hook (`MixRotaryKnob::setVisualStyle(juce::Image, ...)` or similar) that the implementation populates from the Figma asset at app startup or via a baked-in resource. The PRD's acceptance criteria require only that the atom is structurally ready to receive the visual asset, not that the asset is committed at PRD-draft time. The implementation step (after this PRD is approved) executes the Figma fetch, exports the relevant asset (pixel-art ticks, tonal-layered fill pattern, label typography reference), and wires it into the atom.
