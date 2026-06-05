#include "MappingStore.h"

#include "MappingParser.h"
#include "MappingSerializer.h"
#include "MidiDeviceManager.h"

#include "BinaryData.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <regex>
#include <utility>

namespace sonik::midi
{
    namespace
    {
        constexpr int  kMaxFileSizeBytes = 1 * 1024 * 1024; // 1 MB
        const char*    kDdm4000Id        = "behringer-ddm4000";
        const char*    kGenericId        = "generic-midi";

        // Compile a pattern as std::regex, returning nullopt on bad syntax.
        std::optional<std::regex> compilePattern (const juce::String& pattern)
        {
            if (pattern.isEmpty())
                return std::nullopt;
            try
            {
                return std::regex (pattern.toStdString(),
                                   std::regex::ECMAScript | std::regex::icase);
            }
            catch (const std::regex_error&)
            {
                return std::nullopt;
            }
        }

        bool regexAcceptsOrEmpty (const juce::String& pattern, const juce::String& candidate)
        {
            if (pattern.isEmpty())
                return true; // No constraint declared.
            const auto re = compilePattern (pattern);
            if (! re.has_value())
                return false;
            try
            {
                return std::regex_search (candidate.toStdString(), *re);
            }
            catch (const std::regex_error&)
            {
                return false;
            }
        }
    }

    //--------------------------------------------------------------------------
    juce::File MappingStore::defaultUserMappingDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support")
                   .getChildFile ("Sonik")
                   .getChildFile ("MidiMappings");
    }

    bool MappingStore::isValidUserMappingFilename (juce::StringRef filename) noexcept
    {
        const juce::String s = filename;
        if (s.isEmpty())
            return false;
        if (! s.endsWithIgnoreCase (".json"))
            return false;
        if (s.containsAnyOf ("/\\")
            || s.contains (".."))
            return false;
        return true;
    }

    //--------------------------------------------------------------------------
    MappingStore::MappingStore (MidiDeviceManager& mgr,
                                juce::File         dir,
                          bool               loadUserProfilesAsync,
                          MigrationRegistry  migrationRegistry,
                          int                currentSchemaVersion)
        : deviceManager (mgr),
            userDir (std::move (dir)),
            migrations (std::move (migrationRegistry)),
            schemaVersionTarget (currentSchemaVersion)
    {
        loadBundledProfiles();

        // Create user dir up-front so saveUserMapping doesn't have to.
        if (! userDir.exists())
        {
            const auto result = userDir.createDirectory();
            if (! result.wasOk())
                recordLoadError (userDir.getFullPathName(),
                                 "createDirectory failed: " + result.getErrorMessage());
        }

        // PRD-0048: load per-device active overrides (best-effort, sync).
        loadActiveOverridesFromDisk();

        if (loadUserProfilesAsync)
        {
            threadPool = std::make_unique<juce::ThreadPool> (1);
            enumerateUserProfilesAsync();
        }
        else
        {
            enumerateUserProfilesNow();
            // Fire synchronously so test code can rely on the callback.
            onUserProfilesLoaded();
        }

        deviceManager.addDeviceListChangeListener (this);

        // Resolve mappings for devices already enumerated.
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput)
                continue;
            auto resolved = resolveForRecord (rec);
            std::unique_lock lock (stateMutex);
            activeByDevice[rec.deviceId] = std::move (resolved);
        }
    }

    MappingStore::~MappingStore()
    {
        deviceManager.removeDeviceListChangeListener (this);
        if (threadPool != nullptr)
            threadPool->removeAllJobs (true, 4000);
    }

    //--------------------------------------------------------------------------
    void MappingStore::loadBundledProfiles()
    {
        struct BundledRaw
        {
            const char* id;
            const char* data;
            int         size;
        };

        const BundledRaw bundled[] = {
            { kDdm4000Id, BinaryData::behringerddm4000_json,
                          BinaryData::behringerddm4000_jsonSize },
            { kGenericId, BinaryData::genericmidi_json,
                          BinaryData::genericmidi_jsonSize },
        };

        for (const auto& b : bundled)
        {
            const juce::String text (juce::CharPointer_UTF8 (b.data),
                                     (size_t) b.size);
            const auto root = juce::JSON::parse (text);
            auto parseRes = MappingParser::parse (root, juce::String ("<bundled:") + b.id + ">");

            // Debug: assert parses cleanly.  Release: silently fall back so app still launches.
            jassert (parseRes.errors.empty());

            bundledProfiles.emplace (juce::String (b.id),
                                     std::make_shared<const Mapping> (std::move (parseRes.mapping)));
        }
        genericProfileId = kGenericId;
    }

    //--------------------------------------------------------------------------
    void MappingStore::enumerateUserProfilesAsync()
    {
        loadJobDispatched.store (true, std::memory_order_release);

        juce::WeakReference<MappingStore> weakThis (this);

        threadPool->addJob ([this, weakThis]() noexcept
        {
            std::vector<MappingLoadError> errors;
            std::vector<UserProfileSource> sources;
            try { sources = collectUserProfileSources (errors); }
            catch (...) { /* never propagate */ }

            juce::MessageManager::callAsync ([weakThis,
                                              sources = std::move (sources),
                                              errors  = std::move (errors)]() mutable
            {
                // If the store was destroyed before this queued message ran, the
                // weak reference is now null and we must NOT touch freed state.
                if (auto* self = weakThis.get())
                {
                    self->loadUserProfileSources (std::move (sources), std::move (errors));
                    self->onUserProfilesLoaded();
                }
            });
        });
    }

    void MappingStore::enumerateUserProfilesNow()
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        std::vector<MappingLoadError> errors;
        auto sources = collectUserProfileSources (errors);
        loadUserProfileSources (std::move (sources), std::move (errors));
    }

    std::vector<MappingStore::UserProfileSource>
    MappingStore::collectUserProfileSources (std::vector<MappingLoadError>& errors) const
    {
        std::vector<UserProfileSource> sources;

        if (! userDir.exists() || ! userDir.isDirectory())
            return sources;

        juce::Array<juce::File> files;
        userDir.findChildFiles (files, juce::File::findFiles, false, "*.json");

        // Deterministic order for diagnostics.
        std::sort (files.begin(), files.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });

        for (const auto& file : files)
        {
            // PRD-0048: underscore-prefixed files are internal state (e.g.
            // `_active-mappings.json`), not user-authored mappings.
            if (file.getFileName().startsWithChar ('_'))
                continue;

            const auto size = file.getSize();
            if (size <= 0 || size > kMaxFileSizeBytes)
            {
                errors.emplace_back (MappingLoadError::Kind::IoFailure,
                                     file.getFullPathName(),
                                     "skipped: size out of bounds (" + juce::String (size) + " bytes)");
                continue;
            }

            sources.push_back ({ file, file.loadFileAsString() });
        }

        return sources;
    }

    void MappingStore::loadUserProfileSources (std::vector<UserProfileSource> sources,
                                               std::vector<MappingLoadError> errors)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> loaded;
        std::unordered_map<juce::String, MigratedMappingRecord> migratedRecords;

        for (const auto& source : sources)
        {
            const auto root = juce::JSON::parse (source.jsonText);
            if (root.isVoid())
            {
                errors.emplace_back (MappingLoadError::Kind::JsonParseFailure,
                                     source.file.getFullPathName(),
                                     "JSON parse failure");
                continue;
            }

            auto loadResult = loadMappingFromJson (root, source.file.getFullPathName());
            for (auto&& e : loadResult.errors)
                errors.push_back (std::move (e));

            if (! loadResult.loaded)
                continue;

            const auto stem = source.file.getFileNameWithoutExtension();

            // Accept partially-valid mapping; loader is tolerant per PRD-0042.
            loaded.emplace (stem,
                            std::make_shared<const Mapping> (std::move (loadResult.mapping)));

            if (! loadResult.stepsApplied.empty())
            {
                MigratedMappingRecord record;
                record.mappingId    = stem;
                record.sourcePath   = source.file.getFullPathName();
                record.stepsApplied = std::move (loadResult.stepsApplied);
                record.migratedJson = std::move (loadResult.migratedJson);
                migratedRecords.emplace (stem, std::move (record));
            }
        }

        {
            std::unique_lock lock (stateMutex);
            userProfiles = std::move (loaded);
            migratedUserMappings = std::move (migratedRecords);
            for (auto&& e : errors)
                loadErrors.push_back (std::move (e));
        }
        userProfilesReady.store (true, std::memory_order_release);
    }

    MappingStore::MappingJsonLoadResult
    MappingStore::loadMappingFromJson (const juce::var& root, const juce::String& sourcePath) const
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        MappingJsonLoadResult result;

        if (! root.isObject())
        {
            auto pr = MappingParser::parse (root, sourcePath);
            for (const auto& e : pr.errors)
            {
                MappingLoadError loadError (MappingLoadError::Kind::ValidationError,
                                            sourcePath,
                                            "validation: " + e.detail);
                loadError.parserError = e;
                result.errors.push_back (std::move (loadError));
            }
            result.mapping = std::move (pr.mapping);
            result.loaded  = true;
            return result;
        }

        const int fromVersion = static_cast<int> (root.getProperty ("schemaVersion", 0));
        auto migrationResult = migrations.apply (root, fromVersion, schemaVersionTarget, sourcePath);

        if (migrationResult.error.has_value())
        {
            const bool newerThanSupported = fromVersion > schemaVersionTarget;
            MappingLoadError loadError (newerThanSupported
                                            ? MappingLoadError::Kind::UnsupportedSchemaVersion
                                            : MappingLoadError::Kind::MigrationFailed,
                                        sourcePath,
                                        newerThanSupported
                                            ? ("unsupported schema version: got v" + juce::String (fromVersion)
                                               + ", supported up to v" + juce::String (schemaVersionTarget))
                                            : ("migration failed: " + migrationResult.error->reason));
            loadError.migrationError      = migrationResult.error;
            loadError.fromVersion         = fromVersion;
            loadError.maxSupportedVersion = schemaVersionTarget;
            result.errors.push_back (std::move (loadError));
            return result;
        }

        result.migratedJson = migrationResult.migratedJson;
        result.stepsApplied = std::move (migrationResult.stepsApplied);

        auto pr = MappingParser::parse (result.migratedJson, sourcePath);
        if (! pr.errors.empty() && ! result.stepsApplied.empty())
        {
            const auto& parserError = pr.errors.front();
            MappingLoadError loadError (MappingLoadError::Kind::MigrationProducedInvalidOutput,
                                        sourcePath,
                                        "migration produced invalid output: " + parserError.detail);
            loadError.parserError         = parserError;
            loadError.fromVersion         = fromVersion;
            loadError.maxSupportedVersion = schemaVersionTarget;
            loadError.lastMigrationStep   = result.stepsApplied.back();
            result.errors.push_back (std::move (loadError));
            return result;
        }

        for (const auto& e : pr.errors)
        {
            MappingLoadError loadError (MappingLoadError::Kind::ValidationError,
                                        sourcePath,
                                        "validation: " + e.detail);
            loadError.parserError         = e;
            loadError.fromVersion         = fromVersion;
            loadError.maxSupportedVersion = schemaVersionTarget;
            result.errors.push_back (std::move (loadError));
        }

        result.mapping = std::move (pr.mapping);
        result.loaded  = true;
        return result;
    }

    void MappingStore::onUserProfilesLoaded()
    {
        // Re-resolve any already-connected devices, then notify.
        std::vector<std::uint64_t> affected;
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput)
                continue;
            auto newMapping = resolveForRecord (rec);
            std::shared_ptr<const Mapping> previous;
            juce::String mappingId ("<none>");
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = newMapping;
                if (newMapping != nullptr)
                {
                    for (const auto& [id, m] : bundledProfiles)
                        if (m == newMapping) { mappingId = id + " (bundled)"; break; }
                    if (mappingId == "<none>")
                        for (const auto& [id, m] : userProfiles)
                            if (m == newMapping) { mappingId = id + " (user)"; break; }
                }
            }
            DBG ("[MIDI] mapping resolved  id=" << juce::String::toHexString ((juce::int64) rec.deviceId)
                 << "  device='" << rec.productName << "'"
                 << "  mapping=" << mappingId);
            if (newMapping != previous)
                affected.push_back (rec.deviceId);
        }

        fireUserProfilesLoaded();
        for (auto id : affected)
            fireActiveMappingChanged (id);
    }

    //--------------------------------------------------------------------------
    bool MappingStore::deviceMatchAccepts (const Mapping& m, const MidiDeviceRecord& rec) noexcept
    {
        if (! regexAcceptsOrEmpty (m.deviceMatch.manufacturerPattern, rec.manufacturer))
            return false;
        if (! regexAcceptsOrEmpty (m.deviceMatch.productPattern, rec.productName))
            return false;
        if (! regexAcceptsOrEmpty (m.deviceMatch.midiNamePattern, rec.productName))
            return false;
        return true;
    }

    int MappingStore::scoreCandidate (const Mapping&         mapping,
                                      const MidiDeviceRecord& record,
                                      bool                   isUserMapping) noexcept
    {
        // PRD-0051 §1.4 AC 5 scoring tiers.
        if (! deviceMatchAccepts (mapping, record))
            return 0;

        if (mapping.deviceMatch.hasIdentifierHint())
        {
            if (! mapping.deviceMatch.identifierHintMatches (record.juceIdentifier))
                return 0; // configured but does not match -> reject
            return isUserMapping ? 4 : 2;
        }

        // No identifierHint: any-port match.
        return isUserMapping ? 3 : 1;
    }

    std::shared_ptr<const Mapping> MappingStore::resolveForRecord (const MidiDeviceRecord& rec) const
    {
        // Snapshot containers under shared lock.
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> userSnap;
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> bundledSnap;
        juce::String genericId;
        juce::String overrideId;
        {
            std::shared_lock lock (stateMutex);
            userSnap    = userProfiles;
            bundledSnap = bundledProfiles;
            genericId   = genericProfileId;
            if (auto it = activeOverrideByDevice.find (rec.deviceId);
                it != activeOverrideByDevice.end())
                overrideId = it->second;
        }

        // PRD-0048: explicit per-device override wins. Note that overrides do
        // NOT bypass identifierHint matching when matching is possible — but
        // the user has explicitly pinned a profile, so we honour the choice
        // even if its identifierHint does not match the device's port. The
        // intent is "I have decided this mapping owns this device".
        if (overrideId.isNotEmpty())
        {
            if (auto it = userSnap.find (overrideId); it != userSnap.end() && it->second != nullptr)
                return it->second;
            if (auto it = bundledSnap.find (overrideId); it != bundledSnap.end() && it->second != nullptr)
                return it->second;
            // Stale override (referenced mapping no longer exists). Fall
            // through to automatic resolution; the override is cleaned up
            // lazily by deleteUserMapping; we do not mutate here under a
            // reader lock.
        }

        // PRD-0051: scored resolution. Higher score wins; ties broken by
        // (user > bundled), then alphabetical id for determinism.
        struct Candidate
        {
            int                            score;
            bool                           isUser;
            juce::String                   id;
            std::shared_ptr<const Mapping> mapping;
        };
        std::vector<Candidate> candidates;
        candidates.reserve (userSnap.size() + bundledSnap.size());

        for (const auto& [stem, mapping] : userSnap)
        {
            if (mapping == nullptr) continue;
            const int s = scoreCandidate (*mapping, rec, /*isUserMapping*/ true);
            if (s > 0)
                candidates.push_back ({ s, true, stem, mapping });
        }
        for (const auto& [id, mapping] : bundledSnap)
        {
            if (mapping == nullptr) continue;
            if (id == genericId)
                continue; // generic-midi is the universal fallback, scored separately
            const int s = scoreCandidate (*mapping, rec, /*isUserMapping*/ false);
            if (s > 0)
                candidates.push_back ({ s, false, id, mapping });
        }

        if (! candidates.empty())
        {
            std::sort (candidates.begin(), candidates.end(),
                       [] (const Candidate& a, const Candidate& b) noexcept
                       {
                           if (a.score != b.score) return a.score > b.score;
                           if (a.isUser != b.isUser) return a.isUser; // user > bundled
                           return a.id.compareIgnoreCase (b.id) < 0;
                       });
            return candidates.front().mapping;
        }

        // Generic-midi fallback (lowest tier).
        if (auto it = bundledSnap.find (genericId); it != bundledSnap.end())
            return it->second;

        return nullptr;
    }

    //--------------------------------------------------------------------------
    std::shared_ptr<const Mapping> MappingStore::getActiveMappingForDevice (std::uint64_t deviceId) const
    {
        std::shared_lock lock (stateMutex);
        if (const auto it = activeByDevice.find (deviceId); it != activeByDevice.end())
            return it->second;
        return nullptr;
    }

    //--------------------------------------------------------------------------
    void MappingStore::midiDeviceAdded (std::uint64_t deviceId)
    {
        MidiDeviceRecord rec;
        for (const auto& r : deviceManager.getDevices())
            if (r.deviceId == deviceId) { rec = r; break; }
        if (rec.deviceId == 0)
            return;
        if (! rec.isInput)
            return;

        auto mapping = resolveForRecord (rec);
        juce::String mappingId ("<none>");
        {
            std::unique_lock lock (stateMutex);
            activeByDevice[deviceId] = mapping;
            if (mapping != nullptr)
            {
                for (const auto& [id, m] : bundledProfiles)
                    if (m == mapping) { mappingId = id + " (bundled)"; break; }
                if (mappingId == "<none>")
                    for (const auto& [id, m] : userProfiles)
                        if (m == mapping) { mappingId = id + " (user)"; break; }
            }
        }
        DBG ("[MIDI] mapping resolved  id=" << juce::String::toHexString ((juce::int64) deviceId)
             << "  device='" << rec.productName << "'"
             << "  mapping=" << mappingId);
        fireActiveMappingChanged (deviceId);
    }

    void MappingStore::midiDeviceRemoved (std::uint64_t deviceId)
    {
        std::unique_lock lock (stateMutex);
        activeByDevice.erase (deviceId);
    }

    //--------------------------------------------------------------------------
    SaveResult MappingStore::saveUserMapping (const Mapping& mapping, juce::StringRef filename)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        if (! isValidUserMappingFilename (filename))
        {
            fireUserMappingSaved (juce::String (filename), SaveResult::InvalidFilename);
            return SaveResult::InvalidFilename;
        }

        // Ensure the user dir exists at save time as well (paranoid: user
        // might have deleted it between construction and now).
        if (! userDir.exists())
        {
            const auto cr = userDir.createDirectory();
            if (! cr.wasOk())
            {
                fireUserMappingSaved (juce::String (filename), SaveResult::IoFailure);
                return SaveResult::IoFailure;
            }
        }

        juce::var root;
        try { root = MappingSerializer::serialize (mapping); }
        catch (...)
        {
            fireUserMappingSaved (juce::String (filename), SaveResult::SerializeFailure);
            return SaveResult::SerializeFailure;
        }

        const auto json = juce::JSON::toString (root, false);

        const auto target = userDir.getChildFile (juce::String (filename));
        const auto tmp    = userDir.getChildFile (juce::String (filename) + ".tmp");

        if (tmp.existsAsFile())
            tmp.deleteFile();

        if (! tmp.replaceWithText (json))
        {
            fireUserMappingSaved (juce::String (filename), SaveResult::IoFailure);
            return SaveResult::IoFailure;
        }

        if (target.existsAsFile())
            target.deleteFile();

        if (! tmp.moveFileTo (target))
        {
            tmp.deleteFile();
            fireUserMappingSaved (juce::String (filename), SaveResult::IoFailure);
            return SaveResult::IoFailure;
        }

        // Re-parse to update in-memory state from the canonical source on disk.
        auto pr = MappingParser::parse (root, target.getFullPathName());
        auto sp = std::make_shared<const Mapping> (std::move (pr.mapping));

        const auto stem = target.getFileNameWithoutExtension();
        bool wasNew = false;
        std::vector<std::uint64_t> affected;
        {
            std::unique_lock lock (stateMutex);
            wasNew = userProfiles.find (stem) == userProfiles.end();
            userProfiles[stem] = sp;
            migratedUserMappings.erase (stem);
        }
        // Re-resolve devices.
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput)
                continue;
            auto resolved = resolveForRecord (rec);
            std::shared_ptr<const Mapping> previous;
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = resolved;
            }
            if (resolved != previous)
                affected.push_back (rec.deviceId);
        }

        fireUserMappingSaved (juce::String (filename), SaveResult::Ok);
        if (wasNew)
            fireMappingAdded (stem);
        for (auto id : affected)
            fireActiveMappingChanged (id);

        return SaveResult::Ok;
    }

    //--------------------------------------------------------------------------
    std::vector<MappingLoadError> MappingStore::getLoadErrors() const
    {
        std::shared_lock lock (stateMutex);
        return loadErrors;
    }

    std::vector<MigratedMappingInfo> MappingStore::getMigratedMappings() const
    {
        std::shared_lock lock (stateMutex);

        std::vector<MigratedMappingInfo> out;
        out.reserve (migratedUserMappings.size());

        for (const auto& [mappingId, record] : migratedUserMappings)
        {
            MigratedMappingInfo info;
            info.mappingId    = mappingId;
            info.sourcePath   = record.sourcePath;
            info.stepsApplied = record.stepsApplied;

            for (const auto& [deviceId, mapping] : activeByDevice)
            {
                if (idForMappingLocked (mapping) == mappingId)
                {
                    info.deviceId = deviceId;
                    break;
                }
            }

            out.push_back (std::move (info));
        }

        std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b)
        {
            return a.mappingId.compareIgnoreCase (b.mappingId) < 0;
        });

        return out;
    }

    bool MappingStore::saveMigratedCopy (const juce::String& mappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        MigratedMappingRecord record;
        {
            std::shared_lock lock (stateMutex);
            const auto it = migratedUserMappings.find (mappingId);
            if (it == migratedUserMappings.end())
                return false;
            record = it->second;
        }

        if (record.sourcePath.isEmpty())
            return false;

        const juce::File target (record.sourcePath);
        const auto tmp = target.getSiblingFile (target.getFileName() + ".tmp");

        if (tmp.existsAsFile())
            tmp.deleteFile();

        const auto json = juce::JSON::toString (record.migratedJson, false);
        if (! tmp.replaceWithText (json))
            return false;

        if (! tmp.replaceFileIn (target))
        {
            tmp.deleteFile();
            return false;
        }

        {
            std::unique_lock lock (stateMutex);
            migratedUserMappings.erase (mappingId);
        }

        return true;
    }

    void MappingStore::reloadUserMappings()
    {
        {
            std::unique_lock lock (stateMutex);
            loadErrors.clear();
            userProfiles.clear();
            migratedUserMappings.clear();
        }
        userProfilesReady.store (false, std::memory_order_release);
        enumerateUserProfilesAsync();
    }

    void MappingStore::addListener (MappingStoreListener* l)
    {
        if (l != nullptr && std::find (listeners.begin(), listeners.end(), l) == listeners.end())
            listeners.push_back (l);
    }

    void MappingStore::removeListener (MappingStoreListener* l)
    {
        listeners.erase (std::remove (listeners.begin(), listeners.end(), l), listeners.end());
    }

    void MappingStore::waitForUserProfilesLoaded()
    {
        if (! loadJobDispatched.load (std::memory_order_acquire))
            return;
        if (threadPool != nullptr)
            threadPool->removeAllJobs (false, 5000);
    }

    void MappingStore::recordLoadError (juce::String path,
                                        juce::String message,
                                        MappingLoadError::Kind kind)
    {
        std::unique_lock lock (stateMutex);
        loadErrors.emplace_back (kind, std::move (path), std::move (message));
    }

    void MappingStore::fireUserProfilesLoaded()
    {
        for (auto* l : listeners) l->userProfilesLoaded();
    }

    void MappingStore::fireActiveMappingChanged (std::uint64_t id)
    {
        for (auto* l : listeners) l->activeMappingChanged (id);
    }

    void MappingStore::fireUserMappingSaved (juce::String filename, SaveResult r)
    {
        for (auto* l : listeners) l->userMappingSaved (filename, r);
    }

    void MappingStore::fireMappingAdded (juce::String id)
    {
        for (auto* l : listeners) l->mappingAdded (id);
    }

    void MappingStore::fireMappingRemoved (juce::String id)
    {
        for (auto* l : listeners) l->mappingRemoved (id);
    }

    //--------------------------------------------------------------------------
    // PRD-0048: profile-management & active-override helpers.

    juce::File MappingStore::activeOverridesFile() const
    {
        // Filename is `_active-mappings.json` (legacy from PRD-0048).
        // PRD-0051 §1.4 specifies a file named `_device_state.json` for the
        // same semantic ("persisted active-mapping per deviceId"). We keep
        // the existing filename: renaming would orphan all shipping user
        // state, and the schema + atomic write semantics already satisfy
        // PRD-0051's requirement. The legacy filename is therefore the
        // canonical on-disk identity for both PRDs.
        return userDir.getChildFile ("_active-mappings.json");
    }

    void MappingStore::loadActiveOverridesFromDisk()
    {
        const auto f = activeOverridesFile();
        if (! f.existsAsFile())
            return;

        const auto size = f.getSize();
        if (size <= 0 || size > kMaxFileSizeBytes)
        {
            recordLoadError (f.getFullPathName(), "active-overrides: size out of bounds");
            return;
        }

        const auto root = juce::JSON::parse (f.loadFileAsString());
        if (! root.isObject())
            return;

        const auto active = root.getProperty ("active", juce::var());
        if (! active.isObject())
            return;

        std::unordered_map<std::uint64_t, juce::String> loaded;
        if (auto* obj = active.getDynamicObject())
        {
            for (const auto& prop : obj->getProperties())
            {
                const juce::String keyStr = prop.name.toString();
                const auto         valStr = prop.value.toString();
                if (keyStr.isEmpty() || valStr.isEmpty())
                    continue;
                // Keys are hex, no 0x prefix.
                const auto deviceId = (std::uint64_t) keyStr.getHexValue64();
                if (deviceId == 0)
                    continue;
                loaded.emplace (deviceId, valStr);
            }
        }

        std::unique_lock lock (stateMutex);
        activeOverrideByDevice = std::move (loaded);
    }

    void MappingStore::saveActiveOverridesToDisk()
    {
        std::unordered_map<std::uint64_t, juce::String> snap;
        {
            std::shared_lock lock (stateMutex);
            snap = activeOverrideByDevice;
        }

        auto* obj    = new juce::DynamicObject();
        auto* active = new juce::DynamicObject();
        for (const auto& [deviceId, mappingId] : snap)
            active->setProperty (juce::Identifier (juce::String::toHexString ((juce::int64) deviceId)),
                                 mappingId);
        obj->setProperty ("version", 1);
        obj->setProperty ("active",  juce::var (active));

        const auto json = juce::JSON::toString (juce::var (obj), false);

        if (! userDir.exists())
            userDir.createDirectory();

        const auto target = activeOverridesFile();
        const auto tmp    = target.getSiblingFile (target.getFileName() + ".tmp");
        if (tmp.existsAsFile()) tmp.deleteFile();
        if (! tmp.replaceWithText (json))
            return;
        if (target.existsAsFile()) target.deleteFile();
        if (! tmp.moveFileTo (target))
            tmp.deleteFile();
    }

    std::shared_ptr<const Mapping> MappingStore::findMappingById (const juce::String& id) const
    {
        std::shared_lock lock (stateMutex);
        if (auto it = userProfiles.find (id); it != userProfiles.end())
            return it->second;
        if (auto it = bundledProfiles.find (id); it != bundledProfiles.end())
            return it->second;
        return {};
    }

    juce::String MappingStore::idForMappingLocked (const std::shared_ptr<const Mapping>& m) const
    {
        if (m == nullptr) return {};
        for (const auto& [id, mp] : bundledProfiles)
            if (mp == m) return id;
        for (const auto& [id, mp] : userProfiles)
            if (mp == m) return id;
        return {};
    }

    juce::String MappingStore::sanitiseFilenameStem (juce::StringRef raw) noexcept
    {
        const juce::String s = juce::String (raw).trim();
        if (s.isEmpty()) return {};

        juce::String out;
        out.preallocateBytes (s.getNumBytesAsUTF8());
        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '-' || c == '_')
                out += juce::String::charToString (c);
            else if (c == ' ' || c == '.')
                out += "-";
            // drop everything else.
        }
        // Strip leading dots/dashes and double-dot sequences.
        while (out.startsWithChar ('.') || out.startsWithChar ('-'))
            out = out.substring (1);
        while (out.contains (".."))
            out = out.replace ("..", "-");

        return out.substring (0, 64);
    }

    //--------------------------------------------------------------------------
    juce::String MappingStore::getActiveMappingIdForDevice (std::uint64_t deviceId) const
    {
        std::shared_lock lock (stateMutex);
        const auto it = activeByDevice.find (deviceId);
        if (it == activeByDevice.end()) return {};
        return idForMappingLocked (it->second);
    }

    std::vector<MappingProfileSummary>
    MappingStore::listAvailableMappings (std::uint64_t deviceId) const
    {
        MidiDeviceRecord rec;
        for (const auto& r : deviceManager.getDevices())
            if (r.deviceId == deviceId) { rec = r; break; }

        std::vector<MappingProfileSummary> out;
        std::shared_lock lock (stateMutex);

        out.reserve (bundledProfiles.size() + userProfiles.size());

        for (const auto& [id, m] : bundledProfiles)
        {
            MappingProfileSummary s;
            s.id            = id;
            s.displayName   = (m != nullptr && m->displayName.isNotEmpty()) ? m->displayName : id;
            s.origin        = MappingOrigin::Bundled;
            s.readOnly      = true;
            s.matchesDevice = (m != nullptr && rec.deviceId != 0 && deviceMatchAccepts (*m, rec));
            out.push_back (std::move (s));
        }
        for (const auto& [id, m] : userProfiles)
        {
            MappingProfileSummary s;
            s.id            = id;
            s.displayName   = (m != nullptr && m->displayName.isNotEmpty()) ? m->displayName : id;
            s.origin        = MappingOrigin::User;
            s.readOnly      = false;
            s.matchesDevice = (m != nullptr && rec.deviceId != 0 && deviceMatchAccepts (*m, rec));
            out.push_back (std::move (s));
        }

        std::sort (out.begin(), out.end(),
                   [] (const MappingProfileSummary& a, const MappingProfileSummary& b)
                   {
                       if (a.origin != b.origin)
                           return a.origin == MappingOrigin::Bundled;
                       return a.id.compareIgnoreCase (b.id) < 0;
                   });
        return out;
    }

    //--------------------------------------------------------------------------
    CreateUserCopyResult MappingStore::createUserCopy (juce::StringRef sourceMappingId,
                                                       juce::StringRef newDisplayName,
                                                       juce::String*   outNewMappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const juce::String srcId  = sourceMappingId;
        const juce::String rawNm  = newDisplayName;

        auto src = findMappingById (srcId);
        if (src == nullptr)
            return CreateUserCopyResult::UnknownSource;

        const juce::String stem = sanitiseFilenameStem (rawNm);
        if (stem.isEmpty())
            return CreateUserCopyResult::InvalidName;

        // Find a unique stem on disk + in-memory.
        juce::String finalStem = stem;
        {
            std::shared_lock lock (stateMutex);
            int suffix = 2;
            while (userProfiles.find (finalStem) != userProfiles.end()
                   || userDir.getChildFile (finalStem + ".json").existsAsFile()
                   || bundledProfiles.find (finalStem) != bundledProfiles.end())
            {
                finalStem = stem + "-" + juce::String (suffix++);
                if (suffix > 1024)
                    return CreateUserCopyResult::DuplicateName;
            }
        }

        // Build the new Mapping copy and stamp its displayName.
        Mapping copy = *src;
        copy.displayName = rawNm.trim();

        juce::var root;
        try { root = MappingSerializer::serialize (copy); }
        catch (...) { return CreateUserCopyResult::SerializeFailure; }

        const auto filename = finalStem + ".json";
        const auto target   = userDir.getChildFile (filename);
        const auto tmp      = userDir.getChildFile (filename + ".tmp");

        const auto json = juce::JSON::toString (root, false);
        if (tmp.existsAsFile()) tmp.deleteFile();
        if (! tmp.replaceWithText (json))
            return CreateUserCopyResult::IoFailure;
        if (target.existsAsFile()) target.deleteFile();
        if (! tmp.moveFileTo (target))
        {
            tmp.deleteFile();
            return CreateUserCopyResult::IoFailure;
        }

        // Re-parse from disk so the in-memory state matches the canonical file.
        auto pr = MappingParser::parse (root, target.getFullPathName());
        auto sp = std::make_shared<const Mapping> (std::move (pr.mapping));

        {
            std::unique_lock lock (stateMutex);
            userProfiles[finalStem] = sp;
            migratedUserMappings.erase (finalStem);
        }

        if (outNewMappingId != nullptr) *outNewMappingId = finalStem;
        fireMappingAdded (finalStem);
        return CreateUserCopyResult::Ok;
    }

    //--------------------------------------------------------------------------
    DeleteUserMappingResult MappingStore::deleteUserMapping (juce::StringRef mappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const juce::String id = mappingId;
        if (id.isEmpty())
            return DeleteUserMappingResult::UnknownMapping;

        bool existed = false;
        {
            std::unique_lock lock (stateMutex);
            if (auto it = userProfiles.find (id); it != userProfiles.end())
            {
                userProfiles.erase (it);
                migratedUserMappings.erase (id);
                existed = true;
            }
        }
        if (! existed)
            return DeleteUserMappingResult::UnknownMapping;

        const auto file = userDir.getChildFile (id + ".json");
        bool ioOk = true;
        if (file.existsAsFile() && ! file.deleteFile())
            ioOk = false;

        // Drop any active overrides referring to this id, collect affected devices.
        std::vector<std::uint64_t> overrideCleared;
        {
            std::unique_lock lock (stateMutex);
            for (auto it = activeOverrideByDevice.begin(); it != activeOverrideByDevice.end(); )
            {
                if (it->second == id)
                {
                    overrideCleared.push_back (it->first);
                    it = activeOverrideByDevice.erase (it);
                }
                else { ++it; }
            }
        }
        if (! overrideCleared.empty())
            saveActiveOverridesToDisk();

        // Re-resolve every active device (deletion can change auto-resolution too,
        // e.g. if the user profile was the active mapping by deviceMatch).
        std::vector<std::uint64_t> changed;
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput) continue;
            auto fresh = resolveForRecord (rec);
            std::shared_ptr<const Mapping> previous;
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = fresh;
            }
            if (fresh != previous)
                changed.push_back (rec.deviceId);
        }

        fireMappingRemoved (id);
        for (auto d : changed)
            fireActiveMappingChanged (d);

        return ioOk ? DeleteUserMappingResult::Ok : DeleteUserMappingResult::IoFailure;
    }

    //--------------------------------------------------------------------------
    SetActiveMappingResult MappingStore::setActiveMapping (std::uint64_t   deviceId,
                                                           juce::StringRef mappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        // Validate device.
        MidiDeviceRecord rec;
        for (const auto& r : deviceManager.getDevices())
            if (r.deviceId == deviceId) { rec = r; break; }
        if (rec.deviceId == 0)
            return SetActiveMappingResult::UnknownDevice;

        const juce::String id = mappingId;

        if (id.isNotEmpty())
        {
            // Validate mapping exists.
            std::shared_lock lock (stateMutex);
            if (userProfiles.find (id) == userProfiles.end()
                && bundledProfiles.find (id) == bundledProfiles.end())
                return SetActiveMappingResult::UnknownMapping;
        }

        // Apply override (or clear when empty).
        std::shared_ptr<const Mapping> previous;
        {
            std::unique_lock lock (stateMutex);
            previous = activeByDevice[deviceId];
            if (id.isEmpty())
                activeOverrideByDevice.erase (deviceId);
            else
                activeOverrideByDevice[deviceId] = id;
        }

        saveActiveOverridesToDisk();

        // Re-resolve and update active table.
        auto fresh = resolveForRecord (rec);
        {
            std::unique_lock lock (stateMutex);
            activeByDevice[deviceId] = fresh;
        }

        if (fresh != previous)
            fireActiveMappingChanged (deviceId);

        return SetActiveMappingResult::Ok;
    }

    //--------------------------------------------------------------------------
    // PRD-0051: per-physical-USB-port disambiguation
    //--------------------------------------------------------------------------
    SetIdentifierHintResult
    MappingStore::setIdentifierHint (juce::StringRef                   mappingId,
                                     std::optional<juce::String>       identifierHint)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const juce::String id = mappingId;
        if (id.isEmpty())
            return SetIdentifierHintResult::UnknownMapping;

        // Bundled mappings are read-only. UI must duplicate first.
        {
            std::shared_lock lock (stateMutex);
            if (bundledProfiles.find (id) != bundledProfiles.end()
                && userProfiles.find (id) == userProfiles.end())
                return SetIdentifierHintResult::BundledNotEditable;
        }

        std::shared_ptr<const Mapping> original;
        {
            std::shared_lock lock (stateMutex);
            const auto it = userProfiles.find (id);
            if (it == userProfiles.end() || it->second == nullptr)
                return SetIdentifierHintResult::UnknownMapping;
            original = it->second;
        }

        // Build the modified copy. The UI path only writes literal hints; the
        // regex form is reserved for hand-authored JSON.
        Mapping edited = *original;
        edited.deviceMatch.identifierHintLiteral.reset();
        edited.deviceMatch.identifierHintRegexSrc.reset();
        edited.deviceMatch.identifierHintRegexCache.reset();
        if (identifierHint.has_value() && identifierHint->isNotEmpty())
            edited.deviceMatch.identifierHintLiteral = *identifierHint;

        juce::var root;
        try { root = MappingSerializer::serialize (edited); }
        catch (...) { return SetIdentifierHintResult::SerializeFailure; }

        const auto json     = juce::JSON::toString (root, false);
        const auto target   = userDir.getChildFile (id + ".json");
        const auto tmp      = userDir.getChildFile (id + ".json.tmp");

        if (tmp.existsAsFile()) tmp.deleteFile();
        if (! tmp.replaceWithText (json))
            return SetIdentifierHintResult::IoFailure;
        if (target.existsAsFile()) target.deleteFile();
        if (! tmp.moveFileTo (target))
        {
            tmp.deleteFile();
            return SetIdentifierHintResult::IoFailure;
        }

        // Re-parse from canonical disk source so in-memory matches the file.
        auto pr = MappingParser::parse (root, target.getFullPathName());
        auto sp = std::make_shared<const Mapping> (std::move (pr.mapping));

        {
            std::unique_lock lock (stateMutex);
            userProfiles[id] = sp;
        }

        // Re-resolve every device — changing an identifierHint can promote
        // or demote this mapping for multiple devices simultaneously.
        std::vector<std::uint64_t> affected;
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput) continue;
            auto fresh = resolveForRecord (rec);
            std::shared_ptr<const Mapping> previous;
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = fresh;
            }
            if (fresh != previous)
                affected.push_back (rec.deviceId);
        }

        for (auto did : affected)
            fireActiveMappingChanged (did);

        return SetIdentifierHintResult::Ok;
    }

    SwapIdentifierHintsResult
    MappingStore::swapIdentifierHints (std::uint64_t deviceA, std::uint64_t deviceB)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        if (deviceA == 0 || deviceB == 0 || deviceA == deviceB)
            return SwapIdentifierHintsResult::UnknownDevice;

        const auto idA = getActiveMappingIdForDevice (deviceA);
        const auto idB = getActiveMappingIdForDevice (deviceB);

        if (idA.isEmpty() || idB.isEmpty())
            return SwapIdentifierHintsResult::UnknownDevice;
        if (idA == idB)
            return SwapIdentifierHintsResult::SameMapping;

        // Both sides must be user mappings (bundled are read-only).
        {
            std::shared_lock lock (stateMutex);
            const bool aIsUser = userProfiles.find (idA) != userProfiles.end();
            const bool bIsUser = userProfiles.find (idB) != userProfiles.end();
            if (! aIsUser || ! bIsUser)
                return SwapIdentifierHintsResult::OneSideIsBundled;
        }

        // Snapshot the two old hints for rollback.
        std::optional<juce::String> oldHintA, oldHintB;
        {
            std::shared_lock lock (stateMutex);
            const auto& a = userProfiles[idA]->deviceMatch;
            const auto& b = userProfiles[idB]->deviceMatch;
            oldHintA = a.identifierHintLiteral;
            oldHintB = b.identifierHintLiteral;
        }

        // Write A with B's hint.
        const auto rA = setIdentifierHint (idA, oldHintB);
        if (rA == SetIdentifierHintResult::SerializeFailure)
            return SwapIdentifierHintsResult::SerializeFailure;
        if (rA == SetIdentifierHintResult::IoFailure)
            return SwapIdentifierHintsResult::IoFailure;
        if (rA != SetIdentifierHintResult::Ok)
            return SwapIdentifierHintsResult::UnknownDevice;

        // Write B with A's hint.
        const auto rB = setIdentifierHint (idB, oldHintA);
        if (rB != SetIdentifierHintResult::Ok)
        {
            // Best-effort rollback: restore A's original hint.
            (void) setIdentifierHint (idA, oldHintA);
            if (rB == SetIdentifierHintResult::SerializeFailure)
                return SwapIdentifierHintsResult::SerializeFailure;
            return SwapIdentifierHintsResult::IoFailure;
        }

        return SwapIdentifierHintsResult::Ok;
    }

    //--------------------------------------------------------------------------
    // PRD-0050: Import / export support.
    //--------------------------------------------------------------------------
    std::shared_ptr<const Mapping> MappingStore::getMappingById (juce::StringRef id) const
    {
        return findMappingById (juce::String (id));
    }

    bool MappingStore::userMappingExists (juce::StringRef stem) const
    {
        const juce::String s = stem;
        if (s.isEmpty())
            return false;
        {
            std::shared_lock lock (stateMutex);
            if (userProfiles.find (s) != userProfiles.end())
                return true;
        }
        return userDir.getChildFile (s + ".json").existsAsFile();
    }

    juce::String MappingStore::getActiveMappingDisplayNameForDevice (std::uint64_t deviceId) const
    {
        std::shared_lock lock (stateMutex);
        const auto it = activeByDevice.find (deviceId);
        if (it == activeByDevice.end() || it->second == nullptr)
            return {};
        if (it->second->displayName.isNotEmpty())
            return it->second->displayName;
        return idForMappingLocked (it->second);
    }

    RegisterImportedResult
    MappingStore::registerImportedMapping (const Mapping&  mapping,
                                           juce::StringRef filenameStem,
                                           bool            overwriteExisting)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        RegisterImportedResult out;

        const juce::String stem = sanitiseFilenameStem (filenameStem);
        if (stem.isEmpty())
        {
            out.status = RegisterImportedResult::Status::InvalidStem;
            return out;
        }

        // Bundled ids are reserved.
        {
            std::shared_lock lock (stateMutex);
            if (bundledProfiles.find (stem) != bundledProfiles.end())
            {
                out.status = RegisterImportedResult::Status::InvalidStem;
                return out;
            }
        }

        const auto filename = stem + ".json";
        const auto target   = userDir.getChildFile (filename);
        const auto tmp      = userDir.getChildFile (filename + ".tmp");

        const bool diskExists = target.existsAsFile();
        bool       memExists  = false;
        {
            std::shared_lock lock (stateMutex);
            memExists = (userProfiles.find (stem) != userProfiles.end());
        }

        if ((diskExists || memExists) && ! overwriteExisting)
        {
            out.status = RegisterImportedResult::Status::ConflictNotOverwritten;
            return out;
        }

        if (! userDir.exists())
        {
            const auto cr = userDir.createDirectory();
            if (! cr.wasOk())
            {
                out.status = RegisterImportedResult::Status::IoFailure;
                return out;
            }
        }

        juce::var root;
        try { root = MappingSerializer::serialize (mapping); }
        catch (...)
        {
            out.status = RegisterImportedResult::Status::SerializeFailure;
            return out;
        }

        const auto json = juce::JSON::toString (root, false);

        if (tmp.existsAsFile())
            tmp.deleteFile();
        if (! tmp.replaceWithText (json))
        {
            tmp.deleteFile();
            out.status = RegisterImportedResult::Status::IoFailure;
            return out;
        }
        if (target.existsAsFile())
            target.deleteFile();
        if (! tmp.moveFileTo (target))
        {
            tmp.deleteFile();
            out.status = RegisterImportedResult::Status::IoFailure;
            return out;
        }

        // Re-parse from the canonical on-disk JSON so the in-memory state
        // matches what a fresh process would load.
        auto pr = MappingParser::parse (root, target.getFullPathName());
        auto sp = std::make_shared<const Mapping> (std::move (pr.mapping));

        const bool wasNew = ! memExists;
        {
            std::unique_lock lock (stateMutex);
            userProfiles[stem] = sp;
            migratedUserMappings.erase (stem);
        }

        // Re-resolve devices: a freshly-added mapping may take precedence
        // over a previously-active bundled profile via deviceMatch.
        std::vector<std::uint64_t> affected;
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput) continue;
            auto fresh = resolveForRecord (rec);
            std::shared_ptr<const Mapping> previous;
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = fresh;
            }
            if (fresh != previous)
                affected.push_back (rec.deviceId);
        }

        if (wasNew)
            fireMappingAdded (stem);
        for (auto id : affected)
            fireActiveMappingChanged (id);

        out.status    = RegisterImportedResult::Status::Ok;
        out.finalStem = stem;
        return out;
    }
}
