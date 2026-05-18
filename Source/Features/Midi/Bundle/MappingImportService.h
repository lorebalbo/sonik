#pragma once
//==============================================================================
// PRD-0050: Mapping import service.
//
// Runs the 7-stage validation pipeline declared in `ImportPipeline.h` on a
// background `juce::ThreadPool` so the Message thread is never blocked on
// JSON parsing, SHA-256 hashing, or file I/O.
//
// The service is split into two phases by design:
//
//   * `prepareImportAsync(file, onResult)` — runs stages 1-7 *without*
//     writing anything to disk and posts the resulting preview (or
//     `ImportError`) back to the Message thread. The UI uses this to render
//     the preview pane and the conflict modal.
//
//   * `commitImport(prepared, resolution, ...)` — synchronous, Message
//     thread only, writes the migrated mapping to disk via
//     `MappingStore::registerImportedMapping`.
//
// Splitting prepare/commit lets the UI present the conflict modal after the
// hash + parse pass but before any file system change occurs.
//==============================================================================

#include "../MappingStore.h"
#include "ImportPipeline.h"
#include "SonikMidiBundle.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>

namespace sonik::midi
{
    /** Result of the import preparation phase (stages 1-7, no disk write).
        The `prepared` Mapping is the parsed, migrated form ready to be
        registered by `commitImport`. */
    struct ImportPrepared
    {
        bool                                ok { false };
        ImportError                         error;          // populated when ok == false
        ImportPreview                       preview;
        std::shared_ptr<const Mapping>      mapping;        // populated when ok == true
    };

    struct ImportCommitResult
    {
        enum class Status : std::uint8_t
        {
            Ok,
            Cancelled,
            InvalidName,
            ConflictUnresolved,
            IoFailure,
            SerializeFailure,
        };

        Status       status { Status::Ok };
        juce::String finalMappingId;
        juce::String errorDetail;
    };

    class MappingImportService final
    {
    public:
        MappingImportService (MappingStore& store,
                              const MigrationRegistry& migrations,
                              juce::ThreadPool& pool,
                              int targetSchemaVersion = kCurrentSchemaVersion);

        using PrepareCallback = std::function<void (ImportPrepared)>;

        /** Background-thread prepare. Callback marshals to the Message
            thread automatically. */
        void prepareImportAsync (juce::File source, PrepareCallback onResult);

        /** Synchronous prepare from a file path — used by tests and by the
            sync path inside `prepareImportAsync`. */
        ImportPrepared prepareImportFromFile (const juce::File& source);

        /** Synchronous prepare from already-parsed root JSON. */
        ImportPrepared prepareImportFromJson (const juce::var& root);

        /** Message-thread commit of a successful prepare. Writes the
            mapping to disk atomically and registers it with MappingStore. */
        ImportCommitResult commitImport (const ImportPrepared& prepared,
                                         ConflictResolution    resolution,
                                         const juce::String&   renameToStem = {});

    private:
        ImportPrepared runPipeline (const juce::var& root);

        MappingStore&            store;
        const MigrationRegistry& migrations;
        juce::ThreadPool&        threadPool;
        int                      targetSchemaVersion;
    };
}
