#pragma once
//==============================================================================
// PRD-0069: Daw projection SourceModeReader.
//
// A thin, MESSAGE-THREAD-ONLY adapter that the live-deck projection bridge uses
// to learn which lane(s) a deck is currently audible on. It re-exports PRD-0062's
// published source-mode contract (the deck's `sourceMode` ValueTree property,
// read via the existing ::SourceModeReader) and derives the per-stem audibility
// from the deck's audio-thread-published mute atomics (DeckAudioState), using
// acquire-ordered loads. It never reaches into PRD-0062's internals and never
// writes any deck state.
//
// THREADING: query on the message thread (the only thread that mutates the deck
// tree). The atomic mute loads are safe from any thread; acquire ordering pairs
// with the audio engine's release stores.
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "../../Deck/SourceModeReader.h"   // PRD-0062 published contract
#include "../../Deck/AudioThreadState.h"   // DeckAudioState mute atomics

namespace Daw
{

enum class ProjectionMode
{
    Original,   // deck plays the pristine original source
    Stems       // deck plays separated stems (instrumental + vocal)
};

//------------------------------------------------------------------------------
// The active source mode plus per-lane audibility for one deck, at one instant.
//   * Original mode               -> original audible; inst/vocal not drawn.
//   * Stems, both audible          -> instrumental + vocal.
//   * Stems, one muted             -> only the audible stem's lane.
//   * Stems, both muted            -> nothing audible (no lane drawn, §1.5.5).
//------------------------------------------------------------------------------
struct LaneAudibility
{
    ProjectionMode mode         = ProjectionMode::Original;
    bool           original     = true;
    bool           instrumental = false;
    bool           vocal        = false;
};

//------------------------------------------------------------------------------
// Resolves the active source mode from the deck ValueTree (PRD-0062) and the
// per-lane audibility from the deck's mute atomics (acquire loads). The "INST"
// (summed instrumental) lane is audible unless all three of drums/bass/other are
// muted; the "VOCAL" lane is audible unless vocals are muted.
//------------------------------------------------------------------------------
inline LaneAudibility resolveAudibility (const juce::ValueTree& deckTree,
                                         const DeckAudioState&  audio)
{
    LaneAudibility a;

    ::SourceModeReader reader (deckTree);
    if (! reader.isStems())
    {
        a.mode         = ProjectionMode::Original;
        a.original     = true;
        a.instrumental = false;
        a.vocal        = false;
        return a;
    }

    a.mode     = ProjectionMode::Stems;
    a.original = false;

    const bool vocalsMuted = audio.stemVocalsMuted.load (std::memory_order_acquire);
    const bool drumsMuted  = audio.stemDrumsMuted .load (std::memory_order_acquire);
    const bool bassMuted   = audio.stemBassMuted  .load (std::memory_order_acquire);
    const bool otherMuted  = audio.stemOtherMuted .load (std::memory_order_acquire);

    const bool instMuted = drumsMuted && bassMuted && otherMuted;

    a.instrumental = ! instMuted;
    a.vocal        = ! vocalsMuted;
    return a;
}

} // namespace Daw
