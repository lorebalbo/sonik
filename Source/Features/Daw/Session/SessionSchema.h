#pragma once
//==============================================================================
// PRD-0095: Session Schema — the canonical on-disk structure of a
// `.soniksession` project document.
//
// A `.soniksession` is a single versioned juce::ValueTree serialized to XML
// (see SessionSerializer). Its shape:
//
//   SONIK_SESSION                                  ← root (this file's identifiers)
//     property: schemaVersion      (int)           ← writer's source of truth
//     property: projectSampleRate  (double)
//     property: appVersion         (string)
//     property: savedAtUtc         (string, ISO-8601)
//     ├── MASTER_GRID                               ← tempo / transport reference
//     │     property: bpm                (double)
//     │     property: downbeatSample     (int64)
//     │     property: timeSigNumerator   (int)
//     │     property: timeSigDenominator (int)
//     │     property: playheadSample     (int64)    ← arrangement state, not chrome
//     │     property: loopStartSample    (int64)
//     │     property: loopEndSample      (int64)
//     │     property: loopEnabled        (bool)
//     ├── VIEW_STATE                                ← portable view chrome (all optional)
//     │     property: zoomSamplesPerPixel (double)
//     │     property: scrollStartSample   (int64)
//     │     property: selectedClipId      (string)
//     ├── SOURCE_REFS                               ← relocation hints (PRD-0097)
//     │     └── SOURCE_REF (one per distinct sourceFileId)
//     │           property: sourceFileId   (string)
//     │           property: lastKnownPath  (string)
//     │           property: displayName    (string)
//     │           property: sourceKind     (string: Library/External/StemCache)
//     └── Daw                                       ← VERBATIM structural copy of the
//                                                     live `daw` ValueTree (PRD-0063):
//                                                     tracks → lanes → clips, plus the
//                                                     `automation` subtree (PRD-0087).
//
// The `Daw` child is a *structural copy*: the serializer never re-shapes it, so
// every lane, every clip (all eight DawClip fields), and every automation
// lane/breakpoint is persisted by structural identity, and any unknown node or
// attribute written by a newer build round-trips untouched (§1.5.6).
//
// Audio is referenced by stable id only (DawClip::sourceFileId). No sample data
// is ever embedded; a session referencing a multi-gigabyte FLAC is a few KB of
// XML.
//
// THREADING: this header declares identifiers only and touches no audio-thread
// path. All serialization runs on a message/background thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../State/DawState.h"   // DawIDs::Daw + DawState::kProjectSampleRate

namespace Daw::Session
{
    //--------------------------------------------------------------------------
    // The single source of truth for the version stamped onto every saved file.
    // A document loaded at version N is migrated forward to this value by
    // SessionMigrator before it is handed to the caller. Bumping this constant
    // and adding exactly one migration step is the entire cost of a schema
    // change (§1.5.2).
    //--------------------------------------------------------------------------
    inline constexpr int kCurrentSchemaVersion = 1;

    //--------------------------------------------------------------------------
    // The canonical project file extension (leading dot included). `save`
    // normalises any target to this extension (§1.5.7); `load` does NOT enforce
    // it — content is authoritative, tolerating user renames.
    //--------------------------------------------------------------------------
    inline const juce::String kSessionFileExtension { ".soniksession" };

    //--------------------------------------------------------------------------
    // MIME type fixed here for downstream OS file-type registration (PRD-0096).
    //--------------------------------------------------------------------------
    inline const juce::String kSessionMimeType { "application/vnd.sonik.session+xml" };

    namespace IDs
    {
        #define DECLARE_SESSION_ID(name) const juce::Identifier name (#name);

        // ---- Root node + its properties -------------------------------------
        DECLARE_SESSION_ID (SONIK_SESSION)      // document root type
        DECLARE_SESSION_ID (schemaVersion)      // int, writer's source of truth
        DECLARE_SESSION_ID (projectSampleRate)  // double
        DECLARE_SESSION_ID (appVersion)         // string (build that wrote it)
        DECLARE_SESSION_ID (savedAtUtc)         // string, ISO-8601 UTC

        // ---- MASTER_GRID node + its properties ------------------------------
        DECLARE_SESSION_ID (MASTER_GRID)        // tempo / transport reference node
        DECLARE_SESSION_ID (bpm)                // double
        DECLARE_SESSION_ID (downbeatSample)     // int64
        DECLARE_SESSION_ID (timeSigNumerator)   // int
        DECLARE_SESSION_ID (timeSigDenominator) // int
        DECLARE_SESSION_ID (playheadSample)     // int64 (arrangement state)
        DECLARE_SESSION_ID (loopStartSample)    // int64
        DECLARE_SESSION_ID (loopEndSample)      // int64
        DECLARE_SESSION_ID (loopEnabled)        // bool

        // ---- VIEW_STATE node + its properties (all optional on load) --------
        DECLARE_SESSION_ID (VIEW_STATE)         // portable view chrome node
        DECLARE_SESSION_ID (zoomSamplesPerPixel)// double
        DECLARE_SESSION_ID (scrollStartSample)  // int64
        DECLARE_SESSION_ID (selectedClipId)     // string (canonical clip Uuid)

        // ---- SOURCE_REFS node + child SOURCE_REF (relocation hints) ---------
        DECLARE_SESSION_ID (SOURCE_REFS)        // container, child of root
        DECLARE_SESSION_ID (SOURCE_REF)         // one per distinct sourceFileId
        DECLARE_SESSION_ID (sourceFileId)       // string (matches DawClip::sourceFileId)
        DECLARE_SESSION_ID (lastKnownPath)      // string absolute path hint
        DECLARE_SESSION_ID (displayName)        // string, for the relocation UI
        DECLARE_SESSION_ID (sourceKind)         // string: Library / External / StemCache

        #undef DECLARE_SESSION_ID

        //----------------------------------------------------------------------
        // The `Daw` child of the session root is a verbatim copy of the live
        // `daw` branch. Its type identifier is therefore DawIDs::Daw, re-exported
        // here so call-sites can write `root.getChildWithName (IDs::DAW)`.
        //----------------------------------------------------------------------
        inline const juce::Identifier& DAW() noexcept { return DawIDs::Daw; }
    }

    //--------------------------------------------------------------------------
    // Source-kind discriminator persisted in SOURCE_REF (PRD-0097 resolves
    // each kind through a kind-appropriate strategy).
    //--------------------------------------------------------------------------
    namespace SourceKindStrings
    {
        inline constexpr const char* kLibrary   = "Library";
        inline constexpr const char* kExternal  = "External";
        inline constexpr const char* kStemCache = "StemCache";
    }
}
