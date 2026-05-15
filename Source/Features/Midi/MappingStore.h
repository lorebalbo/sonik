#pragma once
//==============================================================================
// PRD-0043: Mapping Storage, Loading and Crash-Safe Defaults.
//
// Owns the lifetime of all Mapping instances: the two bundled JSON profiles
// embedded in the binary, every user-edited JSON file found in the user
// mapping directory, and a per-device "active" snapshot that downstream
// PRDs read on any thread.
//
// Threading model:
//   * Constructor (Message thread): synchronously parses bundled profiles,
//     creates the user mapping directory if missing, enqueues a background
//     ThreadPoolJob to enumerate user files. Does NOT block the caller on
//     disk I/O.
//   * Background ThreadPoolJob: loads + parses every *.json in the user
//     directory. On completion, schedules a Message-thread callAsync that
//     swaps userProfiles under the writer lock and fires
//     userProfilesLoaded().
//   * getActiveMappingForDevice(): callable from ANY thread. Takes the
//     shared_mutex in shared mode for nanoseconds, copies a shared_ptr,
//     releases.
//   * saveUserMapping(): Message thread only.
//==============================================================================

#include "DeviceListChangeListener.h"
#include "MappingTypes.h"
#include "MidiDeviceRecord.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace sonik::midi
{
    class MidiDeviceManager;

    //--------------------------------------------------------------------------
    /** Diagnostic record for files that failed to load.  Surfaced via
        getLoadErrors() so the MIDI Learn UI can display a banner. */
    struct MappingLoadError
    {
        juce::String sourcePath;
        juce::String message;
    };

    //--------------------------------------------------------------------------
    enum class SaveResult : std::uint8_t
    {
        Ok,
        InvalidFilename,
        IoFailure,
        SerializeFailure,
    };

    //--------------------------------------------------------------------------
    class MappingStoreListener
    {
    public:
        virtual ~MappingStoreListener() = default;

        virtual void userProfilesLoaded() {}
        virtual void activeMappingChanged (std::uint64_t /*deviceId*/) {}
        virtual void userMappingSaved (juce::String /*filename*/, SaveResult) {}
    };

    //--------------------------------------------------------------------------
    class MappingStore final : public DeviceListChangeListener
    {
    public:
        /** Construct attached to a device manager.  If `loadUserProfilesAsync`
            is false (used by tests), user-profile enumeration runs
            synchronously in the constructor. */
        MappingStore (MidiDeviceManager& deviceManager,
                      juce::File         userMappingDirectory       = defaultUserMappingDirectory(),
                      bool               loadUserProfilesAsync      = true);

        ~MappingStore() override;

        MappingStore (const MappingStore&)            = delete;
        MappingStore& operator= (const MappingStore&) = delete;

        // ---- Thread-safe accessor ------------------------------------------

        /** Returns a snapshot pointer that the caller may keep arbitrarily
            long; the Mapping itself is immutable.  Returns nullptr if no
            mapping has been resolved for the device yet. */
        std::shared_ptr<const Mapping> getActiveMappingForDevice (std::uint64_t deviceId) const;

        // ---- Message thread -----------------------------------------------

        SaveResult saveUserMapping (const Mapping& mapping, juce::StringRef filename);

        std::vector<MappingLoadError> getLoadErrors() const;

        void addListener    (MappingStoreListener*);
        void removeListener (MappingStoreListener*);

        /** Blocks until the background user-profile load has finished.
            Test-only helper.  No-op once already loaded. */
        void waitForUserProfilesLoaded();

        // ---- DeviceListChangeListener -------------------------------------

        void midiDeviceAdded   (std::uint64_t deviceId) override;
        void midiDeviceRemoved (std::uint64_t deviceId) override;

        // ---- Helpers -------------------------------------------------------

        static juce::File defaultUserMappingDirectory();

        /** Validates that filename has no path separators / `..` / NUL,
            and ends in `.json`. */
        static bool isValidUserMappingFilename (juce::StringRef filename) noexcept;

    private:
        struct BundledProfile
        {
            juce::String                  id;
            std::shared_ptr<const Mapping> mapping;
        };

        void loadBundledProfiles();
        void enumerateUserProfilesAsync();
        void enumerateUserProfilesNow();
        void onUserProfilesLoaded();

        // Picks user profile (first match), then bundled by-device, then generic.
        // Caller must NOT hold the writer lock.
        std::shared_ptr<const Mapping> resolveForRecord (const MidiDeviceRecord& rec) const;

        // Returns true if the mapping's deviceMatch regexes accept the record.
        static bool deviceMatchAccepts (const Mapping&, const MidiDeviceRecord&) noexcept;

        void recordLoadError (juce::String path, juce::String message);

        void fireUserProfilesLoaded();
        void fireActiveMappingChanged (std::uint64_t);
        void fireUserMappingSaved     (juce::String, SaveResult);

        MidiDeviceManager& deviceManager;
        juce::File         userDir;

        mutable std::shared_mutex stateMutex;

        // Bundled profiles are immutable after construction.  Keyed by a
        // stable id ("behringer-ddm4000", "generic-midi").
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> bundledProfiles;
        juce::String                                                     genericProfileId;

        // User profiles: keyed by filename stem.  Replaced wholesale by the
        // background loader and by saveUserMapping.
        std::unordered_map<juce::String, std::shared_ptr<const Mapping>> userProfiles;

        std::unordered_map<std::uint64_t, std::shared_ptr<const Mapping>> activeByDevice;

        std::vector<MappingLoadError> loadErrors;

        // Listeners are mutated only on the Message thread.
        std::vector<MappingStoreListener*> listeners;

        std::atomic<bool> userProfilesReady { false };
        std::unique_ptr<juce::ThreadPool>                threadPool;

        // Set when the background job has been enqueued (or run inline).
        // Used by waitForUserProfilesLoaded.
        std::atomic<bool> loadJobDispatched { false };
    };
}
