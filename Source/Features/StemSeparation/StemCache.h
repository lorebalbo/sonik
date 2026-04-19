#pragma once

#include "StemData.h"
#include "../Deck/Database/TrackDatabase.h"
#include "../AudioEngine/AudioBufferHolder.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <optional>
#include <set>

/// Manages the on-disk stem cache and stems_data table in SQLite.
/// Cache location: ~/Library/Caches/Sonik/Stems/<content_hash>/
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

    /// Returns the stem cache root directory.
    static juce::File getCacheDirectory();

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

    bool writeWavFile (const juce::File& file,
                        const juce::AudioBuffer<float>& buffer,
                        double sampleRate);

    AudioBufferHolder::Ptr readWavFile (const juce::File& file, double targetSampleRate);

    TrackDatabase& db;
    int64_t        maxCacheSize;
};
