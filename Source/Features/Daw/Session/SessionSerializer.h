#pragma once
//==============================================================================
// PRD-0095: SessionSerializer — the round-trip mapping between the live `daw`
// ValueTree (PRD-0063 / PRD-0087) and a `.soniksession` file on disk.
//
// Contract:
//   * save(daw, metadata, target) writes the complete model losslessly to a
//     `.soniksession` file, ATOMICALLY (temp-write + rename, §1.5.5), with the
//     target extension normalised to `.soniksession` (§1.5.7).
//   * load(source) parses the file, runs it through SessionMigrator, and returns
//     a SessionDocument, or a typed error (FileNotFound / CorruptFile /
//     UnsupportedFutureVersion). It NEVER returns a partial/garbage model.
//   * load(save(x)) is deeply equal to x for every persisted field.
//
// The `daw` subtree is persisted as a verbatim structural copy, so automation
// (PRD-0087) and any unknown future node round-trip without a hand-written field
// projection.
//
// THREADING: message/background thread only. Performs file I/O freely; touches
// no processBlock path; allocates juce::String / juce::ValueTree freely.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <optional>

#include "SessionSchema.h"
#include "SessionMigrator.h"

namespace Daw::Session
{
    //--------------------------------------------------------------------------
    // Master grid / transport reference persisted in the MASTER_GRID node.
    // (The live MasterGridService is derived from the master clock; this is the
    // saved snapshot of the reference the arrangement was built against.)
    //--------------------------------------------------------------------------
    struct MasterGridRef
    {
        double       bpm                { 0.0 };
        std::int64_t downbeatSample     { 0 };
        int          timeSigNumerator   { 4 };
        int          timeSigDenominator { 4 };
        std::int64_t playheadSample     { 0 };
        std::int64_t loopStartSample    { 0 };
        std::int64_t loopEndSample      { 0 };
        bool         loopEnabled        { false };
    };

    //--------------------------------------------------------------------------
    // Persisted view chrome. Every field is optional: a session missing them (or
    // a future session adding more) loads cleanly and the caller (PRD-0096)
    // applies sensible defaults, so view state never blocks reconstruction.
    //--------------------------------------------------------------------------
    struct ViewState
    {
        std::optional<double>       zoomSamplesPerPixel;
        std::optional<std::int64_t> scrollStartSample;
        juce::String                selectedClipId; // empty => no selection
    };

    //--------------------------------------------------------------------------
    // Everything `save` needs beyond the `daw` tree itself.
    //--------------------------------------------------------------------------
    struct SessionMetadata
    {
        double         projectSampleRate { DawState::kProjectSampleRate };
        MasterGridRef  masterGrid;
        ViewState      viewState;
        juce::String   appVersion;

        // Optional relocation-hint table (PRD-0095 §1.5.4). When valid, it must
        // be a SOURCE_REFS node whose children are SOURCE_REF nodes; it is
        // copied verbatim into the document. PRD-0097 consumes it on open.
        juce::ValueTree sourceRefs;
    };

    //--------------------------------------------------------------------------
    // The reconstructed model handed back by `load`.
    //--------------------------------------------------------------------------
    struct SessionDocument
    {
        juce::ValueTree daw;          // type DawIDs::Daw, ready to swap into the model
        double          projectSampleRate { DawState::kProjectSampleRate };
        MasterGridRef   masterGrid;
        ViewState       viewState;
        juce::ValueTree sourceRefs;   // SOURCE_REFS node (may be invalid/empty)
        int             loadedFromVersion { kCurrentSchemaVersion };
    };

    //--------------------------------------------------------------------------
    // Typed outcomes.
    //--------------------------------------------------------------------------
    enum class SaveError
    {
        None = 0,
        InvalidInput,   // the supplied `daw` tree is not a Daw branch
        IoFailure       // temp-write or rename failed (original left intact)
    };

    enum class LoadError
    {
        None = 0,
        FileNotFound,
        CorruptFile,             // unparseable, wrong root, or unmigratable
        UnsupportedFutureVersion // schemaVersion > kCurrentSchemaVersion
    };

    struct SaveResult
    {
        SaveError    error { SaveError::None };
        juce::File   writtenPath;     // the normalised `.soniksession` path
        juce::String message;
        bool ok() const noexcept { return error == SaveError::None; }
    };

    struct LoadResult
    {
        LoadError       error { LoadError::None };
        SessionDocument document;
        juce::String    message;
        bool ok() const noexcept { return error == LoadError::None; }
    };

    //==========================================================================
    class SessionSerializer
    {
    public:
        SessionSerializer() = default;
        virtual ~SessionSerializer() = default;

        //----------------------------------------------------------------------
        // Writes the whole model to `target`, normalising the extension to
        // `.soniksession` and writing atomically. The supplied `daw` must be a
        // DawIDs::Daw branch; it is copied (the caller's live tree is untouched).
        //----------------------------------------------------------------------
        SaveResult save (const juce::ValueTree& daw,
                         const SessionMetadata& metadata,
                         const juce::File& target);

        //----------------------------------------------------------------------
        // Reads, validates, migrates, and reconstructs a session. The extension
        // is NOT enforced — content is authoritative.
        //----------------------------------------------------------------------
        LoadResult load (const juce::File& source) const;

        //----------------------------------------------------------------------
        // Builds the full SONIK_SESSION root tree (without touching disk).
        // Exposed for tests and for callers that want to inspect/encode the tree
        // themselves.
        //----------------------------------------------------------------------
        juce::ValueTree buildSessionTree (const juce::ValueTree& daw,
                                          const SessionMetadata& metadata) const;

        //----------------------------------------------------------------------
        // Reconstructs a SessionDocument from an already-parsed (and migrated)
        // SONIK_SESSION root. Exposed for tests.
        //----------------------------------------------------------------------
        static SessionDocument documentFromTree (const juce::ValueTree& sessionRoot);

        //----------------------------------------------------------------------
        // Normalises any path to carry exactly one `.soniksession` extension.
        //----------------------------------------------------------------------
        static juce::File normaliseTarget (const juce::File& target);

        //----------------------------------------------------------------------
        // TEST SEAM (§1.4 atomic-write guarantee): when set true, `save`
        // simulates a failure AFTER the temp file is written but BEFORE the
        // rename, so a test can assert any pre-existing target is left intact.
        // Not used in production.
        //----------------------------------------------------------------------
        void setSimulateWriteFailureForTest (bool shouldFail) noexcept
        {
            simulateWriteFailureForTest = shouldFail;
        }

    protected:
        const SessionMigrator& migrator() const noexcept { return sessionMigrator; }

    private:
        SaveError writeAtomically (const juce::File& target, const juce::String& xml) const;

        SessionMigrator sessionMigrator;
        bool            simulateWriteFailureForTest { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionSerializer)
    };
}
