---
name: "EPIC-0007: Mixer"
status: Open
---

# 1. EPIC-0007: Mixer

## 1.1. Goal and Vision

Build the central mixing console of Sonik: the signal-flow stage that takes the
output of every deck (EPIC-0001), routes it through a per-channel signal chain
(gain → 3-band EQ with per-band kills → filter → channel fader), assigns it to
either side of a crossfader, sums everything into a master output bus, and
visualises levels in real time. The Mixer is the second foundational Epic
after the Deck: every transition, every blend, every EQ swap a DJ performs
during a live set physically happens in this stage.

The end-user experience targets professional DJs trained on Pioneer DJM /
Allen & Heath Xone / Behringer DDM4000 mixers. They expect: click-free EQ
kills, isolator-grade EQ depth (-26 dB or more per band), a smooth
center-detented filter sweep (HPF right / LPF left), audibly identical
crossfader curves to industry standards (sharp, smooth, transition-style),
post-fader stereo level meters with peak hold and clip indication, and
fader / knob ergonomics that feel familiar to anyone who has DJed on hardware.

The Mixer must also be the satisfying counterpart to EPIC-0005 (MIDI Controller
System): every visible control in this Epic must be addressable by the binding
targets already declared in PRD-0042 / PRD-0043 (`mixer.channel.{A,B,C,D}.*`,
`mixer.crossfader`, `mixer.master.gain`), and every level meter or LED-capable
toggle must expose a state value suitable for MIDI output feedback (PRD-0047).

## 1.2. Scope & Boundaries

### 1.2.1. In Scope

User-facing features:

- Per-deck channel strip rendered alongside / inside the deck shell (PRD-0005),
  consisting of:
  - **Channel gain knob** (trim): -inf dB … +12 dB, default 0 dB. This
    *unifies* the per-deck trim defined in PRD-0010 with the mixer's channel
    gain (see §1.3.5 — Reconciliation).
  - **3-band EQ knobs** (HIGH / MID / LOW): isolator-style, -inf dB … +6 dB,
    default 0 dB (12 o'clock), with a true cut at full counter-clockwise.
  - **3 EQ kill buttons** (one per band): momentary or latch toggles that
    fully mute the band click-free.
  - **Filter knob**: single rotary control covering both HPF (clockwise, 12 →
    5 o'clock) and LPF (counter-clockwise, 12 → 7 o'clock), with a center
    detent at 12 o'clock that bypasses the filter entirely.
  - **Channel volume fader**: 0 … 1.0 linear, default 1.0, using the same
    visual layout and behaviour established by the pitch fader in PRD-0010
    (atom reuse, no new fader atom).
  - **Crossfader assign buttons**: per-channel `A` and `B` toggles. A channel
    may be assigned to A, to B, to both (centered / through), or to neither
    (off — equivalent to "thru" with crossfader fully ignored, depending on
    final UX decision in the relevant PRD).
  - **Stereo level meter**: post-fader, post-EQ, post-filter peak + RMS,
    with peak-hold ticks and explicit clip indicator (≥ 0 dBFS).
- **Crossfader** (single global control): 0 (full A) … 1 (full B), default
  0.5 (centered). Selectable curve: sharp, smooth, transition.
- **Master output stage**: master gain knob, master stereo level meter (peak +
  RMS + clip), final hard-clip safety net (already enforced by PRD-0002).
- Click-free parameter smoothing on every continuous mixer control (gain, EQ,
  filter cutoff, channel fader, crossfader, master gain) to prevent zipper
  noise.
- State persistence of mixer parameters via `juce::ValueTree` (mirroring the
  deck state pattern), so that:
  - The UI never polls the audio thread.
  - PRD-0044 / PRD-0047 (MIDI input routing + LED feedback) can read and
    write through the existing binding-target namespace.
  - Future preference / session-save features can serialise mixer state.

Foundational systems (non-user-facing):

- A `Source/Features/Mixer/` feature slice owning all mixer DSP, state, and
  UI components.
- A refactor of the audio-engine summing stage (currently described in
  PRD-0002 as "sum the decks' per-deck-gain buffers directly to the output
  bus") into a structured signal flow:
  `decode → deck DSP → channel strip (gain → EQ → kills → filter → fader) → A/B bus → crossfader → master gain → hard-clip → output bus`.
- A real-time-safe metering bridge from audio thread to UI thread (lock-free,
  decimated to UI refresh rate, e.g. 30–60 Hz).
- A 3-band EQ DSP block (biquad-based shelving + peaking filters) and a
  state-variable filter for the HPF / LPF sweep, both implemented with
  parameter smoothing and click-free coefficient updates.

### 1.2.2. Out of Scope

- **Headphone / cue / monitoring output** (pre-fader listen, cue mix, booth
  output). These belong to a future dedicated Epic ("Cue, Headphone &
  Monitoring"). The mixer's master output is the only output bus in scope
  here.
- **Audio effects** (reverb, delay, flanger, beat-FX units). Separate Epic.
- **Send / return / aux routing**.
- **External input channels** (line-in, mic). Separate Epic if ever in scope.
- **Recording / broadcast of the mixer output**. Separate Epic.
- **Mixer preset save / recall UI**. The state lives in `ValueTree`; a
  preset manager UI is a later enhancement.
- **EQ-band parameter editor** (changing the crossover frequencies or
  filter Qs). The EQ curve is fixed by this Epic; tunable EQ shape is a
  later enhancement.

## 1.3. Implicit & Foundational Technical Requirements

### 1.3.1. Audio-Thread Safety

The mixer DSP runs entirely on the real-time audio thread (inside the
`AudioProcessor::processBlock` chain). Every PRD in this Epic must comply
with the immutable rule from `AGENTS.md`:

- No memory allocation, no locks, no I/O.
- Parameter values are read from `std::atomic<float>` (or equivalent) into
  per-block snapshots; parameter smoothing happens in-place against
  pre-allocated coefficient state.
- Metering values are written into a lock-free structure (e.g.
  `juce::AbstractFifo` or `std::atomic<float>` with peak-hold decay state) for
  the UI thread to read.

### 1.3.2. State Architecture

- A `mixer` branch is added to the central `juce::ValueTree`, with sub-trees
  per channel (`mixer.channel.A`…`.D`) and a global `mixer.master` /
  `mixer.crossfader` section.
- All mixer parameters use the exact identifiers already declared in
  PRD-0042 / PRD-0043, so that the MIDI binding registry continues to work
  without schema migration:
  - `mixer.channel.{A,B,C,D}.gain`
  - `mixer.channel.{A,B,C,D}.eq.high|mid|low`
  - `mixer.channel.{A,B,C,D}.eq.killHigh|killMid|killLow`
  - `mixer.channel.{A,B,C,D}.filter` (new — added to the registry by this
    Epic; PRD-0042 / PRD-0043 are amended accordingly)
  - `mixer.channel.{A,B,C,D}.fader`
  - `mixer.channel.{A,B,C,D}.assignA|assignB`
  - `mixer.crossfader`
  - `mixer.master.gain`
  - Read-only metering values: `mixer.channel.{A,B,C,D}.levelPeakL|R`,
    `.levelRmsL|R`, `.clip`, plus `mixer.master.levelPeakL|R`, etc.
- UI observers subscribe via JUCE Listeners; nothing polls the audio thread.

### 1.3.3. File Structure (Feature-Sliced Design)

All code lives under `Source/Features/Mixer/` and does not `#include` headers
from `Source/Features/Deck/`, `Source/Features/AudioEngine/internal/`, or
any other feature slice — only the public engine / bus contracts. The
internal layout follows Atomic Design:

- `Source/Features/Mixer/State/` — `ValueTree` schema, parameter constants.
- `Source/Features/Mixer/Dsp/` — EQ, kill smoothing, filter, gain stage,
  crossfader curve, metering.
- `Source/Features/Mixer/Routing/` — channel-strip processor, A/B bus,
  master stage.
- `Source/Features/Mixer/Ui/Atoms/` — `MixRotaryKnob` (per the Figma
  "Rotary Knob - MIX" component, fetched during implementation),
  `MixKillButton`, `MixAssignButton`, `MixLevelMeter`. The channel fader
  **reuses the existing pitch-fader atom** introduced in PRD-0010; no new
  fader atom is created.
- `Source/Features/Mixer/Ui/Molecules/` — `ChannelStripEqSection`,
  `ChannelStripMeter`, `CrossfaderRail`.
- `Source/Features/Mixer/Ui/Organisms/` — `ChannelStrip`, `MasterSection`,
  `MixerComponent`.

### 1.3.4. UI Design Language

All UI in this Epic must comply with `DESIGN.md` (strict monochrome palette
`#2d2d2d` / `#fdfdfd`, `Space Mono Regular`, 2-px solid borders, zero
border-radius, dithered patterns, pixel-art icons, tonal layering).

In addition, two component-level design directives are binding for this Epic:

- **Rotary knobs**: all mixer knobs (gain, EQ HIGH/MID/LOW, filter, master
  gain) follow the Figma "Rotary Knob - MIX" component from
  `https://www.figma.com/design/3bmQVcRbY9JSaJqTCPH9AQ/Audio-Plugin-UI-Elements?node-id=0-1`.
  The Figma context must be fetched *during* the implementation PRDs (not
  during planning), translated into a single reusable atom (`MixRotaryKnob`),
  and adapted to comply with the project's monochrome `DESIGN.md` palette
  where the source assets diverge.
- **Faders**: channel faders and the crossfader reuse the layout, hit-target,
  double-click-reset, and visual treatment of the pitch fader established in
  PRD-0010. No second fader atom is introduced.

### 1.3.5. Reconciliation With Existing PRDs

Two pre-existing PRDs partially overlap with this Epic and must be
reconciled, not duplicated:

- **PRD-0002 (Audio Engine Core)** currently specifies "sum the per-deck,
  per-deck-gain buffers directly into the stereo output bus." The mixer
  refactor changes the summing stage. PRD-0002 is **not rewritten**; the
  first PRD of this Epic (Mixer Signal-Flow & Routing) explicitly amends
  the summing contract and documents the new pipeline. PRD-0002's hard-clip
  safety net and atomic-gain contract remain authoritative.
- **PRD-0010 (Pitch Fader & Gain Control)** defines a per-deck gain knob
  with the same -inf … +12 dB range and double-click-to-unity behaviour
  this Epic specifies for the channel gain. **They are the same parameter.**
  The mixer's channel gain knob *replaces* the deck-shell gain knob, reading
  from and writing to the same `ValueTree` field that PRD-0010 established.
  The relevant PRD in this Epic will explicitly state the migration and
  delete the now-redundant gain knob from the deck shell, while preserving
  PRD-0010's pitch-fader behaviour untouched.

### 1.3.6. DSP Targets

- **3-band EQ**: low shelf + mid peaking + high shelf, isolator-style depth
  (full counter-clockwise = -inf dB / true cut, not -12 dB). Crossover
  defaults TBD per implementing PRD; common starting point is low/mid ~
  200–300 Hz, mid/high ~ 2–4 kHz.
- **Per-band kill**: smoothed gain ramp to -inf dB over ~5–10 ms to prevent
  audible clicks; the kill state is the toggle, the smoothing is internal.
- **Filter**: state-variable filter, cutoff sweep mapped exponentially across
  the knob travel; 12 o'clock = bypass (filter coefficients set to identity
  / DSP block skipped), clockwise = HPF with cutoff rising from ~20 Hz to
  ~10–20 kHz, counter-clockwise = LPF with cutoff falling from ~20 kHz to
  ~20 Hz. Resonance is fixed by this Epic.
- **Crossfader curves**: at minimum *sharp* (hard cut near the edges, used
  for scratching) and *smooth* (equal-power blend, used for mixing).
- **Metering**: peak + RMS, peak-hold decay ~1.5 s, clip indicator latches
  on any sample ≥ 0 dBFS at the channel output (post-fader) or master
  output, with a manual reset on user click or after a defined hold time.

## 1.4. PRD Roadmap

The Epic is sequenced so each PRD compiles, runs, and is testable without
the later ones. Numbers will be assigned by `generate_doc.sh` at drafting
time; here we use `TBD` per the Epic Breakdown Workflow.

- [ ] PRD-0052: Mixer State Schema & ValueTree Integration
- [ ] PRD-0053: Mixer Signal-Flow & Routing Refactor (Audio Engine)
- [ ] PRD-0054: Channel Gain Stage & Channel Fader (with PRD-0010 reconciliation)
- [ ] PRD-0055: 3-Band EQ DSP & Per-Band Kill
- [ ] PRD-0056: Per-Channel Filter Knob (HPF / LPF Sweep)
- [ ] PRD-0057: Crossfader & A/B Channel Assignment
- [ ] PRD-0058: Master Output Stage & Output Level Metering
- [ ] PRD-0059: Mixer UI Atoms (`MixRotaryKnob`, `MixKillButton`, `MixAssignButton`, `MixLevelMeter`)
- [ ] PRD-0060: Channel Strip Organism & Mixer Shell
- [ ] PRD-0061: Mixer ↔ MIDI Wiring Validation & DDM4000 Profile Activation
