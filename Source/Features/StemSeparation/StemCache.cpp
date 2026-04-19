#include "StemCache.h"
#include <sqlite3.h>

// ============================================================================
// Static helpers
// ============================================================================

juce::File StemCache::getCacheDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Library")
               .getChildFile ("Caches")
               .getChildFile ("Sonik")
               .getChildFile ("Stems");
}

// ============================================================================
// Construction
// ============================================================================

StemCache::StemCache (TrackDatabase& database, int64_t maxCacheSizeBytes)
    : db (database),
      maxCacheSize (maxCacheSizeBytes)
{
    getCacheDirectory().createDirectory();
}

// ============================================================================
// Cache directory per hash
// ============================================================================

juce::File StemCache::getStemDirectory (const juce::String& contentHash) const
{
    return getCacheDirectory().getChildFile (contentHash);
}

// ============================================================================
// Cache check (message thread — fast)
// ============================================================================

bool StemCache::hasCachedStems (const juce::String& contentHash) const
{
    // 1. Query DB for a "complete" record
    if (! db.hasStemRecord (contentHash))
        return false;

    // 2. Verify all 4 WAV files exist on disk
    auto dir = getCacheDirectory().getChildFile (contentHash);
    for (int i = 0; i < StemData::NumStems; ++i)
    {
        if (! dir.getChildFile (StemData::stemFilename (i)).existsAsFile())
            return false;
    }

    return true;
}

// ============================================================================
// Load cached stems from disk (background thread)
// ============================================================================

StemData::Ptr StemCache::loadCachedStems (const juce::String& contentHash,
                                           double targetSampleRate)
{
    auto dir = getStemDirectory (contentHash);
    auto result = new StemData();

    for (int i = 0; i < StemData::NumStems; ++i)
    {
        auto file = dir.getChildFile (StemData::stemFilename (i));
        auto holder = readWavFile (file, targetSampleRate);

        if (holder == nullptr)
            return nullptr; // Failed to load — treat as cache miss

        result->stems[static_cast<size_t> (i)] = holder;
    }

    return StemData::Ptr (result);
}

// ============================================================================
// Write stems to disk (background thread)
// ============================================================================

bool StemCache::writeStemsToDisk (const juce::String& contentHash,
                                   const juce::String& modelVersion,
                                   const StemData& stems,
                                   double sampleRate)
{
    auto dir = getStemDirectory (contentHash);
    dir.createDirectory();

    int64_t totalBytes = 0;

    for (int i = 0; i < StemData::NumStems; ++i)
    {
        auto holder = stems.stems[static_cast<size_t> (i)];
        if (holder == nullptr)
        {
            deletePartialFiles (contentHash);
            return false;
        }

        auto file = dir.getChildFile (StemData::stemFilename (i));
        if (! writeWavFile (file, holder->getBuffer(), sampleRate))
        {
            deletePartialFiles (contentHash);
            return false;
        }

        totalBytes += file.getSize();
    }

    markRecordComplete (contentHash, totalBytes);
    return true;
}

// ============================================================================
// DB record lifecycle
// ============================================================================

void StemCache::insertPendingRecord (const juce::String& contentHash,
                                      const juce::String& modelVersion)
{
    db.insertStemRecord (contentHash, modelVersion, "pending");
}

void StemCache::markRecordComplete (const juce::String& contentHash,
                                     int64_t fileSizeBytes)
{
    auto dir = getStemDirectory (contentHash);
    db.updateStemRecord (contentHash, "complete", fileSizeBytes,
                          dir.getChildFile (StemData::stemFilename (StemData::Vocals)).getFullPathName(),
                          dir.getChildFile (StemData::stemFilename (StemData::Drums)).getFullPathName(),
                          dir.getChildFile (StemData::stemFilename (StemData::Bass)).getFullPathName(),
                          dir.getChildFile (StemData::stemFilename (StemData::Other)).getFullPathName());
}

void StemCache::deleteCacheEntry (const juce::String& contentHash)
{
    db.deleteStemRecord (contentHash);
    auto dir = getStemDirectory (contentHash);
    if (dir.isDirectory())
        dir.deleteRecursively();
}

void StemCache::deletePartialFiles (const juce::String& contentHash)
{
    auto dir = getStemDirectory (contentHash);
    if (dir.isDirectory())
        dir.deleteRecursively();
    db.deleteStemRecord (contentHash);
}

// ============================================================================
// Cache eviction
// ============================================================================

void StemCache::evictIfNeeded (const std::set<juce::String>& activeHashes)
{
    // Get all records ordered by created_at ascending (oldest first)
    auto records = db.getAllStemRecords();

    int64_t totalSize = 0;
    for (const auto& r : records)
        totalSize += r.fileSizeBytes;

    if (totalSize <= maxCacheSize)
        return;

    // Evict oldest entries until under limit, skipping active hashes
    for (const auto& r : records)
    {
        if (totalSize <= maxCacheSize)
            break;

        if (activeHashes.count (r.contentHash) > 0)
            continue; // Skip actively loaded tracks

        totalSize -= r.fileSizeBytes;
        deleteCacheEntry (r.contentHash);
    }
}

// ============================================================================
// Startup cleanup
// ============================================================================

void StemCache::cleanupOnStartup()
{
    // 1. Delete "pending" (incomplete) entries
    auto pendingHashes = db.getPendingStemHashes();
    for (const auto& hash : pendingHashes)
        deleteCacheEntry (hash);

    // 2. Scan disk for orphan directories (no DB record)
    auto cacheDir = getCacheDirectory();
    if (! cacheDir.isDirectory())
        return;

    auto children = cacheDir.findChildFiles (juce::File::findDirectories, false);
    for (const auto& child : children)
    {
        auto hash = child.getFileName();
        if (! db.hasStemRecord (hash))
            child.deleteRecursively();
    }
}

// ============================================================================
// WAV file I/O
// ============================================================================

bool StemCache::writeWavFile (const juce::File& file,
                                const juce::AudioBuffer<float>& buffer,
                                double sampleRate)
{
    juce::WavAudioFormat wavFormat;
    auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());

    if (stream == nullptr)
        return false;

    auto writer = std::unique_ptr<juce::AudioFormatWriter> (
        wavFormat.createWriterFor (stream.get(),
                                   sampleRate,
                                   static_cast<unsigned int> (buffer.getNumChannels()),
                                   32,      // 32-bit float
                                   {},      // metadata
                                   0));     // quality

    if (writer == nullptr)
        return false;

    stream.release(); // writer takes ownership of the stream

    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

AudioBufferHolder::Ptr StemCache::readWavFile (const juce::File& file, double targetSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        formatManager.createReaderFor (file));

    if (reader == nullptr)
        return nullptr;

    const auto numChannels = static_cast<int> (reader->numChannels);
    const auto numSamples  = static_cast<int> (reader->lengthInSamples);

    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    reader->read (&buffer, 0, numSamples, 0, true, numChannels > 1);

    // Resample if needed
    const double fileSR = reader->sampleRate;
    int64_t outputFrames = numSamples;

    if (targetSampleRate > 0.0 && std::abs (fileSR - targetSampleRate) > 0.01)
    {
        double ratio = fileSR / targetSampleRate;
        outputFrames = static_cast<int64_t> (std::ceil (static_cast<double> (numSamples) / ratio));

        juce::AudioBuffer<float> resampled (numChannels, static_cast<int> (outputFrames));
        resampled.clear();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (ratio,
                                  buffer.getReadPointer (ch),
                                  resampled.getWritePointer (ch),
                                  static_cast<int> (outputFrames));
        }

        return new AudioBufferHolder (std::move (resampled), targetSampleRate, outputFrames);
    }

    return new AudioBufferHolder (std::move (buffer), fileSR, outputFrames);
}
