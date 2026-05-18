#include "MappingExportService.h"

#include "../MappingSerializer.h"
#include "SonikMidiBundleSerializer.h"

#include <utility>

namespace sonik::midi
{
    namespace
    {
        juce::String currentUtcIso8601()
        {
            const auto t = juce::Time::getCurrentTime();
            return t.toISO8601 (/*includeDividerCharacters*/ true);
        }

        juce::String hostDeviceName()
        {
            const auto name = juce::SystemStats::getComputerName();
            return name.isEmpty() ? juce::String() : name;
        }
    }

    //--------------------------------------------------------------------------
    MappingExportService::MappingExportService (MappingStore& s,
                                                juce::ThreadPool& pool,
                                                juce::String version)
        : store      (s),
          threadPool (pool),
          appVersion (std::move (version))
    {
    }

    //--------------------------------------------------------------------------
    void MappingExportService::exportMappingAsync (juce::String mappingId,
                                                   juce::File   destination,
                                                   ExportCallback onComplete)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        threadPool.addJob ([this,
                            id       = std::move (mappingId),
                            dest     = std::move (destination),
                            callback = std::move (onComplete)]() mutable
        {
            // Background thread — explicitly NOT the Message thread.
            jassert (juce::MessageManager::getInstance() == nullptr
                     || ! juce::MessageManager::getInstance()->isThisTheMessageThread());

            auto result = runExport (id, dest);

            juce::MessageManager::callAsync ([callback = std::move (callback),
                                              result   = std::move (result)]() mutable
            {
                if (callback)
                    callback (result);
            });
        });
    }

    //--------------------------------------------------------------------------
    ExportResult MappingExportService::exportMappingNow (juce::String mappingId,
                                                         juce::File   destination)
    {
        return runExport (mappingId, destination);
    }

    //--------------------------------------------------------------------------
    ExportResult MappingExportService::runExport (const juce::String& mappingId,
                                                  const juce::File&   destination)
    {
        ExportResult result;
        result.destination = destination;

        auto mapping = store.getMappingById (mappingId);
        if (mapping == nullptr)
        {
            result.status      = ExportResult::Status::UnknownMapping;
            result.errorDetail = "no mapping with id '" + mappingId + "'";
            return result;
        }

        juce::var mappingJson;
        try
        {
            mappingJson = MappingSerializer::serialize (*mapping);
        }
        catch (...)
        {
            result.status      = ExportResult::Status::SerializeFailure;
            result.errorDetail = "MappingSerializer::serialize threw";
            return result;
        }

        SonikMidiBundle bundle;
        bundle.manifest.appVersion                 = appVersion;
        bundle.manifest.sonikSchemaVersionAtExport = mapping->schemaVersion;
        bundle.manifest.exportedAtIso8601          = currentUtcIso8601();
        bundle.manifest.exporterDeviceName         = hostDeviceName();
        bundle.manifest.sha256                     = SonikMidiBundleSerializer::sha256OfSortedJson (mappingJson);
        bundle.mappingJson                         = std::move (mappingJson);

        const auto bundleVar = SonikMidiBundleSerializer::toJson (bundle);
        const auto text      = juce::JSON::toString (bundleVar, /*allOnOneLine*/ false);

        if (auto* parentDir = new juce::File (destination.getParentDirectory()))
        {
            std::unique_ptr<juce::File> own (parentDir);
            if (! parentDir->exists() && ! parentDir->createDirectory().wasOk())
            {
                result.status      = ExportResult::Status::IoFailure;
                result.errorDetail = "could not create destination directory";
                return result;
            }
        }

        const auto tmp = destination.getSiblingFile (destination.getFileName() + ".tmp");
        if (tmp.existsAsFile())
            tmp.deleteFile();

        if (! tmp.replaceWithText (text))
        {
            tmp.deleteFile();
            result.status      = ExportResult::Status::IoFailure;
            result.errorDetail = "could not write temp file";
            return result;
        }

        if (destination.existsAsFile())
            destination.deleteFile();

        if (! tmp.moveFileTo (destination))
        {
            tmp.deleteFile();
            result.status      = ExportResult::Status::IoFailure;
            result.errorDetail = "could not rename temp file to destination";
            return result;
        }

        result.status = ExportResult::Status::Ok;
        return result;
    }
}
