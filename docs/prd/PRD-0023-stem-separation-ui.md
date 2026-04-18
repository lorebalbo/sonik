---
status: Not Implemented
epic: EPIC-0002
depends-on:
  - PRD-0020
  - PRD-0021
  - PRD-0005
---

# 1. PRD-0023: Stem Separation UI

## 1.1. Problem

The stem separation engine (PRD-0020) and the stem-aware playback system (PRD-0021) operate entirely in the backend. Without a UI, the user has no way to trigger stem separation, see its progress, or toggle individual stems on and off during playback. The DJ needs clear, instant visual controls — conforming to Sonik's 1-bit monochrome design language — that communicate the stem system's state at a glance: whether stems are available, processing, ready, or in error.

## 1.2. Objective

The system provides a stem separation UI that:
- Displays a "STEMS" button on each deck that triggers stem separation for the loaded track, disabled when no track is loaded or separation is already in progress.
- Shows a progress indicator during separation processing (0-100%), with the ability to cancel.
- Displays two stem toggle buttons ("VOC" and "INST") that appear once stems are ready, allowing the user to mute/unmute vocals and instrumentals in real-time.
- Provides immediate visual feedback: active (unmuted) toggles are fully opaque; muted toggles are dimmed; processing state shows a progress fill.
- Conforms strictly to DESIGN.md: monochrome palette (`#000000` / `#F9F9F9`), zero border-radius, instant state inversion on toggle, no gradients or shadows.
- Auto-enables stem toggles when cached stems are detected for a loaded track (no user action required).

## 1.3. User Flow

1. The user loads a track onto Deck A. The `StemSeparationManager` automatically checks the stem cache for this track (PRD-0020). **Cache hit**: the Stems node status transitions to `"loading_cached"` then `"ready"` automatically. The "STEMS" button appears in active style and the "VOC"/"INST" toggles appear immediately — no user action required. Skip to step 5. **Cache miss**: the Stems node status remains `"none"`. The "STEMS" button is visible in the deck's control strip (below the SLIP button), rendered in the inactive style (surface fill, dark text). The "VOC" and "INST" toggles are hidden.
2. The user clicks the "STEMS" button. The UI calls `StemSeparationManager::startSeparation(deckId)`. The Stems node status transitions to `"separating"` and progress to `0.0`. The "STEMS" button label changes to show progress (e.g., "37%") and fills from left to right with the primary color as a progress bar. The button is no longer clickable for starting a new separation, but a click now cancels the running job.
3. Progress updates arrive via ValueTree listener (`IDs::progress` changes). The button repaints to reflect the current percentage. Updates arrive at least once per second.
4. Separation completes. The Stems node status transitions to `"ready"`. The "STEMS" button returns to its default label "STEMS" and switches to the active style (primary fill, surface text) to indicate stems are available. The "VOC" and "INST" toggle buttons appear adjacent to the "STEMS" button.
5. The user clicks "VOC" to mute vocals. The toggle switches to the muted style (dimmed, surface fill with reduced opacity text). The audio engine begins fading out the vocals stem. The user sees the visual state change instantly and hears vocals fade over ~1.5 ms.
6. The user clicks "VOC" again to unmute. The toggle switches back to active style. Vocals fade back in.
7. The user clicks "INST" to mute instrumentals. The toggle switches to muted style. Drums, bass, and other stems are muted in the audio engine simultaneously.
8. Both "VOC" and "INST" can be muted simultaneously. When both are muted, the deck outputs silence (all stems muted). The UI shows both toggles in dimmed state.
9. **Cache hit**: the user loads a track that was previously separated. The Stems node status transitions directly to `"loading_cached"` (brief) then `"ready"`. The "STEMS" button appears in active style and the "VOC"/"INST" toggles appear immediately — no progress phase. This is handled automatically on track load (see step 1).
10. **Cancellation**: during separation, the user clicks the "STEMS" button (which is now in progress mode). This calls `StemSeparationManager::cancelSeparation(deckId)`. The Stems node status returns to `"none"`. The button reverts to inactive style. The "VOC"/"INST" toggles remain hidden.
11. **Error**: separation fails (model error, disk full). The Stems node status transitions to `"error"`. The "STEMS" button shows "ERR" with a dithered error pattern and the `stemError` text is displayed as a tooltip on hover. After 3 consecutive failures for the same track, the button enters a persistent error state showing "ERR" and requires the user to load a different track or restart the app.
12. **Model unavailable**: the model file is missing from `~/Library/Application Support/Sonik/Models/`. The Stems node status is `"model_unavailable"`. The "STEMS" button shows "NO MODEL" at reduced opacity. Clicking it displays a tooltip with the expected model file path. No separation is attempted.
13. **Queued**: the user starts separation on Deck B while Deck A is already separating. The Stems node status on Deck B transitions to `"queued"`. The "STEMS" button shows "WAIT" in a static style (no progress fill) to indicate the job is queued, not stalled. When Deck A finishes, Deck B's job begins and its status transitions to `"separating"` with progress.
14. **Stems toggle off**: the user clicks the "STEMS" button while in `"ready"` state. This toggles stems off — `clearDeckStemBuffers()` is called (PRD-0021), playback reverts to single-buffer mode, and the "VOC"/"INST" toggles disappear. The "STEMS" button returns to inactive style. Clicking again re-enables stems from cache (instant).
15. **Track eject**: the user ejects the track. All stem UI resets: "STEMS" button returns to inactive state, "VOC"/"INST" toggles disappear, Stems node resets.
16. **Empty deck**: when no track is loaded, the "STEMS" button is visible but at 30% alpha (matching the established disabled-component convention). Clicking it does nothing.
17. **Short track**: if the loaded track is shorter than 5 seconds, the "STEMS" button is visible but disabled (30% alpha) with a tooltip: "Track too short for stem separation."

## 1.4. Acceptance Criteria

- [ ] A `StemSeparateButton` component is added to each deck, placed in the control strip sidebar of `DeckShellComponent`, below the existing SLIP button and above the pitch fader. Stem controls are a playback modifier, like slip and key lock, and belong in the same visual group.
- [ ] The "STEMS" button follows the established toggle pattern: extends `juce::Component` + `juce::ValueTree::Listener`, takes `deckTree` via constructor, listens for Stems node changes.
- [ ] **Inactive state** (`status == "none"`): surface fill (`#E2E2E2`), dark text "STEMS" at ~50% opacity, black 1px border, zero border-radius.
- [ ] **Processing state** (`status == "separating"`): the button acts as a progress bar — a left-to-right fill of primary color (`#000000`) proportional to `IDs::progress` (0.0 to 1.0), with the label showing the integer percentage (e.g., "37%"). Text color inverts based on fill position (white text over filled area, dark text over unfilled). A click in this state triggers cancellation.
- [ ] **Ready state** (`status == "ready"`): primary fill (`#000000`), surface text (`#F9F9F9`) "STEMS", black 1px border. Indicates stems are loaded and toggles are available.
- [ ] **Error state** (`status == "error"`): shows "ERR" with a dithered/stippled background pattern (per DESIGN.md warning states). The `stemError` text from the ValueTree is displayed as a tooltip on hover via `setTooltip()`. After 3 consecutive errors for the same track (tracked by a counter on the component), the button enters a persistent error state and does not allow retry until the track is reloaded.
- [ ] **Model unavailable state** (`status == "model_unavailable"`): shows "NO MODEL" at reduced opacity (50% alpha). Clicking displays a tooltip with the expected path: `"Place htdemucs.onnx in ~/Library/Application Support/Sonik/Models/"`. No separation is triggered.
- [ ] **Queued state** (`status == "queued"`): shows "WAIT" with surface fill and dark text (same as inactive but with "WAIT" label). No progress fill animation. A click in this state cancels the queued job.
- [ ] **Loading cached state** (`status == "loading_cached"`): shows the active style (primary fill, surface text "STEMS") immediately, since cache loading is expected to complete in under 1 second.
- [ ] **Empty deck**: button rendered at 30% alpha, click is a no-op.
- [ ] **Short track** (< 5 seconds): button rendered at 30% alpha with tooltip: "Track too short for stem separation." Click is a no-op.
- [ ] Two `StemToggleComponent` instances ("VOC" and "INST") are created per deck. They are only visible when `status == "ready"`.
- [ ] The "VOC" toggle controls `IDs::vocalsMuted` on the Stems ValueTree node. Active (unmuted): primary fill, surface text "VOC". Muted: surface fill (`#E2E2E2`), dark text at 50% opacity.
- [ ] The "INST" toggle controls `IDs::drumsMuted`, `IDs::bassMuted`, and `IDs::otherMuted` simultaneously. Active (unmuted): primary fill, surface text "INST". Muted: same dimmed style. Tooltip on hover: "Mutes drums, bass, and other non-vocal elements."
- [ ] When the 4-stem UI ships in a future iteration, the 2-toggle view (VOC/INST) will be entirely replaced — not augmented. The INST toggle is a convenience shortcut and will not coexist with individual per-stem toggles.
- [ ] When the user clicks "INST" to mute, all three properties (`drumsMuted`, `bassMuted`, `otherMuted`) are set to `true` in a single ValueTree transaction. When unmuting, all three are set to `false`.
- [ ] Toggle state changes trigger `repaint()` via `callAsync` with `SafePointer` (matching established component pattern).
- [ ] `mouseDown()` on a toggle is guarded: no-op if stems are not ready or if deck is empty.
- [ ] Clicking the "STEMS" button while in `"ready"` state toggles stems off: calls `clearDeckStemBuffers()` (PRD-0021), hides "VOC"/"INST" toggles, and returns the button to inactive style. Clicking again re-enables stems from cache (instant transition to `"loading_cached"` → `"ready"`).
- [ ] All three components (STEMS, VOC, INST) are laid out in the control strip sidebar of `DeckShellComponent`. The "STEMS" button is placed directly below the SLIP button (24px height, `.reduced(12, 2)` matching existing toggles). The "VOC" and "INST" toggles are placed below "STEMS" (each 24px height, same reduced insets), visible only when stems are `"ready"`.
- [ ] `DeckShellComponent::resized()` is updated to allocate space for the 3 stem components (STEMS, VOC, INST) in the control strip sidebar, below the SLIP button.
- [ ] The stem controls are always present in the layout (even when stems are not ready) to prevent layout shifts. When stems are not ready, only the "STEMS" button is visible; "VOC" and "INST" are hidden (zero height).
- [ ] Font: `FontOptions(11.0f).withStyle("Bold")`, matching the existing toggle button convention.
- [ ] All `paint()` implementations use `juce::Colours` or hex color constants consistent with DESIGN.md: primary `#000000`, surface `#F9F9F9`, inactive fill `#E2E2E2`.
- [ ] No transitions or animations on state changes — instant repaint (per DESIGN.md: "zero-latency visual feedback").
- [ ] The error dithered pattern uses the `Random::getSystemRandom()` stipple approach defined in DESIGN.md for warning states.
- [ ] All new UI code resides under `Source/Features/StemSeparation/UI/`.
- [ ] The `DeckShellComponent` test expectations (child count in `TrackInfoTests`) are updated to account for the new stem separation components.