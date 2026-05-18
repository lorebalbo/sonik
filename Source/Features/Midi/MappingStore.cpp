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
            // PRD-0048: underscore-prefixed files are internal state (e.g.
            // `_active-mappings.json`), not user-authored mappings.
            if (file.getFileName().startsWithChar ('_'))
                continue;

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

        // PRD-0048: explicit per-device override wins.
        if (overrideId.isNotEmpty())
        {
            if (auto it = userSnap.find (overrideId); it != userSnap.end() && it->second != nullptr)
                return it->second;
            if (auto it = bundledSnap.find (overrideId); it != bundledSnap.end() && it->second != nullptr)
                return it->second;
            // Stale override (referenced mapping no longer exists). Fall through
            // to automatic resolution; the override is cleaned up lazily by
            // deleteUserMapping; we do not mutate here under a reader lock.
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

        const auto stem = target.getFileNameWithoutExtension();
        bool wasNew = false;
        std::vector<std::uint64_t> affected;
        {
            std::unique_lock lock (stateMutex);
            wasNew = userProfiles.find (stem) == userProfiles.end();
            userProfiles[stem] = sp;
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

    void MappingStore::reloadUserMappings()
    {
        {
            std::unique_lock lock (stateMutex);
            loadErrors.clear();
            userProfiles.clear();
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
        JUCE_ASSERT_MESSAGE_THREAD

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
        }

        if (outNewMappingId != nullptr) *outNewMappingId = finalStem;
        fireMappingAdded (finalStem);
        return CreateUserCopyResult::Ok;
    }

    //--------------------------------------------------------------------------
    DeleteUserMappingResult MappingStore::deleteUserMapping (juce::StringRef mappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        const juce::String id = mappingId;
        if (id.isEmpty())
            return DeleteUserMappingResult::UnknownMapping;

        bool existed = false;
        {
            std::unique_lock lock (stateMutex);
            if (auto it = userProfiles.find (id); it != userProfiles.end())
            {
                userProfiles.erase (it);
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
        JUCE_ASSERT_MESSAGE_THREAD

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
}
