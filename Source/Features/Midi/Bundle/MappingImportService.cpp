#include "MappingImportService.h"

#include "../ControlTargetRegistry.h"
#include "../MappingParser.h"
#include "SonikMidiBundleSerializer.h"

#include <utility>

namespace sonik::midi
{
    namespace
    {
        juce::String describeDeviceMatch (const Mapping& m)
        {
            const auto& dm = m.deviceMatch;
            const auto manufacturer = dm.manufacturerPattern.isNotEmpty() ? dm.manufacturerPattern : juce::String ("(any)");
            const auto product      = dm.productPattern.isNotEmpty()      ? dm.productPattern      : juce::String ("(any)");
            return manufacturer + " " + product;
        }
    }

    //--------------------------------------------------------------------------
    MappingImportService::MappingImportService (MappingStore& s,
                                                const MigrationRegistry& reg,
                                                juce::ThreadPool& pool,
                                                int targetSchema)
        : store               (s),
          migrations          (reg),
          threadPool          (pool),
          targetSchemaVersion (targetSchema)
    {
    }

    //--------------------------------------------------------------------------
    void MappingImportService::prepareImportAsync (juce::File source, PrepareCallback onResult)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        threadPool.addJob ([this,
                            src      = std::move (source),
                            callback = std::move (onResult)]() mutable
        {
            jassert (juce::MessageManager::getInstance() == nullptr
                     || ! juce::MessageManager::getInstance()->isThisTheMessageThread());

            auto prepared = prepareImportFromFile (src);

            juce::MessageManager::callAsync ([callback = std::move (callback),
                                              prepared = std::move (prepared)]() mutable
            {
                if (callback)
                    callback (std::move (prepared));
            });
        });
    }

    //--------------------------------------------------------------------------
    ImportPrepared MappingImportService::prepareImportFromFile (const juce::File& source)
    {
        ImportPrepared out;

        if (! source.existsAsFile())
        {
            out.error.stage  = ImportStage::JsonParse;
            out.error.reason = "file does not exist: " + source.getFullPathName();
            return out;
        }

        const auto text = source.loadFileAsString();
        if (text.isEmpty())
        {
            out.error.stage  = ImportStage::JsonParse;
            out.error.reason = "file is empty";
            return out;
        }

        juce::var root;
        const auto parseResult = juce::JSON::parse (text, root);
        if (parseResult.failed() || root.isVoid())
        {
            out.error.stage             = ImportStage::JsonParse;
            out.error.reason            = "JSON parse failure";
            out.error.parserErrorDetail = parseResult.getErrorMessage();
            return out;
        }

        return runPipeline (root);
    }

    //--------------------------------------------------------------------------
    ImportPrepared MappingImportService::prepareImportFromJson (const juce::var& root)
    {
        return runPipeline (root);
    }

    //--------------------------------------------------------------------------
    ImportPrepared MappingImportService::runPipeline (const juce::var& root)
    {
        ImportPrepared out;

        // ---- Stage 2: manifest extract -------------------------------------
        auto envelope = SonikMidiBundleSerializer::fromJson (root);
        if (! envelope.ok)
        {
            out.error.stage                = ImportStage::ManifestExtract;
            out.error.reason               = envelope.error.reason;
            out.error.missingManifestField = envelope.error.missingField;
            return out;
        }

        SonikMidiBundle bundle = std::move (envelope.value);

        // ---- Stage 3: SHA-256 verify ---------------------------------------
        const auto computedHash = SonikMidiBundleSerializer::sha256OfSortedJson (bundle.mappingJson);
        if (! computedHash.equalsIgnoreCase (bundle.manifest.sha256))
        {
            out.error.stage          = ImportStage::Sha256Verify;
            out.error.reason         = "sha256 mismatch: mapping block has been altered or the file is corrupt";
            out.error.expectedSha256 = bundle.manifest.sha256;
            out.error.computedSha256 = computedHash;
            return out;
        }

        // ---- Stage 4: schema migration -------------------------------------
        const int fromVersion = static_cast<int> (bundle.mappingJson.getProperty ("schemaVersion", 0));
        auto migrationResult  = migrations.apply (bundle.mappingJson, fromVersion, targetSchemaVersion);

        if (migrationResult.error.has_value())
        {
            const bool newerThanSupported = fromVersion > targetSchemaVersion;
            out.error.stage               = ImportStage::SchemaMigrate;
            out.error.reason              = newerThanSupported
                                              ? ("unsupported schema version: bundle is v" + juce::String (fromVersion)
                                                 + ", this build supports up to v" + juce::String (targetSchemaVersion))
                                              : ("migration failed: " + migrationResult.error->reason);
            out.error.migrationError      = migrationResult.error;
            out.error.fromVersion         = fromVersion;
            out.error.maxSupportedVersion = targetSchemaVersion;
            return out;
        }

        const auto migratedJson      = migrationResult.migratedJson;
        const auto migrationStepsCnt = static_cast<int> (migrationResult.stepsApplied.size());

        // ---- Stage 5: parse mapping ---------------------------------------
        auto parseRes = MappingParser::parse (migratedJson, "<import>");

        // A bundle whose mapping has no usable bindings AT ALL is treated as
        // a stage-5 fatal failure — the file is structurally broken.
        if (parseRes.mapping.deviceMatch.manufacturerPattern.isEmpty()
            && parseRes.mapping.deviceMatch.productPattern.isEmpty()
            && parseRes.mapping.deviceMatch.midiNamePattern.isEmpty()
            && parseRes.mapping.bindings.empty()
            && parseRes.mapping.modifiers.empty()
            && ! parseRes.errors.empty())
        {
            out.error.stage              = ImportStage::MappingParse;
            out.error.reason             = "mapping parse failed: " + parseRes.errors.front().detail;
            out.error.mappingParseDetail = parseRes.errors.front().detail;
            return out;
        }

        const auto parsedMapping = std::make_shared<const Mapping> (parseRes.mapping);

        // ---- Stage 7 detection (deferred until preview assembly) ----------
        juce::StringArray unknownTargetIds;
        if (const auto* bindingsArr = migratedJson.getProperty ("bindings", juce::var()).getArray())
        {
            for (const auto& bVar : *bindingsArr)
            {
                if (! bVar.isObject())
                    continue;
                const auto targetStr = bVar.getProperty ("target", juce::var()).toString();
                if (targetStr.isEmpty())
                    continue;
                if (! ControlTargetRegistry::lookup (juce::StringRef (targetStr.toRawUTF8())).has_value())
                {
                    if (! unknownTargetIds.contains (targetStr))
                        unknownTargetIds.add (targetStr);
                }
            }
        }

        // ---- Preview assembly ---------------------------------------------
        const auto displayName = parsedMapping->displayName.isNotEmpty()
                                     ? parsedMapping->displayName
                                     : juce::String ("imported-mapping");
        const auto intendedStem = MappingStore::sanitiseFilenameStem (displayName);

        out.preview.deviceMatchDisplay    = describeDeviceMatch (*parsedMapping);
        out.preview.mappingId             = intendedStem;
        out.preview.mappingName           = displayName;
        out.preview.schemaVersion         = parsedMapping->schemaVersion;
        out.preview.exporterAppVersion    = bundle.manifest.appVersion;
        out.preview.exporterDeviceName    = bundle.manifest.exporterDeviceName;
        out.preview.exportedAtIso8601     = bundle.manifest.exportedAtIso8601;
        out.preview.bindingCount          = static_cast<int> (parsedMapping->bindings.size());
        out.preview.modifierCount         = static_cast<int> (parsedMapping->modifiers.size());
        out.preview.migrationStepsApplied = migrationStepsCnt;
        out.preview.unknownTargetIds      = unknownTargetIds;

        // ---- Stage 6: conflict detection ----------------------------------
        if (intendedStem.isNotEmpty() && store.userMappingExists (intendedStem))
        {
            out.preview.conflictDetected         = true;
            out.preview.conflictExistingMappingId = intendedStem;
            // Surfaced via preview, not as a fatal error — UI shows the
            // conflict modal; commitImport requires a resolution.
        }

        out.mapping = parsedMapping;
        out.ok      = true;
        return out;
    }

    //--------------------------------------------------------------------------
    ImportCommitResult MappingImportService::commitImport (const ImportPrepared& prepared,
                                                           ConflictResolution    resolution,
                                                           const juce::String&   renameToStem)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        ImportCommitResult result;

        if (! prepared.ok || prepared.mapping == nullptr)
        {
            result.status      = ImportCommitResult::Status::SerializeFailure;
            result.errorDetail = "prepared import is not ok";
            return result;
        }

        if (resolution == ConflictResolution::Cancel)
        {
            result.status = ImportCommitResult::Status::Cancelled;
            return result;
        }

        juce::String stem    = prepared.preview.mappingId;
        bool overwrite       = false;

        if (prepared.preview.conflictDetected)
        {
            switch (resolution)
            {
                case ConflictResolution::Replace:
                    overwrite = true;
                    break;
                case ConflictResolution::RenameTo:
                    stem = MappingStore::sanitiseFilenameStem (renameToStem);
                    if (stem.isEmpty())
                    {
                        result.status      = ImportCommitResult::Status::InvalidName;
                        result.errorDetail = "rename target is not a valid filesystem stem";
                        return result;
                    }
                    break;
                case ConflictResolution::None:
                case ConflictResolution::Cancel:
                    result.status      = ImportCommitResult::Status::ConflictUnresolved;
                    result.errorDetail = "conflict requires Rename or Replace";
                    return result;
            }
        }
        else
        {
            // No conflict — accept an explicit RenameTo if provided, else use the
            // intended stem from the preview.
            if (resolution == ConflictResolution::RenameTo)
            {
                stem = MappingStore::sanitiseFilenameStem (renameToStem);
                if (stem.isEmpty())
                {
                    result.status      = ImportCommitResult::Status::InvalidName;
                    result.errorDetail = "rename target is not a valid filesystem stem";
                    return result;
                }
            }
        }

        if (stem.isEmpty())
        {
            result.status      = ImportCommitResult::Status::InvalidName;
            result.errorDetail = "intended filename stem is empty";
            return result;
        }

        auto reg = store.registerImportedMapping (*prepared.mapping, stem, overwrite);

        switch (reg.status)
        {
            case RegisterImportedResult::Status::Ok:
                result.status         = ImportCommitResult::Status::Ok;
                result.finalMappingId = reg.finalStem;
                break;
            case RegisterImportedResult::Status::InvalidStem:
                result.status      = ImportCommitResult::Status::InvalidName;
                result.errorDetail = "filename stem rejected by store";
                break;
            case RegisterImportedResult::Status::ConflictNotOverwritten:
                result.status      = ImportCommitResult::Status::ConflictUnresolved;
                result.errorDetail = "conflict still present and overwrite not requested";
                break;
            case RegisterImportedResult::Status::IoFailure:
                result.status      = ImportCommitResult::Status::IoFailure;
                result.errorDetail = "atomic write failed";
                break;
            case RegisterImportedResult::Status::SerializeFailure:
                result.status      = ImportCommitResult::Status::SerializeFailure;
                result.errorDetail = "MappingSerializer threw";
                break;
        }

        return result;
    }
}
