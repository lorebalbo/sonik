#pragma once

#include "StemData.h"
#include "../Deck/Database/TrackDatabase.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <optional>
#include <set>

/// Manages the on-disk stem cache and stems_data table in SQLite.
/// Cache location: ~/Library/Application Support/Sonik/Stems/<content_hash>/
/// (PERSISTENT — next to the database. Previously ~/Library/Caches, which macOS
///  treats as purgeable, so stems vanished when the OS reclaimed that cache.)
///
/// Responsibilities:
///  - Check cache for existing stem separations (fast DB query on message thread)
///  - Write separated stems as 32-bit float WAV files (background thread)
///  - Load cached stems from disk (background thread)
///  - Evict old entries when cache exceeds size limit
///  - Startup cleanup of orphan and incomplete entries
class StemCache
{
public:
    /// @param database  Reference to the app's TrackDatabase (for stems_data table).
    /// @param maxCacheSizeBytes  Maximum total cache size in bytes (default 10 GB).
    explicit StemCache (TrackDatabase& database,
                        int64_t maxCacheSizeBytes = 10LL * 1024 * 1024 * 1024);
    ~StemCache() = default;

    StemCache (const StemCache&) = delete;
    StemCache& operator= (const StemCache&) = delete;

    /// Returns the stem cache root directory (persistent, in Application Support).
    static juce::File getCacheDirectory();

    /// The pre-2026 cache root (~/Library/Caches/Sonik/Stems). Retained only so
    /// existing stems can be migrated to the persistent location on startup.
    static juce::File legacyCacheDirectory();

    /// Check if stems are cached for the given content hash.
    /// Verifies both the DB record and the presence of all 4 WAV files on disk.
    /// Runs on the message thread (fast DB query + file existence check).
    bool hasCachedStems (const juce::String& contentHash) const;

    /// Load cached stem WAV files from disk.
    /// Returns nullptr on failure.
    /// Runs on a background thread.
    StemData::Ptr loadCachedStems (const juce::String& contentHash,
                                   double targetSampleRate);

    /// Write stem buffers to disk as 32-bit float WAV files.
    /// Also inserts/updates the DB record.
    /// Returns true on success.
    /// Runs on a background thread.
    bool writeStemsToDisk (const juce::String& contentHash,
                           const juce::String& modelVersion,
                           const StemData& stems,
                           double sampleRate);

    /// Insert a "pending" DB record before writing files.
    void insertPendingRecord (const juce::String& contentHash,
                               const juce::String& modelVersion);

    /// Mark a DB record as "complete" after all files are written.
    void markRecordComplete (const juce::String& contentHash,
                              int64_t fileSizeBytes);

    /// Delete cache entry (DB record + disk files) for a given content hash.
    void deleteCacheEntry (const juce::String& contentHash);

    /// Delete any partially written files for a content hash.
    void deletePartialFiles (const juce::String& contentHash);

    /// Run cache eviction if total size exceeds the limit.
    /// @param activeHashes  Content hashes of currently-loaded tracks (skip these).
    void evictIfNeeded (const std::set<juce::String>& activeHashes);

    /// Startup cleanup: remove orphan directories and incomplete ("pending") entries.
    void cleanupOnStartup();

private:
    juce::File getStemDirectory (const juce::String& contentHash) const;

    /// One-time move of any stems still in the legacy Caches location into the
    /// persistent cache. Same-volume rename; skips entries already migrated.
    void migrateLegacyStems();

    /// True when `dir` holds all 4 non-empty stem WAVs (a complete set on disk).
    static bool dirHasCompleteStems (const juce::File& dir);

    /// Re-index an on-disk complete stem set whose DB record was lost (e.g. the
    /// database was reset): inserts a fresh "complete" record instead of deleting
    /// the files, so persistent stems survive a database loss.
    void reregisterStems (const juce::String& contentHash, const juce::File& dir);

    bool writeWavFile (const juce::File& file,
                        const juce::AudioBuffer<float>& buffer,
                        double sampleRate);

    AudioBufferHolder::Ptr readWavFile (const juce::File& file, double targetSampleRate);

    TrackDatabase& db;
    int64_t        maxCacheSize;
};
