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
        const char*    kReloopId         = "reloop-contour-interface-edition";
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
                                bool               loadUserProfilesAsync)
        : deviceManager (mgr),
          userDir       (std::move (dir))
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
            { kReloopId,  BinaryData::reloopcontourinterfaceedition_json,
                          BinaryData::reloopcontourinterfaceedition_jsonSize },
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

        threadPool->addJob ([this]() noexcept
        {
            try { enumerateUserProfilesNow(); }
            catch (...) { /* never propagate */ }
            juce::MessageManager::callAsync ([this]() { onUserProfilesLoaded(); });
        });
    }

    void MappingStore::enumerateUserProfilesNow()
    {
        if (! userDir.exists() || ! userDir.isDirectory())
            return;

        juce::Array<juce::File> files;
        userDir.findChildFiles (files, juce::File::findFiles, false, "*.json");

        // Deterministic order for diagnostics.
        std::sort (files.begin(), files.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });

        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> loaded;
        std::vector<MappingLoadError> errors;

        for (const auto& file : files)
        {
            const auto size = file.getSize();
            if (size <= 0 || size > kMaxFileSizeBytes)
            {
                errors.push_back ({ file.getFullPathName(),
                                    "skipped: size out of bounds (" + juce::String (size) + " bytes)" });
                continue;
            }

            const auto text = file.loadFileAsString();
            const auto root = juce::JSON::parse (text);
            if (root.isVoid())
            {
                errors.push_back ({ file.getFullPathName(), "JSON parse failure" });
                continue;
            }

            auto pr = MappingParser::parse (root, file.getFullPathName());
            for (const auto& e : pr.errors)
                errors.push_back ({ file.getFullPathName(),
                                    "validation: " + e.detail });

            // Accept partially-valid mapping; loader is tolerant per PRD-0042.
            loaded.emplace (file.getFileNameWithoutExtension(),
                            std::make_shared<const Mapping> (std::move (pr.mapping)));
        }

        {
            std::unique_lock lock (stateMutex);
            userProfiles = std::move (loaded);
            for (auto&& e : errors)
                loadErrors.push_back (std::move (e));
        }
        userProfilesReady.store (true, std::memory_order_release);
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
            {
                std::unique_lock lock (stateMutex);
                previous = activeByDevice[rec.deviceId];
                activeByDevice[rec.deviceId] = newMapping;
            }
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

    std::shared_ptr<const Mapping> MappingStore::resolveForRecord (const MidiDeviceRecord& rec) const
    {
        // Snapshot containers under shared lock.
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> userSnap;
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> bundledSnap;
        juce::String genericId;
        {
            std::shared_lock lock (stateMutex);
            userSnap    = userProfiles;
            bundledSnap = bundledProfiles;
            genericId   = genericProfileId;
        }

        for (const auto& [stem, mapping] : userSnap)
        {
            if (mapping != nullptr && deviceMatchAccepts (*mapping, rec))
                return mapping;
        }

        for (const auto& [id, mapping] : bundledSnap)
        {
            if (id == genericId)
                continue;
            if (mapping != nullptr && deviceMatchAccepts (*mapping, rec))
                return mapping;
        }

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
        {
            std::unique_lock lock (stateMutex);
            activeByDevice[deviceId] = mapping;
        }
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
        JUCE_ASSERT_MESSAGE_THREAD

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

        std::vector<std::uint64_t> affected;
        {
            std::unique_lock lock (stateMutex);
            userProfiles[target.getFileNameWithoutExtension()] = sp;
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

    void MappingStore::recordLoadError (juce::String path, juce::String message)
    {
        std::unique_lock lock (stateMutex);
        loadErrors.push_back ({ std::move (path), std::move (message) });
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
}
