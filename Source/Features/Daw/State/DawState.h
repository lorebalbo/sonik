#pragma once
//==============================================================================
// PRD-0063: DAW State Schema — identifier constants, project-rate constants,
// and the message-thread-only factory/helpers that build the non-destructive
// `daw` branch of the central juce::ValueTree.
//
// Tree shape (one juce::Identifier per level, declared below):
//   SonikState
//     └── Daw                              ← type "Daw"  (parallel to Decks/Mixer)
//           └── tracks                     ← type "tracks"  (container)
//                 └── track                ← type "track"   (one per deck, lazy)
//                       property: deckIndex (int, stable)
//                       └── lanes          ← type "lanes"   (container, eager)
//                             └── lane     ← type "lane"    (exactly 3: Original,
//                                                            Instrumental, Vocal)
//                                   property: laneId   (juce::Uuid canonical str)
//                                   property: laneKind (string enum)
//                                   └── clips           ← type "clips" (container)
//                                         └── clip      ← type "clip" (DawClip)
//
// THREADING: every helper here runs on the MESSAGE THREAD only. The branch is
// observed via juce::ValueTree::Listener. No field is read/written from the
// audio thread (no processBlock access, no atomics, no locks).
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace DawIDs
{
    #define DECLARE_DAW_ID(name) const juce::Identifier name (#name);

    // ---- Tree type identifiers -------------------------------------------
    DECLARE_DAW_ID (Daw)        // top-level branch, parallel to Decks / Mixer
    DECLARE_DAW_ID (tracks)     // container of per-deck channel groups
    DECLARE_DAW_ID (track)      // one channel group per deck
    DECLARE_DAW_ID (lanes)      // container of the three lanes
    DECLARE_DAW_ID (lane)       // a single lane (Original / Instrumental / Vocal)
    DECLARE_DAW_ID (clips)      // container of clip nodes within a lane
    DECLARE_DAW_ID (clip)       // a single non-destructive clip node (DawClip)

    // ---- Track properties -------------------------------------------------
    DECLARE_DAW_ID (deckIndex)  // int, stable id of the owning deck

    // ---- Lane properties --------------------------------------------------
    DECLARE_DAW_ID (laneId)     // juce::Uuid canonical string, minted once
    DECLARE_DAW_ID (laneKind)   // string enum: "Original"/"Instrumental"/"Vocal"

    #undef DECLARE_DAW_ID
}

namespace DawState
{
    //--------------------------------------------------------------------------
    // Project sample-rate: the single unit every *Sample field in the `daw`
    // branch is expressed against (PRD-0063 §1.5.4). Reconciling a source's
    // native rate to this is an EPIC-0010 (playback/resampling) obligation.
    // Aligned with the codebase-wide canonical analysis/engine rate.
    //--------------------------------------------------------------------------
    static constexpr double kProjectSampleRate = 44100.0;

    //--------------------------------------------------------------------------
    // Musical grid constant. Declared here now; the master-grid service
    // (PRD-0064) reuses it. 4 beats per bar (4/4).
    //--------------------------------------------------------------------------
    static constexpr int kBeatsPerBar = 4;

    //--------------------------------------------------------------------------
    // Builds a fresh, standalone `Daw` branch: a "Daw" tree carrying an empty
    // "tracks" container. No tracks are created here (tracks are lazy, see
    // §1.5.3). Message-thread only.
    //--------------------------------------------------------------------------
    juce::ValueTree createDawBranch();

    //--------------------------------------------------------------------------
    // Idempotently attaches a `Daw` branch to the supplied root state tree
    // (mirrors MixerStateSchema's get-or-create). If a "Daw" child already
    // exists it is reused (and its "tracks" container ensured); otherwise a
    // new one is created via createDawBranch() and added. Returns the branch.
    // Message-thread only.
    //--------------------------------------------------------------------------
    juce::ValueTree getOrCreateDawBranch (juce::ValueTree rootState);

    //--------------------------------------------------------------------------
    // Idempotently creates the channel-group `track` for `deckIndex` if it does
    // not already exist, pre-populating its three lanes (Original, Instrumental,
    // Vocal) via the ChannelGroup factory. Calling twice for the same deck is a
    // no-op: it returns the existing track without duplicating lanes or clips.
    // Returns the track node. Message-thread only.
    //--------------------------------------------------------------------------
    juce::ValueTree ensureTrackForDeck (juce::ValueTree dawBranch, int deckIndex);

    //--------------------------------------------------------------------------
    // Looks up an existing track node by deck index. Returns an invalid tree if
    // no track has been created for that deck yet.
    //--------------------------------------------------------------------------
    juce::ValueTree findTrackForDeck (juce::ValueTree dawBranch, int deckIndex);

    //--------------------------------------------------------------------------
    // Returns the timeline start (project-rate samples) of the earliest playable
    // clip in the arrangement, or 0 when there are no clips. EPIC-0010: used to
    // seed the DawTransport origin so Play starts on the recorded content rather
    // than at sample 0. The arrangement is anchored at the master-grid phase
    // origin (PRD-0069), which is non-zero whenever a master deck drives the
    // grid, so a fixed sample-0 origin would play silence up to the first clip.
    // Clips flagged missingSource (PRD-0097) are skipped: they are silent and
    // never enter the engine snapshot, so they must not define the start.
    // Message-thread only.
    //--------------------------------------------------------------------------
    std::int64_t earliestClipStartSample (const juce::ValueTree& dawBranch);
}
