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
    /** PRD-0048: where a mapping came from. */
    enum class MappingOrigin : std::uint8_t
    {
        Bundled, // Embedded in the binary; read-only.
        User     // On-disk JSON in the user mapping directory; editable.
    };

    /** PRD-0048: row in the per-device profile dropdown. */
    struct MappingProfileSummary
    {
        juce::String  id;            // Stable id (bundled name or user filename stem).
        juce::String  displayName;   // Human label; falls back to id when empty.
        MappingOrigin origin         { MappingOrigin::User };
        bool          readOnly       { false };
        bool          matchesDevice  { false }; // true if the deviceMatch regex accepts this device.
    };

    //--------------------------------------------------------------------------
    enum class CreateUserCopyResult : std::uint8_t
    {
        Ok,
        UnknownSource,
        InvalidName,
        DuplicateName,
        IoFailure,
        SerializeFailure,
    };

    enum class DeleteUserMappingResult : std::uint8_t
    {
        Ok,
        UnknownMapping,
        IoFailure,
    };

    enum class SetActiveMappingResult : std::uint8_t
    {
        Ok,
        UnknownDevice,
        UnknownMapping,
        IoFailure,   // override persistence failed; in-memory state still updated.
    };

    //--------------------------------------------------------------------------
    class MappingStoreListener
    {
    public:
        virtual ~MappingStoreListener() = default;

        virtual void userProfilesLoaded() {}
        virtual void activeMappingChanged (std::uint64_t /*deviceId*/) {}
        virtual void userMappingSaved (juce::String /*filename*/, SaveResult) {}

        // PRD-0048: emitted after the in-memory user-profile table gains
        // (`mappingAdded`) or loses (`mappingRemoved`) an entry.
        virtual void mappingAdded   (juce::String /*mappingId*/) {}
        virtual void mappingRemoved (juce::String /*mappingId*/) {}
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

        /** PRD-0048 Phase 8: Reload user mappings from disk.  Clears the
            current `loadErrors` set, re-scans the user mapping directory,
            and fires `userProfilesLoaded()` when complete.  Bound to the
            "Reload" action in the load-error banner. */
        void reloadUserMappings();

        // ---- PRD-0048: profile management & per-device active override ----

        /** Returns the stable mapping id (bundled name or user filename stem)
            of the mapping currently resolved for the device, or empty string
            if no mapping is resolved.  Thread-safe. */
        juce::String getActiveMappingIdForDevice (std::uint64_t deviceId) const;

        /** Returns the list of all available mappings (bundled + user) for the
            device, each tagged with origin / read-only / device-match flag.
            Stable-sorted: bundled first, then user, alphabetical by id. */
        std::vector<MappingProfileSummary> listAvailableMappings (std::uint64_t deviceId) const;

        /** Deep-copy `sourceMappingId` (bundled or user) into a new user file.
            `newDisplayName` is sanitised into a filesystem-safe stem; an
            integer suffix is appended on collision. On success, fires
            `mappingAdded` with the new id; the new mapping is NOT auto-set
            as active for any device. The new id is written to
            `outNewMappingId` when non-null. */
        CreateUserCopyResult createUserCopy (juce::StringRef sourceMappingId,
                                             juce::StringRef newDisplayName,
                                             juce::String*   outNewMappingId = nullptr);

        /** Delete a user mapping by id (filename stem). Bundled mappings
            cannot be deleted. Removes the on-disk file, the in-memory entry,
            and any per-device active overrides pointing at it (those devices
            re-resolve to their bundled default). Fires `mappingRemoved` and
            `activeMappingChanged` for each affected device. */
        DeleteUserMappingResult deleteUserMapping (juce::StringRef mappingId);

        /** Pin a specific mapping as the active mapping for a device. The
            override is persisted to disk so it survives restarts. Use an
            empty string to clear the override (re-enable automatic
            resolution). Fires `activeMappingChanged` when the resolved
            mapping changes. */
        SetActiveMappingResult setActiveMapping (std::uint64_t   deviceId,
                                                 juce::StringRef mappingId);

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
        void fireMappingAdded         (juce::String);
        void fireMappingRemoved       (juce::String);

        // PRD-0048: active-override persistence.
        juce::File activeOverridesFile() const;
        void       loadActiveOverridesFromDisk();   // sync; called from ctor.
        void       saveActiveOverridesToDisk();     // sync; best-effort.

        // Look up the in-memory mapping for an id (user first, then bundled).
        // Caller must NOT hold the writer lock.
        std::shared_ptr<const Mapping> findMappingById (const juce::String& id) const;

        // Returns the stable id for a Mapping pointer, or empty if unknown.
        // Caller must hold at least a reader lock.
        juce::String idForMappingLocked (const std::shared_ptr<const Mapping>& m) const;

        // Sanitises a free-form display name into a filesystem-safe stem.
        // Returns empty on completely-invalid input.
        static juce::String sanitiseFilenameStem (juce::StringRef raw) noexcept;

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

        // PRD-0048: per-device explicit override (mappingId). When present,
        // resolveForRecord returns the override before falling through to
        // automatic device-match resolution. Empty string is never stored.
        std::unordered_map<std::uint64_t, juce::String> activeOverrideByDevice;

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
