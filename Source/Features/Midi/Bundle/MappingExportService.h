#pragma once
//==============================================================================
// PRD-0050: Mapping export service.
//
// Runs the export workflow on a background `juce::ThreadPool`:
//   1. Serialise the chosen Mapping via `MappingSerializer::serialize`.
//   2. Compute the deterministic SHA-256 of the sorted mapping JSON.
//   3. Assemble the bundle envelope (manifest + mapping).
//   4. Atomically write `<destination>.tmp` then rename → `<destination>`.
//      On failure the `.tmp` is unlinked; the destination is never left in
//      a half-written state.
//
// All file I/O and hashing run off the Message thread. Completion is posted
// back via `juce::MessageManager::callAsync` so the caller's callback runs
// on the Message thread.
//==============================================================================

#include "../MappingStore.h"
#include "SonikMidiBundle.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>

namespace sonik::midi
{
    struct ExportResult
    {
        enum class Status : std::uint8_t
        {
            Ok,
            UnknownMapping,
            SerializeFailure,
            IoFailure,
        };

        Status       status { Status::Ok };
        juce::File   destination;
        juce::String errorDetail;
    };

    class MappingExportService final
    {
    public:
        MappingExportService (MappingStore& store,
                              juce::ThreadPool& pool,
                              juce::String appVersion);

        using ExportCallback = std::function<void (const ExportResult&)>;

        /** Background-thread export. `onComplete` is invoked on the Message
            thread when finished. */
        void exportMappingAsync (juce::String mappingId,
                                 juce::File   destination,
                                 ExportCallback onComplete);

        /** Synchronous helper for tests — runs the entire export on the
            calling thread. */
        ExportResult exportMappingNow (juce::String mappingId,
                                       juce::File   destination);

    private:
        ExportResult runExport (const juce::String& mappingId,
                                const juce::File&   destination);

        MappingStore&     store;
        juce::ThreadPool& threadPool;
        juce::String      appVersion;
    };
}
