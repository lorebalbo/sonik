#pragma once
//==============================================================================
// PRD-0041: Audio-thread-facing view of the MIDI bridge.
//
// `MidiMessageBridge` lives in Source/Features/Midi/ and owns the FIFO.
// The audio engine includes ONLY this header to gain the ability to drain
// the FIFO each callback. It re-exports MidiMessageBridge.h so the audio
// engine has the full type needed to call `drainAudioThreadFifo`.
//
// Architectural rule (CLAUDE.md FSD): a feature module may include this
// header to reach the bridge. The bridge itself does not include anything
// from Features/AudioEngine/, Features/Deck/, Features/Mixer/, or
// Features/Library/.
//==============================================================================

#include "../Midi/MidiMessageBridge.h"
