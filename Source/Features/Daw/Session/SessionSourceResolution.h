#pragma once
//==============================================================================
// PRD-0097: SessionSourceResolution — the integration layer that binds the
// GUI-agnostic SourceIdResolver (logic core) to the live application subsystems
// (TrackDatabase, filesystem, StemSeparationManager, the EPIC-0010 recompile
// trigger) and owns the session-scoped resolution lifecycle.
//
// Responsibilities:
//   - Provide the four real SourceIdResolver::Strategies (library-by-id,
//     path-exists, content-hash match, stem-by-parent).
//   - Run the resolution pass at session open (the PRD-0097 SEAM in
//     SonikApplication), BEFORE the EPIC-0010 snapshot recompile, writing the
//     per-clip `missingSource` flag into the live `daw` ValueTree so:
//       * Missing-source clips render with the DESIGN.md "Glitch" treatment.
//       * The ArrangementCompiler excludes them from the engine snapshot.
//   - Maintain the current set of ResolvedSources (one per distinct sourceFileId)
//     so the batch UI can list missing sources and gating can query completeness.
//   - Relocate a Library/External source (reusing PRD-0039 FileChooser + dedup +
//     canonical path-write) and re-derive a StemCache source (EPIC-0002), each
//     applying to ALL clips that reference the source (§1.5.6), clearing the
//     flag, and triggering a recompile that re-admits the clips.
//   - Build the SOURCE_REFS relocation-hint table at save time so a reopened
//     session carries kind + last-known path for every distinct source.
//
// Gating (§1.5.7): areAllSourcesResolved() / missingSourceCount() are the single
// queries the play/export entry points consult.
//
// THREADING: message thread for orchestration; the DB query, filesystem checks,
// and the stem re-derivation kick-off all run off the audio thread. The audio
// thread only ever sees Resolved clips via the EPIC-0010 snapshot.
//==============================================================================

#include <functional>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "SourceIdResolver.h"

class TrackDatabase;
class StemSeparationManager;

namespace Daw { class ArrangementRecompileTrigger; }

namespace Daw::Session
{
    class SessionSourceResolution
    {
    public:
        //----------------------------------------------------------------------
        // @param dawBranch          The live "Daw" ValueTree branch (single source
        //                           of truth). Flags are written here.
        // @param database           EPIC-0004 library DB (id/hash -> path, relocate).
        // @param stemManager        EPIC-0002 stem separation (re-derivation).
        // @param recompileTrigger   EPIC-0010 trigger to recompile the snapshot
        //                           after the flag set changes (may be null until
        //                           the playback layer is wired; set later).
        //----------------------------------------------------------------------
        SessionSourceResolution (juce::ValueTree dawBranch,
                                 TrackDatabase& database,
                                 StemSeparationManager& stemManager);

        void setRecompileTrigger (ArrangementRecompileTrigger* trigger) noexcept
        {
            recompileTrigger_ = trigger;
        }

        //----------------------------------------------------------------------
        // Run the full resolution pass over the (already-swapped) live daw branch,
        // using `sourceRefs` (the loaded SOURCE_REFS hint table) for kind +
        // last-known path. Writes the per-clip `missingSource` flag and requests a
        // recompile. Call this at the PRD-0097 SEAM on every session open.
        //----------------------------------------------------------------------
        void runResolutionPass (const juce::ValueTree& sourceRefs);

        //----------------------------------------------------------------------
        // Re-run resolution over the live daw branch with no external hint table
        // (e.g. after New, or to clear all flags). Cheap: an empty SOURCE_REFS.
        //----------------------------------------------------------------------
        void clearAndReresolve();

        //----------------------------------------------------------------------
        // Gating queries (§1.5.7) — the single seam play AND export consult.
        //----------------------------------------------------------------------
        bool areAllSourcesResolved() const noexcept;
        int  missingSourceCount()    const noexcept;

        // The current missing sources (one per distinct sourceFileId), for the UI.
        std::vector<ResolvedSource> missingSources() const;

        // The clipIds of every clip whose source is Missing (for play-gating
        // highlight). Derived from the live daw tree's `missingSource` flags.
        juce::StringArray missingClipIds() const;

        //----------------------------------------------------------------------
        // Relocate a Library/External source to `replacementFile` (the PRD-0039
        // FileChooser result). Runs the PRD-0039 canonical-path dedup check and
        // writes the library_tracks row by content hash. On success: rebinds the
        // source, clears the flag on ALL referencing clips, recompiles, and
        // returns true. `dedupRejected` (optional) is set true when the file is
        // already in the library under another row (so the UI can warn).
        //----------------------------------------------------------------------
        bool relocateSource (const juce::String& sourceFileId,
                             const juce::File& replacementFile,
                             bool* dedupRejected = nullptr);

        //----------------------------------------------------------------------
        // Re-derive a StemCache source: re-runs EPIC-0002 separation for the
        // parent track on the existing background thread. The completion callback
        // (message thread) reports success; on success the caller re-resolves +
        // clears flags via reresolveSource(). Returns false synchronously when the
        // parent track itself does not resolve (the UI must relocate the parent
        // first, §1.5.5).
        //----------------------------------------------------------------------
        bool reDeriveStems (const juce::String& sourceFileId,
                            std::function<void (bool success)> onComplete);

        //----------------------------------------------------------------------
        // Re-resolve a single source after an out-of-band fix and, on success,
        // clear the flag on its clips + recompile. Returns the new state's
        // resolved-ness. Used by reDeriveStems' completion.
        //----------------------------------------------------------------------
        bool reresolveSource (const juce::String& sourceFileId);

        //----------------------------------------------------------------------
        // Build the SOURCE_REFS hint table from the live daw branch for the next
        // save (one SOURCE_REF per distinct sourceFileId, carrying kind +
        // last-known path + display name). Message thread only.
        //----------------------------------------------------------------------
        juce::ValueTree buildSourceRefs() const;

        // The resolver's strategies, exposed for testing / reuse.
        const SourceIdResolver& resolver() const noexcept { return resolver_; }

    private:
        // Write the `missingSource` flag onto every clip whose sourceFileId is in
        // `missingIds` (true) and clear it on all others. Single tree pass.
        void applyFlagsToClips (const juce::StringArray& missingIds);

        // Clear the flag on every clip referencing `sourceFileId`.
        void clearFlagForSource (const juce::String& sourceFileId);

        void requestRecompile();

        // Locate the loaded ResolvedSource for an id (nullptr if not tracked).
        ResolvedSource*       findSource (const juce::String& sourceFileId);
        const ResolvedSource* findSource (const juce::String& sourceFileId) const;

        juce::ValueTree              dawBranch_;
        TrackDatabase&               database_;
        StemSeparationManager&       stemManager_;
        ArrangementRecompileTrigger* recompileTrigger_ { nullptr };

        SourceIdResolver             resolver_;
        std::vector<ResolvedSource>  sources_;   // current pass result

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionSourceResolution)
    };
}
