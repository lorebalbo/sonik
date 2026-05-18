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
//   * Background ThreadPoolJob: enumerates user files and reads their text.
//     On completion, schedules a Message-thread callAsync that migrates,
//     parses, swaps userProfiles under the writer lock, and fires
//     userProfilesLoaded().
//   * getActiveMappingForDevice(): callable from ANY thread. Takes the
//     shared_mutex in shared mode for nanoseconds, copies a shared_ptr,
//     releases.
//   * saveUserMapping(): Message thread only.
//==============================================================================

#include "DeviceListChangeListener.h"
#include "MappingTypes.h"
#include "Migrations/MigrationRegistry.h"
#include "MidiDeviceRecord.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sonik::midi
{
    class MidiDeviceManager;

    //--------------------------------------------------------------------------
    /** Diagnostic record for files that failed to load.  Surfaced via
        getLoadErrors() so the MIDI Learn UI can display a banner. */
    struct MappingLoadError
    {
        enum class Kind : std::uint8_t
        {
            IoFailure,
            JsonParseFailure,
            ValidationError,
            MigrationFailed,
            UnsupportedSchemaVersion,
            MigrationProducedInvalidOutput,
        };

        MappingLoadError() = default;

        MappingLoadError (juce::String path, juce::String text)
            : sourcePath (std::move (path)),
              message    (std::move (text))
        {}

        MappingLoadError (Kind errorKind, juce::String path, juce::String text)
            : kind       (errorKind),
              sourcePath (std::move (path)),
              message    (std::move (text))
        {}

        Kind         kind { Kind::ValidationError };
        juce::String sourcePath;
        juce::String message;
        std::optional<MigrationError>  migrationError;
        std::optional<MigrationStep>   lastMigrationStep;
        std::optional<ValidationError> parserError;
        int fromVersion         { 0 };
        int maxSupportedVersion { kCurrentSchemaVersion };
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

    struct MigratedMappingInfo
    {
        std::uint64_t              deviceId { 0 };
        juce::String               mappingId;
        juce::String               sourcePath;
        std::vector<MigrationStep> stepsApplied;
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
    /** PRD-0050: result of registering an imported (already-parsed) mapping. */
    struct RegisterImportedResult
    {
        enum class Status : std::uint8_t
        {
            Ok,
            InvalidStem,
            ConflictNotOverwritten,
            IoFailure,
            SerializeFailure,
        };

        Status       status { Status::Ok };
        juce::String finalStem;
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
                      bool               loadUserProfilesAsync      = true,
                      MigrationRegistry  migrationRegistry          = {},
                      int                currentSchemaVersion       = kCurrentSchemaVersion);

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

        std::vector<MigratedMappingInfo> getMigratedMappings() const;

        bool saveMigratedCopy (const juce::String& mappingId);

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

        // ---- PRD-0050: Import / export support ----------------------------

        /** Returns the in-memory Mapping for the given id (user profile takes
            precedence over a bundled profile with the same id). nullptr if
            no such mapping exists. Thread-safe. */
        std::shared_ptr<const Mapping> getMappingById (juce::StringRef id) const;

        /** Returns true if a user mapping with the given filename stem
            currently exists in memory or on disk. */
        bool userMappingExists (juce::StringRef stem) const;

        /** Returns the displayName of the currently-active mapping for
            `deviceId`, or empty string if no mapping is resolved.
            Thread-safe. */
        juce::String getActiveMappingDisplayNameForDevice (std::uint64_t deviceId) const;

        /** Sanitise a free-form mapping display name into a filesystem-safe
            stem. Exposed for the import dialog so it can preview the final
            on-disk filename before commit. */
        static juce::String sanitiseFilenameStem (juce::StringRef raw) noexcept;

        /** PRD-0050: atomically write an imported mapping to disk and
            register it with the in-memory store. Performs MappingSerializer
            serialisation + `.tmp` + rename. Fires `mappingAdded` on
            success. Message thread only. */
        RegisterImportedResult registerImportedMapping (const Mapping&  mapping,
                                                        juce::StringRef filenameStem,
                                                        bool            overwriteExisting);

        /** Read-only access to the migration registry the store was
            constructed with. Used by PRD-0050's import pipeline so the same
            v(N) → v(N+1) ladder is applied on import as on load. */
        const MigrationRegistry& getMigrationRegistry() const noexcept { return migrations; }

        /** Returns the schema version the store migrates *to*. PRD-0050's
            import service forwards this to MigrationRegistry::apply so a
            bundle exported on a newer build is rejected cleanly. */
        int getSchemaVersionTarget() const noexcept { return schemaVersionTarget; }

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

        struct UserProfileSource
        {
            juce::File   file;
            juce::String jsonText;
        };

        struct MappingJsonLoadResult
        {
            Mapping                       mapping;
            juce::var                     migratedJson;
            std::vector<MigrationStep>    stepsApplied;
            std::vector<MappingLoadError> errors;
            bool                          loaded { false };
        };

        struct MigratedMappingRecord
        {
            juce::String               mappingId;
            juce::String               sourcePath;
            std::vector<MigrationStep> stepsApplied;
            juce::var                  migratedJson;
        };

        void loadBundledProfiles();
        void enumerateUserProfilesAsync();
        void enumerateUserProfilesNow();
        std::vector<UserProfileSource> collectUserProfileSources (std::vector<MappingLoadError>& errors) const;
        void loadUserProfileSources (std::vector<UserProfileSource> sources,
                                     std::vector<MappingLoadError> errors);
        void onUserProfilesLoaded();

        MappingJsonLoadResult loadMappingFromJson (const juce::var& root,
                                                   const juce::String& sourcePath) const;

        // Picks user profile (first match), then bundled by-device, then generic.
        // Caller must NOT hold the writer lock.
        std::shared_ptr<const Mapping> resolveForRecord (const MidiDeviceRecord& rec) const;

        // Returns true if the mapping's deviceMatch regexes accept the record.
        static bool deviceMatchAccepts (const Mapping&, const MidiDeviceRecord&) noexcept;

        void recordLoadError (juce::String path,
                              juce::String message,
                              MappingLoadError::Kind kind = MappingLoadError::Kind::IoFailure);

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

        // (sanitiseFilenameStem is public; declared above.)

        MidiDeviceManager& deviceManager;
        juce::File         userDir;
        MigrationRegistry  migrations;
        int                schemaVersionTarget { kCurrentSchemaVersion };

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

        std::unordered_map<juce::String, MigratedMappingRecord> migratedUserMappings;

        // Listeners are mutated only on the Message thread.
        std::vector<MappingStoreListener*> listeners;

        std::atomic<bool> userProfilesReady { false };
        std::unique_ptr<juce::ThreadPool>                threadPool;

        // Set when the background job has been enqueued (or run inline).
        // Used by waitForUserProfilesLoaded.
        std::atomic<bool> loadJobDispatched { false };
    };
}
