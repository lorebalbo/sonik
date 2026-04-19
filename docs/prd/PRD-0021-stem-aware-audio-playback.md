---
status: Implemented
epic: EPIC-0002
depends-on:
  - PRD-0020
  - PRD-0002
  - PRD-0004
  - PRD-0014
  - PRD-0017
---

# 1. PRD-0021: Stem-Aware Audio Playback

## 1.1. Problem

Once stems have been separated (PRD-0020), the DJ needs to hear the result: toggling "VOC" or "INST" must instantly mute or unmute the corresponding stem group during live playback. This requires the audio engine's real-time `processBlock` — currently designed to read a single stereo buffer per deck — to simultaneously read from multiple stem buffers, apply per-stem gain control (mute/unmute), and sum them before the deck-level gain stage.

This is the highest-risk change in the Stem Separation Epic. The audio thread runs under a hard real-time deadline (< 3 ms at 44.1 kHz / 128 samples). Any memory allocation, lock, or I/O introduced here causes audible dropouts. Every existing feature that interacts with the audio hot path — transport fades, loop crossfades, seeking, slip mode — must continue to work identically when stems are active. If stems are not active, the engine must have zero additional overhead (no extra reads, no extra math).

Without this PRD, separated stems are files on disk with no way to play them.

## 1.2. Objective

The system provides a stem-aware audio playback engine that:
- Reads from N stem buffer pairs (N=4: vocals, drums, bass, other) when stems are active for a deck, applying per-stem mute/unmute gain before summing into a single stereo pair for the deck.
- Falls back to the original single-buffer read path when stems are not active, with zero additional overhead (no extra reads, no extra branches in the fast path).
- Transitions between muted and unmuted states on any stem with a 64-sample crossfade ramp to prevent clicks.
- Maintains full compatibility with all existing audio features: transport play/pause/stop, seeking, fade-in/fade-out ramps, loop wrap-back with crossfade, slip mode shadow playhead, cue preview, and end-of-track handling.
- Accepts stem buffers delivered from PRD-0020 via an atomic swap mechanism that prevents the audio thread from seeing a partially-updated set of stem pointers.
- Retains the original (non-stem) audio buffer in memory alongside the stem buffers. The original buffer is NOT released when stems activate — this avoids a complex re-decode flow if stems are toggled off and ensures instant revert to single-buffer mode.
- Applies a 64-sample crossfade when transitioning from original-buffer playback to stem-buffer playback (and vice versa) to mask any subtle numerical differences between the original signal and the sum of stems.
- Provides the per-stem mute/unmute state to the audio thread via `std::atomic` fields in `DeckAudioState`, synced from the ValueTree by `AudioStateSync`.

## 1.3. User Flow

1. The user has a track loaded on Deck A playing normally (single buffer mode). All existing playback behavior works as before — the audio engine reads from `channelL`/`channelR` and applies gain, fades, loops, and time stretching exactly as it does today.
2. Stem separation completes (PRD-0020). The `StemSeparationManager` delivers 4 `AudioBufferHolder::Ptr` stem buffers to the audio engine via a message-thread call.
3. The audio engine stores the 4 stem buffer pairs in the `DeckAudioSource` and atomically sets a `stemsActive` flag. On the next `processBlock` cycle, the engine applies a 64-sample crossfade from the original buffer to the summed stem output. Initially all stems are unmuted — the output is perceptually equivalent to the original signal. The user hears no audible change.
4. The user presses the "VOC" toggle to mute vocals. The UI sets `vocalsMuted = true` on the Stems ValueTree node. `AudioStateSync` writes `stemVocalsMuted.store(true)` atomically. On the next `processBlock` cycle, the audio thread reads this flag and begins a 64-sample fade-out ramp on the vocals stem. The user hears vocals smoothly disappear over ~1.5 ms.
5. The user presses "VOC" again to unmute. The reverse happens: a 64-sample fade-in ramp brings vocals back. No click or pop.
6. The user presses "INST" to mute instrumentals. Since "INST" maps to 3 stems (drums + bass + other), the UI sets `drumsMuted`, `bassMuted`, and `otherMuted` to true simultaneously. All three stems begin fading out. The user hears only the vocals (acapella).
7. While stems are active, the user uses the loop controls. The loop crossfade logic reads from each stem buffer's "old continuation" position independently and blends correctly. The user hears a click-free loop with stems.
8. While stems are active, the user seeks to a new position. The fade-out/fade-in seek mechanism applies to the summed stem output. The playhead jumps to the new position across all stem buffers simultaneously (they share one playhead). The user hears a smooth transition.
9. The user activates slip mode and triggers a hot cue while stems are active. The primary playhead jumps; the shadow playhead continues advancing. On slip return, the fade-out/fade-in mechanism applies to the summed stem output. All stem buffers are read from the shadow position after the return.
10. The user loads a new track onto the deck. The stem buffers are released, `stemsActive` is set to false, all stem mute states reset to `false` (unmuted), and the engine reverts to single-buffer mode. The Stems node status resets to `"none"`.

## 1.4. Acceptance Criteria

- [ ] `DeckAudioSource` is extended with 4 stem buffer pointer pairs: `stemChannelL[4]` / `stemChannelR[4]` (each `std::atomic<const float*>`, initialized to `nullptr`) and `stemBufferNumFrames[4]` (`std::atomic<int64_t>`).
- [ ] `DeckAudioSource` has a `stemsActive` (`std::atomic<bool>`, default `false`) flag that the audio thread reads to choose between single-buffer and multi-stem read paths.
- [ ] `DeckAudioSource` holds 4 `AudioBufferHolder::Ptr` (`stemBufferHolders[4]`) on the message thread for ref-counted ownership of stem buffers.
- [ ] `AudioEngine` exposes `setDeckStemBuffers(deckId, vocals, drums, bass, other)` and `clearDeckStemBuffers(deckId)` methods, callable only from the message thread.
- [ ] `setDeckStemBuffers()` stores the 4 buffer holders, extracts raw float pointers with `memory_order_release` stores, and sets `stemsActive = true`. The original buffer holder is retained in memory (NOT released) to allow instant revert to single-buffer mode.
- [ ] `clearDeckStemBuffers()` sets `stemsActive = false`, nullifies all stem channel pointers, and releases stem buffer holders. The original buffer remains valid and playback reverts to single-buffer mode.
- [ ] `DeckAudioState` is extended with 4 stem mute atomics: `stemVocalsMuted`, `stemDrumsMuted`, `stemBassMuted`, `stemOtherMuted` (all `std::atomic<bool>`, default `false`).
- [ ] `AudioStateSync` maps ValueTree properties `IDs::vocalsMuted`, `IDs::drumsMuted`, `IDs::bassMuted`, `IDs::otherMuted` to the corresponding `DeckAudioState` atomics.
- [ ] In `processBlock`, when `stemsActive == false`, the read path is **identical** to the current implementation — no additional branches, no additional reads. Zero overhead.
- [ ] In `processBlock`, when `stemsActive == true`, the engine reads from all 4 stem buffer pairs at the current playhead position, applies per-stem gain (0.0 for muted, 1.0 for unmuted), and sums into a single `rawL`/`rawR` pair before the existing gain/fade/metering logic.
- [ ] Per-stem mute/unmute transitions use a 64-sample crossfade ramp per stem. Each stem has its own fade state (`stemFadeRemaining[4]`, `stemFadeDirection[4]`) stored in `DeckAudioSource` as audio-thread-only fields.
- [ ] The crossfade ramp for stem mute/unmute is independent of the transport fade ramp — both can be active simultaneously (e.g., stem mute during a transport fade-in).
- [ ] All stem buffers share the same playhead (`playheadAccumulator`). There is no independent per-stem playhead.
- [ ] All stem buffers share the same shadow playhead (`shadowPlayheadAccumulator`). Slip mode operates identically with stems.
- [ ] Loop crossfade logic, when stems are active, reads "old continuation" samples from each stem buffer independently and blends them with the loop-start samples per-stem before summing.
- [ ] End-of-track detection uses the same `bufferNumFrames` (all stems have the same length as the original track). The existing end-of-track fade-out applies to the summed stem output.
- [ ] Seeking (including slip-seek, cue return) applies to the shared playhead. All stem buffers are read from the new position after the seek fade completes.
- [ ] Pre-fader metering is computed from the summed stem output (same measurement point as single-buffer mode). No per-stem metering in this PRD.
- [ ] When stems are active and the user ejects/unloads the track, `clearDeckStemBuffers()` is called, reverting to the empty deck state.
- [ ] When stems are active and a new track is loaded onto the same deck, `clearDeckStemBuffers()` is called before the new track's buffer is set.
- [ ] The atomic swap of stem buffers is safe: all 4 stem pointer pairs and `stemsActive` are stored in a sequence that guarantees the audio thread never reads a mix of old and new pointers. The `stemsActive` flag is stored **last** with `memory_order_release` after all channel pointers are set.
- [ ] Pre-allocated scratch buffers for stem reads (e.g., `stemScratchL[4][MAX_STRETCH_BLOCK]`) are added to `DeckAudioSource` if needed for time stretcher integration (PRD-0022), but are not used in this PRD.
- [ ] All changes to `AudioEngine.cpp`'s hot path are verified to contain zero allocations, zero locks, and zero I/O via code review.
- [ ] The stem read path uses linear interpolation (matching the existing vinyl read path) for sub-sample accuracy at non-integer playhead positions.
- [ ] All new code follows the existing memory ordering conventions: `memory_order_release` for message-thread stores, `memory_order_acquire` for audio-thread loads, `memory_order_relaxed` for audio-thread-only fields.
- [ ] Stem mute state is classified as track-specific: all 4 mute atomics (`stemVocalsMuted`, `stemDrumsMuted`, `stemBassMuted`, `stemOtherMuted`) reset to `false` on track load and on track eject. Mute state does not persist across track changes.
- [ ] When transitioning from single-buffer to stem-buffer playback (stemsActive goes from false to true), a 64-sample crossfade blends the original buffer output with the summed stem output to mask any numerical differences. The reverse crossfade applies when stemsActive goes from true to false.
- [ ] Stem solo state (`vocalsSolo`, `drumsSolo`, `bassSolo`, `otherSolo` already in ValueTree) is NOT implemented in this PRD — deferred to 4-stem UI iteration. Only mute is implemented.
- [ ] All changes reside within `Source/Features/AudioEngine/` and `Source/Features/Deck/AudioThreadState.h` / `DeckIdentifiers.h`.