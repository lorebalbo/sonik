---
status: Not Implemented
epic: EPIC-0002
depends-on:
  - PRD-0005
  - PRD-0019
  - PRD-0020
  - PRD-0021
  - PRD-0022
---

# 1. PRD-0023: Stem Separation UI

## 1.1. Problem

The full stem-separation backend stack is complete by the time this PRD is reached: ONNX Runtime integration and model management (PRD-0019), the separation engine and persistent cache (PRD-0020), stem-aware multi-buffer audio playback (PRD-0021), and stem-aware time stretching (PRD-0022) all function end to end. The `StemSeparationManager` exposes a clean orchestration API (`startSeparation`, `cancelSeparation`, `getStemData`, `isModelReady`, `setStemReadyCallback`), and every job's lifecycle is mirrored into a `Stems` child node of the deck's ValueTree with properties for `status`, `progress`, per-stem mute flags, error fields, and model version.

What is missing is the user-facing surface. A DJ standing in front of a deck has no way to trigger separation, no way to watch it progress, no way to cancel a long-running job, and no way to mute or unmute the resulting vocal and instrumental stems. The engine is a powerful machine with no buttons. Two skeleton components already exist in the tree — `StemSeparateButton` and `StemToggleComponent` — and are already constructed and docked by `DeckShellComponent`, but their behaviour, state lifecycle, disabled conditions, progress rendering, and DESIGN.md conformance are unspecified and only partially implemented.

This is further complicated by two realities of the backend. First, the BS-RoFormer / `htdemucs` inference pipeline does not report fine-grained progress: a `Run()` call is effectively a black box that returns a finished tensor with no intermediate percentage. A naive progress bar would sit at 0% for the entire job and then jump to 100%, which reads as a frozen application. Second, DESIGN.md contains no dedicated specification for stem components — there is no "stem button" or "stem toggle" entry in the design language — so the visual contract must be derived from the generic tactile-button, toggle, colour, and dithering rules, and that derivation must be made explicit rather than left to each implementer's taste.

## 1.2. Objective

The system surfaces the existing stem-separation engine on every deck through a small, DESIGN.md-conformant control cluster such that:

- A "SEPARATE STEMS" button (`StemSeparateButton`), docked in the deck's stems sidebar, triggers `StemSeparationManager::startSeparation(deckId)` on click when a separable track is loaded, and is disabled (greyed, non-interactive, with an explanatory tooltip) when no track is loaded, when the loaded track is shorter than `kMinTrackDurationSeconds` (5.0 s), or while a separation is already in progress for that deck.
- While a separation runs, the button renders a progress indicator combining a numeric percentage and a cycling phase label, driven by the `progress` float and `status` string on the deck's `Stems` ValueTree node, using a DESIGN.md-compliant dithered fill (no green/yellow/red, density increasing with progress) rather than a coloured progress bar.
- The status lifecycle `"none"` → `"queued"` → `"separating"` → `"loading_cached"` → `"ready"` (with terminal error states `"error"` and `"model_unavailable"`) is reflected faithfully in the button's visual state, including the cache-hit fast path where `status` transitions `"none"` → `"loading_cached"` → `"ready"` without ever passing through `"separating"`.
- The user can abort an in-progress separation; doing so calls `StemSeparationManager::cancelSeparation(deckId)`, the status reverts to `"none"`, and the button returns to its idle, clickable state.
- Two toggle buttons, "VOC" and "INST" (`StemToggleComponent`), mute and unmute the vocal and instrumental stems respectively. VOC drives the single `vocalsMuted` property; INST drives the three mute properties `drumsMuted`, `bassMuted`, and `otherMuted` together as one logical instrumental group. Both toggles are interactive only when `status == "ready"` and are disabled (greyed) in every other state.
- Mute/unmute transitions are click-free because the per-stem gain ramps are handled entirely by the audio layer (PRD-0021's 64-sample crossfade); the UI only flips the boolean ValueTree properties and never touches the audio thread.
- Error states (`"error"`, `"model_unavailable"`) are surfaced legibly on the button, and a `consecutiveErrors` counter governs whether the button offers an immediate retry or a more emphatic failure presentation.
- The entire cluster conforms to DESIGN.md's monochrome palette (`#2d2d2d` / `#fdfdfd`), `Space Mono` typography, mandatory `2px solid #2d2d2d` borders, zero `border-radius`, instant (non-animated) state inversion for the toggles, and dithering-in-place-of-gradients for the progress fill.

Out of scope for this PRD: the 4-stem UI (only VOC and INST are exposed; the four individual stem toggles are a future-Epic concern), the Original-vs-stems source-mode toggle (specified separately in PRD-0062), and any stem-specific EQ or effects.

## 1.3. User Flow

1. The DJ loads a track longer than 5 seconds onto Deck A. `DeckShellComponent` shows the SEPARATE STEMS button in its idle, inactive state (`#fdfdfd` fill, `#2d2d2d` text and border). The VOC and INST toggles are visible but disabled (greyed), because `status == "none"`.
2. The DJ clicks SEPARATE STEMS. The component calls `StemSeparationManager::startSeparation("A")`. The `Stems` node's `status` becomes `"queued"` then `"separating"`, and `progress` begins to move. The button switches to its in-progress presentation: a numeric percentage, a cycling phase label (e.g. "ANALYSING", "SEPARATING", "RECONSTRUCTING"), and a dithered fill whose density grows toward the right edge of the button.
3. The displayed percentage races ahead to roughly 85% within the first seconds and then idles, advancing only slowly, because the real engine reports no fine-grained progress (see §1.5.2). The phase label cycles every 3 seconds so the indicator never looks frozen. Audio playback on the deck continues uninterrupted throughout.
4. If the DJ changes their mind, they click the button again (now acting as a cancel affordance) or its dedicated cancel hotspot; the component calls `StemSeparationManager::cancelSeparation("A")`. The status reverts to `"none"`, the dithered fill clears, and the button returns to its idle, clickable state.
5. When inference, iSTFT, and stem WAV export complete, the manager pushes `status` to `"ready"` and `progress` to 1.0. The button snaps to its completed presentation (or hides, per existing `DeckShellComponent` behaviour that swaps it out once stems are ready), and the VOC and INST toggles become enabled.
6. The DJ clicks VOC. `vocalsMuted` flips to `true`; the audio layer crossfades the vocal stem out over 64 samples; the VOC toggle inverts to its active presentation (`#2d2d2d` fill, `#fdfdfd` text) instantly with no fade. Clicking VOC again unmutes. The DJ clicks INST; `drumsMuted`, `bassMuted`, and `otherMuted` all flip together, the instrumental bed crossfades out, and INST inverts.
7. Later, the DJ reloads the same track (or loads it onto another deck). Because the stems are cached, the manager skips re-processing: `status` goes `"none"` → `"loading_cached"` → `"ready"`. The button briefly shows a "LOADING" presentation while the cached WAVs stream into memory, then the toggles enable. No progress bar, no inference, no waiting on the model.
8. If the model is missing or fails to load, clicking SEPARATE STEMS (or loading a track when the model is unavailable) leaves `status == "model_unavailable"`; the button shows a clear "MODEL N/A" presentation and the toggles stay disabled. If inference fails, `status` becomes `"error"`, `stemError` carries detail, and `consecutiveErrors` increments; the button shows an "ERROR" presentation that the DJ can click to retry.

## 1.4. Acceptance Criteria

- [ ] `StemSeparateButton` renders as a DESIGN.md tactile button: a massive square (within the docked sidebar bounds), `2px solid #2d2d2d` border at all times, `Space Mono` label, zero `border-radius`, idle/inactive presentation `#fdfdfd` fill with `#2d2d2d` text.
- [ ] The button is disabled and visibly greyed, and does not call `startSeparation`, when: (a) no track is loaded (`isEmpty`), (b) the loaded track is shorter than `kMinTrackDurationSeconds` (`isShortTrack`), or (c) a separation is already in progress (`status` is `"queued"` or `"separating"` or `"loading_cached"`).
- [ ] When disabled because the track is too short, the button's tooltip (via `SettableTooltipClient`) explains the reason, naming the 5-second minimum.
- [ ] Clicking the button when enabled and `status == "none"` calls `StemSeparationManager::startSeparation(deckId)` exactly once.
- [ ] While `status == "separating"`, the button displays a numeric percentage and a phase label drawn from `phaseLabels()`, with the phase label advancing every 3 seconds.
- [ ] The displayed progress is the hybrid "animated estimate" (see §1.5.2): it races to approximately 85% quickly, then idles, and only resolves to 100% when `status` reaches `"ready"`. The raw `progress` float bounds the animated value (the animation never displays a value lower than the real `progress`).
- [ ] The in-progress fill is rendered as a dithered (checkerboard) pattern of `#2d2d2d` whose density increases with progress; it uses no green, yellow, or red and no smooth gradient.
- [ ] Clicking the button while `status` is `"queued"` or `"separating"` calls `StemSeparationManager::cancelSeparation(deckId)`; the status reverts to `"none"` and the button returns to its idle clickable state.
- [ ] On a cache hit, the button passes through a brief `"loading_cached"` presentation (a "LOADING" label, no percentage and no inference) and then the toggles enable when `status` reaches `"ready"`; the button never displays a `"separating"` progress bar on a cache hit.
- [ ] `status == "error"` renders an "ERROR" presentation on the button; clicking it retries by calling `startSeparation(deckId)` again. The button reads `stemError` for any tooltip detail.
- [ ] `status == "model_unavailable"` renders a "MODEL N/A" presentation; the button is non-actionable for separation (a click does not start a job) and the toggles stay disabled. `StemSeparationManager::isModelReady()` is consulted to distinguish this state.
- [ ] `consecutiveErrors` is read by the button; after repeated failures the presentation reflects a persistent-failure state (see §1.5.5) rather than inviting endless one-click retries with no indication that prior attempts failed.
- [ ] `StemToggleComponent` renders the VOC and INST toggles as DESIGN.md tactile toggles: square, `2px solid #2d2d2d` border, `Space Mono` label, zero `border-radius`, inactive presentation `#fdfdfd`/`#2d2d2d`, active (muted) presentation inverted to `#2d2d2d`/`#fdfdfd`, with instant state change and no animation or fade.
- [ ] Both toggles are interactive only when `status == "ready"`; in every other status they are disabled and greyed and ignore clicks.
- [ ] The VOC toggle reads and writes the single `vocalsMuted` property; clicking it inverts that boolean.
- [ ] The INST toggle reads and writes the three properties `drumsMuted`, `bassMuted`, and `otherMuted` as one group: a click sets all three to the same new value. Its displayed muted state is `true` when all three are muted (see §1.5.1 for mixed-state handling).
- [ ] The toggles only mutate ValueTree boolean properties; they perform no audio-thread work and rely on PRD-0021's 64-sample crossfade for click-free transitions.
- [ ] The cluster is laid out within `DeckShellComponent`'s stems sidebar with no `border-radius`, monochrome fills only, and `Space Mono` text, matching the surrounding deck organism.
- [ ] No new audio-thread code is introduced. The UI communicates with the engine exclusively through `StemSeparationManager`'s message-thread API and through ValueTree property reads/writes; it performs no allocation, locking, or I/O on the audio thread.
- [ ] The 4-stem UI, the Original-vs-stems source toggle (PRD-0062), and stem EQ/FX are not implemented by this PRD.

## 1.5. Grey Areas

### 1.5.1. INST as One Toggle vs Three, and Mixed Mute State

The instrumental group is the sum of three model stems (drums, bass, other), each with its own mute property (`drumsMuted`, `bassMuted`, `otherMuted`). The MVP exposes a single "INST" button, so the question is how one toggle maps onto three booleans, and what the toggle should display if the three booleans ever disagree (e.g. a future feature or a MIDI binding mutes only `bassMuted`).

**Resolution:** INST is a single toggle that writes all three instrumental mute properties together: a click sets `drumsMuted`, `bassMuted`, and `otherMuted` to the same new value in one ValueTree transaction. For display, the toggle reads as "muted" (active/inverted) only when all three are `true`, and as "unmuted" otherwise; a mixed state (some but not all of the three muted) displays as unmuted, and the next click mutes all three. This keeps the MVP honest about its two-stem abstraction while leaving the underlying N-stem properties intact for the future 4-stem UI. The mixed-state-displays-as-unmuted rule is the least surprising default: the only way to reach a mixed state in this Epic is via an external agent, and "click INST to force the whole instrumental bed off" is the expected recovery action.

### 1.5.2. Progress: Real Engine Progress vs Animated Estimate

BS-RoFormer / `htdemucs` inference does not expose fine-grained progress. A `Run()` call returns a finished tensor with no intermediate percentage, so the `progress` float on the `Stems` node moves in a few coarse steps (queued, inference started, iSTFT, export) rather than smoothly from 0 to 1. A progress bar bound directly to `progress` would sit nearly still and then jump, reading as a frozen app.

**Resolution:** Use a hybrid. The displayed value is an animated estimate that races to roughly 85% within the first seconds of `"separating"`, then idles, advancing only slowly, until the real `status` reaches `"ready"` (at which point it resolves to 100%). The animation is bounded below by the real `progress` float — if the engine ever reports a value higher than the current animation, the display jumps up to match, so the estimate never lies in the pessimistic direction. The cycling phase label (advancing every 3 seconds) provides the secondary "something is happening" signal that the coarse percentage cannot. This is honest enough (it never claims completion before the engine confirms it) and avoids the dead-bar perception. The exact race curve and idle rate are presentation tuning, not contract: the contract is "monotonic, bounded below by real progress, never reaches 100% before `status == \"ready\"`, never appears frozen."

### 1.5.3. VOC/INST Before Separation: Disabled vs Hidden

Before a track has been separated (`status != "ready"`), the VOC and INST toggles have nothing to act on. They could be hidden entirely until stems are ready, or shown but disabled.

**Resolution:** Shown but disabled and greyed. Hiding the toggles makes the deck layout jump when separation completes (controls appear out of nowhere), which violates the "rigid, predictable interface" spirit of DESIGN.md and disorients the DJ mid-performance. Keeping them visible-but-disabled communicates the affordance ("these buttons exist and will become usable once you separate") without offering a non-functional click. Greyed here means rendered with the standard inactive border but a dithered/desaturated fill that reads as non-interactive, consistent with the SEPARATE STEMS disabled state, so the whole cluster shares one disabled vocabulary.

### 1.5.4. Cancel Mid-Separation: Target State

When the DJ cancels a running job, the system must decide what state to land in. Options: revert to `"none"` (as if never started), land in a distinct `"cancelled"` state, or land in `"error"`.

**Resolution:** Revert to `"none"` and re-enable the button. A cancellation is a deliberate user action, not a failure, so an `"error"` state would be misleading and would (per §1.5.5) inflate `consecutiveErrors`. A distinct `"cancelled"` state would add a status with no behavioural difference from `"none"` (the button is idle and clickable, the toggles are disabled) and would complicate every status switch for no user benefit. Reverting to `"none"` is the simplest correct outcome: the deck looks exactly as it did before the DJ pressed SEPARATE STEMS, and they can immediately press it again. `cancelSeparation` is responsible for tearing down any partial work and resetting `progress` to 0 before the status reaches `"none"`.

### 1.5.5. Short-Track Disable and Repeated-Error Presentation

Two related edge presentations need defining: the sub-5-second disable, and what happens after several failed attempts. For short tracks, simply greying the button without explanation leaves the DJ wondering why it does nothing. For repeated errors, a button that always offers a one-click retry can trap the DJ in a loop of identical failures with no signal that anything is wrong.

**Resolution:** For short tracks, the button is disabled and greyed and its tooltip explains the constraint explicitly (naming the 5-second `kMinTrackDurationSeconds` minimum), so the disable is self-documenting on hover. For repeated errors, the button reads `consecutiveErrors`: the first one or two failures present a normal "ERROR — click to retry" affordance, but once `consecutiveErrors` crosses a small threshold the presentation shifts to a persistent-failure state that still surfaces `stemError` (so the DJ can read what went wrong) and de-emphasises the retry, signalling that the problem is unlikely to resolve by clicking again. The threshold value is presentation tuning, not contract; the contract is that the button must distinguish a transient first failure from a persistent one rather than presenting an identical infinite retry.

### 1.5.6. Deriving Stem-Component Visuals From Generic DESIGN.md Rules

DESIGN.md contains no dedicated specification for stem components — there is no "stem button", "stem toggle", or "separation progress" entry in the design language. Every other organism in the app (waveforms, knobs, faders, jog wheels, the library) has an explicit spec; the stem cluster does not. Implementers could therefore each invent a different look, fragmenting the design language.

**Resolution:** The stem cluster's visuals are derived entirely from DESIGN.md's generic rules, and this derivation is the binding spec for this PRD. Specifically: the SEPARATE STEMS button and the VOC/INST toggles follow the "Tactile Buttons" rule (massive squares, mandatory `2px solid #2d2d2d` border, active = `#2d2d2d` fill / `#fdfdfd` text, inactive = `#fdfdfd` fill / `#2d2d2d` text); the in-progress fill follows the "Dithered Gradients (Audio Levels & VU Meters)" rule (checkerboard density instead of green/yellow/red); all text uses `Space Mono`; zero `border-radius` per the "Zero Roundedness" rule; and instant state change per "design for instant hover/active states." Because this is a derivation rather than a first-class spec, a future DESIGN.md revision may add an explicit stem-component section; should it conflict with this derivation, the explicit DESIGN.md section wins and this PRD's visuals are amended to match. Calling this out here prevents silent divergence and makes the provenance of the visuals auditable.

### 1.5.7. Cache-Hit Fast Path: Brief "loading_cached" State vs Instant Enable

On a cache hit the stems already exist on disk, so no inference is needed — but the cached WAVs still have to be streamed from disk into memory buffers before the toggles can safely act. The UI could pretend this is instantaneous (jump straight to enabled toggles) or surface the brief load.

**Resolution:** Surface a brief `"loading_cached"` state. The cached-WAV load is fast but not free (a 5-minute track is tens of megabytes per stem), and enabling the toggles before the buffers are resident would risk a click on a not-yet-loaded stem. A short, honest "LOADING" presentation on the button — distinct from the `"separating"` progress bar, with no percentage and no phase cycling — covers the gap, then the toggles enable when `status` reaches `"ready"`. This reuses the existing status lifecycle (`"none"` → `"loading_cached"` → `"ready"`) rather than inventing a parallel "instant" path, keeps the enable-toggles trigger uniformly tied to `"ready"`, and is visually quiet enough that a fast load reads as effectively instant while a slower load still shows that the app is working rather than frozen.
