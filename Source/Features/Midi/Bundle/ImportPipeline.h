#pragma once
//==============================================================================
// PRD-0050: Import validation pipeline types.
//
// A single import flow runs the 7 stages declared in `ImportStage` in order.
// Stages 1–6 are fatal: the first failure halts the pipeline and yields an
// `ImportError`. Stage 7 (unknown control-target ids) is non-fatal — the
// `ImportPreview` carries the list of dropped target ids forward to the UI;
// the user may still commit, and `MappingParser::parse` will drop the
// affected bindings at load time (PRD-0042 partial-load behaviour).
//==============================================================================

#include "../MappingTypes.h"
#include "../Migrations/MigrationRegistry.h"
#include "SonikMidiBundle.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace sonik::midi
{
    enum class ImportStage : std::uint8_t
    {
        JsonParse        = 1,
        ManifestExtract  = 2,
        Sha256Verify     = 3,
        SchemaMigrate    = 4,
        MappingParse     = 5,
        ConflictDetect   = 6,
        TargetIdValidate = 7,
    };

    struct ImportError
    {
        ImportStage  stage { ImportStage::JsonParse };
        juce::String reason;

        // Stage-1 detail.
        std::optional<int> sourceLine;
        std::optional<int> sourceOffset;
        juce::String       parserErrorDetail;

        // Stage-2 detail.
        juce::String missingManifestField;

        // Stage-3 detail.
        juce::String expectedSha256;
        juce::String computedSha256;

        // Stage-4 detail.
        std::optional<MigrationError> migrationError;
        int  fromVersion         { 0 };
        int  maxSupportedVersion { 0 };

        // Stage-5 detail.
        juce::String mappingParseDetail;

        // Stage-6 detail.
        juce::String conflictExistingMappingId;

        // Stage-7 detail (non-fatal; only ever populated alongside ok=true).
        juce::StringArray unknownTargetIds;
    };

    /** Populated by stages 1-5 success; surfaced to the dialog. */
    struct ImportPreview
    {
        juce::String      deviceMatchDisplay;        // "Behringer DDM4000" or pattern fallback.
        juce::String      mappingId;                 // intended filename stem (sanitised).
        juce::String      mappingName;               // mapping.displayName (or id if absent).
        int               schemaVersion { 0 };
        juce::String      exporterAppVersion;
        juce::String      exporterDeviceName;
        juce::String      exportedAtIso8601;
        int               bindingCount { 0 };
        int               modifierCount { 0 };
        int               migrationStepsApplied { 0 };
        juce::StringArray unknownTargetIds;          // populated by stage 7 (non-fatal)
        bool              conflictDetected { false };
        juce::String      conflictExistingMappingId; // stem of existing user mapping
    };

    /** Conflict resolution selected by the user. */
    enum class ConflictResolution : std::uint8_t
    {
        None,             // No conflict — proceed with the preview's intended stem.
        RenameTo,         // Use `renameToStem` instead.
        Replace,          // Overwrite the existing user mapping.
        Cancel,           // Abort.
    };
}
