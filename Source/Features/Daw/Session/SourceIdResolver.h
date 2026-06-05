#pragma once
//==============================================================================
// PRD-0097: SourceIdResolver — resolves every clip's stable `sourceFileId` to a
// readable audio source on session open, BEFORE the EPIC-0010 arrangement
// snapshot is (re)compiled and before playback/export is allowed.
//
// Resolution is per **source**, not per clip: one track chopped into a dozen
// timeline regions shares one `sourceFileId`, so each distinct id is resolved
// exactly once and a single relocation rebinds every clip that references it
// (§1.5.6). Each source resolves through an ordered strategy (§1.5.1):
//     1. library DB id lookup (EPIC-0004)
//     2. stored last-known path  (PRD-0095 relocation hint)
//     3. content-hash match against the library
// The first readable result wins; a source that exhausts all strategies is
// marked Missing and every clip sharing it is flagged together (never dropped).
//
// The resolver is pure logic over the `daw` ValueTree + the SOURCE_REFS hint
// table. All external lookups (DB, filesystem, stem cache) are injected as
// std::function strategies so the resolver is fully testable headless and never
// touches disk or the audio thread directly. Message/background thread only.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <optional>
#include <vector>

#include "SessionSchema.h"

namespace Daw::Session
{
    //--------------------------------------------------------------------------
    // The three source kinds, each resolved through a kind-appropriate path
    // (§1.5.5). Inferred from the SOURCE_REF `sourceKind`, falling back to the
    // id prefix ("lib:" / "stem:" / "ext:") when no hint exists.
    //--------------------------------------------------------------------------
    enum class SourceKind { Library, External, StemCache };

    enum class ResolutionState { Resolved, Missing };

    //--------------------------------------------------------------------------
    // One entry per DISTINCT sourceFileId — the unit the batch-resolution UI
    // (PRD-0097 §1.3.2) lists and the unit a relocation rebinds.
    //--------------------------------------------------------------------------
    struct ResolvedSource
    {
        juce::String     sourceFileId;
        SourceKind       kind          { SourceKind::External };
        juce::String     displayName;          // for the relocation UI
        juce::String     lastKnownPath;         // broken hint, shown to the DJ
        juce::String     parentTrackId;         // StemCache: the parent (for re-derive)
        ResolutionState  state         { ResolutionState::Missing };
        juce::String     resolvedPath;          // bound absolute path when Resolved
        int              clipCount     { 0 };    // how many clips reference this source
    };

    //==========================================================================
    class SourceIdResolver
    {
    public:
        //----------------------------------------------------------------------
        // Injected resolution strategies. Each returns a readable absolute path,
        // or nullopt when it cannot satisfy the source.
        //----------------------------------------------------------------------
        struct Strategies
        {
            // (1) Library DB lookup by stable id (EPIC-0004).
            std::function<std::optional<juce::String> (const juce::String& sourceFileId)> libraryById;
            // (2) Cheap existence check for the stored last-known path.
            std::function<bool (const juce::String& path)> pathExists
                = [] (const juce::String& p) { return juce::File (p).existsAsFile(); };
            // (3) Content-hash match against the library (most expensive, last).
            std::function<std::optional<juce::String> (const juce::String& sourceFileId)> hashMatch;
            // StemCache: readable cached-stem path for a resolved parent track id.
            std::function<std::optional<juce::String> (const juce::String& parentTrackId)> stemByParentId;
        };

        explicit SourceIdResolver (Strategies strategies);

        //----------------------------------------------------------------------
        // Collects the set of DISTINCT sourceFileIds across all clips in `daw`,
        // resolves each exactly once through the ordered strategy, and returns
        // one ResolvedSource per distinct id (with its clip count). `sourceRefs`
        // is the PRD-0095 SOURCE_REFS hint table (may be invalid/empty).
        //----------------------------------------------------------------------
        std::vector<ResolvedSource> resolve (const juce::ValueTree& daw,
                                             const juce::ValueTree& sourceRefs) const;

        //----------------------------------------------------------------------
        // Resolves a single source descriptor through the ordered strategy,
        // mutating its `state`/`resolvedPath`. Exposed so the host can re-resolve
        // one source after a relocation/re-derivation without a full pass.
        //----------------------------------------------------------------------
        void resolveOne (ResolvedSource& src) const;

        //----------------------------------------------------------------------
        // Binds a user-chosen replacement path to a source (the relocation
        // result). Because clips reference the source by id, this single rebind
        // applies to ALL clips referencing it (§1.5.6). Returns true if `path`
        // is readable.
        //----------------------------------------------------------------------
        bool applyRelocation (ResolvedSource& src, const juce::String& path) const;

        //----------------------------------------------------------------------
        // Gating helpers (§1.5.7): playback and export are blocked while any
        // source is Missing.
        //----------------------------------------------------------------------
        static bool allResolved (const std::vector<ResolvedSource>& sources) noexcept;
        static std::vector<ResolvedSource> missingOnly (const std::vector<ResolvedSource>& sources);
        static int missingCount (const std::vector<ResolvedSource>& sources) noexcept;

        //----------------------------------------------------------------------
        // The set of sourceFileIds that resolved — the host compiles the
        // EPIC-0010 snapshot for clips whose source is in this set only, so the
        // engine never references an unreadable source (§1.4).
        //----------------------------------------------------------------------
        static juce::StringArray resolvedSourceIds (const std::vector<ResolvedSource>& sources);

        // Classifies a source kind from its hint / id prefix.
        static SourceKind classifyKind (const juce::String& sourceFileId,
                                        const juce::String& kindHint);

    private:
        Strategies strategies;
    };
}
