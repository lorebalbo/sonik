#include "AudioFileLoader.h"
#include "AudioEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

// ============================================================================
// LoadJob — ThreadPoolJob that decodes a single audio file
// ============================================================================

class AudioFileLoader::LoadJob : public juce::ThreadPoolJob
{
public:
    LoadJob (AudioFileLoader& owner,
             const juce::String& deckId,
             const juce::File& file)
        : juce::ThreadPoolJob ("LoadJob_" + deckId),
          loader (owner),
          targetDeckId (deckId),
          sourceFile (file)
    {}

    JobStatus runJob() override
    {
        // 1. Validate
        if (! validate())
            return jobHasFinished;

        // 2. Compute content hash
        auto contentHash = AudioFileLoader::computeContentHash (sourceFile);

        // 3. Open reader
        auto reader = std::unique_ptr<juce::AudioFormatReader> (
            loader.formatManager.createReaderFor (sourceFile));

        if (reader == nullptr)
        {
            reportError ("Unable to open audio file: unsupported format");
            return jobHasFinished;
        }

        // 4. Extract metadata
        TrackMetadata meta;
        meta.filePath     = sourceFile.getFullPathName();
        meta.contentHash  = contentHash;
        meta.title        = reader->metadataValues.getValue ("title",  sourceFile.getFileNameWithoutExtension());
        meta.artist       = reader->metadataValues.getValue ("artist", "");
        meta.album        = reader->metadataValues.getValue ("album",  "");
        meta.sampleRate   = reader->sampleRate;
        meta.bitDepth     = static_cast<int> (reader->bitsPerSample);
        meta.totalSamples = reader->lengthInSamples;
        meta.channelCount = static_cast<int> (reader->numChannels);
        meta.duration     = (reader->sampleRate > 0.0)
                                ? static_cast<double> (reader->lengthInSamples) / reader->sampleRate
                                : 0.0;

        int srcChannels = static_cast<int> (reader->numChannels);

        // 5. Extract album art (best-effort)
        auto albumArt = AudioFileLoader::extractAlbumArt (sourceFile);
        meta.hasAlbumArt = albumArt.isValid();

        if (meta.hasAlbumArt)
            loader.storeAlbumArt (contentHash, std::move (albumArt));

        // 6. Publish metadata on message thread
        auto metaCopy = meta;
        auto deckId = targetDeckId;
        juce::MessageManager::callAsync ([this, metaCopy, deckId]()
        {
            loader.deckStateManager.loadTrack (deckId, metaCopy);
        });

        if (shouldExit())
            return jobHasFinished;

        // 7. Decode in chunks
        const int64_t totalFrames = reader->lengthInSamples;
        const int     readChannels = juce::jmax (srcChannels, 2); // at least stereo
        constexpr int chunkSize = 65536;

        juce::AudioBuffer<float> decoded (readChannels, static_cast<int> (totalFrames));
        decoded.clear();

        int64_t framesRead = 0;
        while (framesRead < totalFrames)
        {
            if (shouldExit())
                return jobHasFinished;

            int64_t remaining = totalFrames - framesRead;
            int thisChunk = static_cast<int> (juce::jmin (static_cast<int64_t> (chunkSize), remaining));

            reader->read (&decoded, static_cast<int> (framesRead), thisChunk, framesRead, true, true);
            framesRead += thisChunk;

            // Report progress
            float prog = static_cast<float> (framesRead) / static_cast<float> (totalFrames);
            auto deckIdCopy = targetDeckId;
            juce::MessageManager::callAsync ([this, deckIdCopy, prog]()
            {
                auto deckTree = loader.deckStateManager.getDeckState (deckIdCopy);
                if (deckTree.isValid())
                    deckTree.setProperty (IDs::loadingProgress, prog, nullptr);
            });
        }

        if (shouldExit())
            return jobHasFinished;

        // 8. Channel conversion
        if (srcChannels == 1)
        {
            // Mono → duplicate to stereo
            if (decoded.getNumChannels() < 2)
            {
                juce::AudioBuffer<float> stereo (2, static_cast<int> (totalFrames));
                stereo.copyFrom (0, 0, decoded, 0, 0, static_cast<int> (totalFrames));
                stereo.copyFrom (1, 0, decoded, 0, 0, static_cast<int> (totalFrames));
                decoded = std::move (stereo);
            }
            else
            {
                // readChannels was >= 2, so channel 1 exists; copy channel 0 into it
                decoded.copyFrom (1, 0, decoded, 0, 0, static_cast<int> (totalFrames));
            }
        }
        else if (srcChannels > 2)
        {
            // Multi-channel → take L=ch0, R=ch1
            juce::AudioBuffer<float> stereo (2, static_cast<int> (totalFrames));
            stereo.copyFrom (0, 0, decoded, 0, 0, static_cast<int> (totalFrames));
            stereo.copyFrom (1, 0, decoded, 1, 0, static_cast<int> (totalFrames));
            decoded = std::move (stereo);
        }
        // srcChannels == 2: keep as-is

        if (shouldExit())
            return jobHasFinished;

        // 9. Resample if needed
        double deviceRate = loader.targetSampleRate.load (std::memory_order_relaxed);
        int64_t outputFrames = totalFrames;

        if (deviceRate > 0.0 && std::abs (reader->sampleRate - deviceRate) > 0.01)
        {
            double ratio = reader->sampleRate / deviceRate;
            outputFrames = static_cast<int64_t> (std::ceil (static_cast<double> (totalFrames) / ratio));

            juce::AudioBuffer<float> resampled (2, static_cast<int> (outputFrames));
            resampled.clear();

            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator interpolator;
                int used = interpolator.process (ratio,
                                                 decoded.getReadPointer (ch),
                                                 resampled.getWritePointer (ch),
                                                 static_cast<int> (outputFrames));
                juce::ignoreUnused (used);
            }

            decoded = std::move (resampled);
            meta.sampleRate = deviceRate;
        }

        if (shouldExit())
            return jobHasFinished;

        // 10. Deliver to audio engine
        auto holder = new AudioBufferHolder (std::move (decoded), meta.sampleRate, outputFrames);
        AudioBufferHolder::Ptr holderPtr (holder);

        auto metaFinal = meta;
        auto deckIdFinal = targetDeckId;
        juce::MessageManager::callAsync ([this, holderPtr, metaFinal, deckIdFinal]()
        {
            loader.deliverBuffer (deckIdFinal, holderPtr, metaFinal);
        });

        return jobHasFinished;
    }

private:
    bool validate()
    {
        if (! sourceFile.existsAsFile())
        {
            reportError ("File does not exist: " + sourceFile.getFullPathName());
            return false;
        }

        if (sourceFile.getSize() == 0)
        {
            reportError ("File is empty: " + sourceFile.getFullPathName());
            return false;
        }

        if (! AudioFileLoader::isSupportedExtension (sourceFile.getFileExtension()))
        {
            reportError ("Unsupported file type: " + sourceFile.getFileExtension());
            return false;
        }

        // Warn (log) for very large files
        if (sourceFile.getSize() > 2LL * 1024 * 1024 * 1024)
            DBG ("Warning: file exceeds 2 GB — " + sourceFile.getFullPathName());

        return true;
    }

    void reportError (const juce::String& message)
    {
        auto deckIdCopy = targetDeckId;
        juce::MessageManager::callAsync ([this, deckIdCopy, message]()
        {
            auto deckTree = loader.deckStateManager.getDeckState (deckIdCopy);
            if (deckTree.isValid())
            {
                deckTree.setProperty (IDs::loadingStatus,  "error",  nullptr);
                deckTree.setProperty (IDs::loadingError,   message,  nullptr);
                deckTree.setProperty (IDs::loadingProgress, 0.0f,    nullptr);
            }
        });
    }

    AudioFileLoader& loader;
    juce::String     targetDeckId;
    juce::File       sourceFile;
};

// ============================================================================
// AudioFileLoader implementation
// ============================================================================

AudioFileLoader::AudioFileLoader (DeckStateManager& deckState,
                                  AudioEngine& engine,
                                  double deviceSampleRate)
    : deckStateManager (deckState),
      audioEngine (engine),
      targetSampleRate (deviceSampleRate)
{
    formatManager.registerBasicFormats();
}

AudioFileLoader::~AudioFileLoader()
{
    threadPool.removeAllJobs (true, 5000);
}

void AudioFileLoader::loadFile (const juce::String& deckId, const juce::File& file)
{
    juce::MessageManager::getInstance()->isThisTheMessageThread();

    // Cancel any existing load for this deck
    cancelLoad (deckId);

    // Set loading status on the deck
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    deckTree.setProperty (IDs::loadingStatus,   "loading", nullptr);
    deckTree.setProperty (IDs::loadingProgress,  0.0f,     nullptr);
    deckTree.setProperty (IDs::loadingError,     "",        nullptr);

    // Create and enqueue the job
    auto* job = new LoadJob (*this, deckId, file);
    activeJobs[deckId] = job;
    threadPool.addJob (job, true); // pool takes ownership
}

void AudioFileLoader::cancelLoad (const juce::String& deckId)
{
    auto it = activeJobs.find (deckId);
    if (it != activeJobs.end())
    {
        auto* job = it->second;
        threadPool.removeJob (job, true, 5000);
        activeJobs.erase (it);
    }

    // Reset loading status
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (deckTree.isValid())
    {
        deckTree.setProperty (IDs::loadingStatus,   "idle", nullptr);
        deckTree.setProperty (IDs::loadingProgress,  0.0f,  nullptr);
        deckTree.setProperty (IDs::loadingError,     "",     nullptr);
    }
}

juce::Image AudioFileLoader::getAlbumArt (const juce::String& contentHash) const
{
    std::lock_guard<std::mutex> lock (artCacheMutex);
    auto it = artCache.find (contentHash);
    if (it != artCache.end())
        return it->second;
    return {};
}

void AudioFileLoader::setDeviceSampleRate (double newRate)
{
    targetSampleRate.store (newRate, std::memory_order_relaxed);
}

bool AudioFileLoader::isSupportedExtension (const juce::String& ext)
{
    auto lower = ext.toLowerCase();
    return lower == ".mp3"  || lower == ".flac"
        || lower == ".wav"  || lower == ".aiff"
        || lower == ".aif";
}

void AudioFileLoader::deliverBuffer (const juce::String& deckId,
                                     AudioBufferHolder::Ptr holder,
                                     const TrackMetadata& metadata)
{
    // This runs on the message thread.
    audioEngine.setDeckBuffer (deckId, std::move (holder));

    // Update deck loading state
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (deckTree.isValid())
    {
        deckTree.setProperty (IDs::loadingStatus,   "idle", nullptr);
        deckTree.setProperty (IDs::loadingProgress,  1.0f,  nullptr);
        deckTree.setProperty (IDs::loadingError,     "",     nullptr);

        auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
        trackMeta.setProperty (IDs::channelCount, metadata.bitDepth > 0 ? 2 : 0, nullptr);
    }

    // Remove from active jobs
    activeJobs.erase (deckId);
}

void AudioFileLoader::storeAlbumArt (const juce::String& contentHash, juce::Image image)
{
    std::lock_guard<std::mutex> lock (artCacheMutex);

    // If already cached, move to front of LRU
    auto it = artCache.find (contentHash);
    if (it != artCache.end())
    {
        artCacheLRU.erase (std::remove (artCacheLRU.begin(), artCacheLRU.end(), contentHash),
                           artCacheLRU.end());
        artCacheLRU.push_front (contentHash);
        return;
    }

    // Evict oldest if at capacity
    while (static_cast<int> (artCache.size()) >= maxArtCacheSize && ! artCacheLRU.empty())
    {
        artCache.erase (artCacheLRU.back());
        artCacheLRU.pop_back();
    }

    artCache[contentHash] = std::move (image);
    artCacheLRU.push_front (contentHash);
}

juce::String AudioFileLoader::computeContentHash (const juce::File& file)
{
    // Fast heuristic: MD5 of first 64 KB + file size
    juce::FileInputStream stream (file);
    if (stream.failedToOpen())
        return {};

    constexpr int hashBytes = 65536;
    juce::MemoryBlock block;
    auto bytesToRead = juce::jmin (stream.getTotalLength(), static_cast<int64_t> (hashBytes));
    block.setSize (static_cast<size_t> (bytesToRead));
    stream.read (block.getData(), static_cast<int> (bytesToRead));

    // Append file size as 8 bytes
    int64_t fileSize = file.getSize();
    block.append (&fileSize, sizeof (fileSize));

    return juce::MD5 (block.getData(), block.getSize()).toHexString();
}

juce::Image AudioFileLoader::extractAlbumArt (const juce::File& file)
{
    // Try to parse ID3v2 APIC frame for MP3 files
    auto ext = file.getFileExtension().toLowerCase();
    if (ext != ".mp3")
        return {}; // Only MP3 album art extraction is supported for now

    juce::FileInputStream stream (file);
    if (stream.failedToOpen())
        return {};

    // Check ID3v2 header: "ID3"
    char header[10];
    if (stream.read (header, 10) != 10)
        return {};

    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3')
        return {};

    // ID3v2 tag size (syncsafe integer, bytes 6-9)
    uint32_t tagSize = (static_cast<uint32_t> (header[6] & 0x7F) << 21)
                     | (static_cast<uint32_t> (header[7] & 0x7F) << 14)
                     | (static_cast<uint32_t> (header[8] & 0x7F) << 7)
                     | (static_cast<uint32_t> (header[9] & 0x7F));

    if (tagSize == 0 || tagSize > 50 * 1024 * 1024) // sanity: max 50 MB
        return {};

    // Read entire ID3v2 tag body
    juce::MemoryBlock tagData;
    tagData.setSize (tagSize);
    if (static_cast<uint32_t> (stream.read (tagData.getData(), static_cast<int> (tagSize))) != tagSize)
        return {};

    auto* data = static_cast<const uint8_t*> (tagData.getData());
    uint32_t pos = 0;

    // ID3v2.3/2.4 frame parsing
    while (pos + 10 <= tagSize)
    {
        // Frame ID (4 bytes), Size (4 bytes), Flags (2 bytes)
        char frameId[5] = {};
        std::memcpy (frameId, data + pos, 4);

        if (frameId[0] == '\0')
            break; // padding reached

        uint32_t frameSize;
        bool isV24 = (header[3] == 4);
        if (isV24)
        {
            // v2.4: syncsafe integer
            frameSize = (static_cast<uint32_t> (data[pos + 4] & 0x7F) << 21)
                      | (static_cast<uint32_t> (data[pos + 5] & 0x7F) << 14)
                      | (static_cast<uint32_t> (data[pos + 6] & 0x7F) << 7)
                      | (static_cast<uint32_t> (data[pos + 7] & 0x7F));
        }
        else
        {
            // v2.3: regular big-endian integer
            frameSize = (static_cast<uint32_t> (data[pos + 4]) << 24)
                      | (static_cast<uint32_t> (data[pos + 5]) << 16)
                      | (static_cast<uint32_t> (data[pos + 6]) << 8)
                      | (static_cast<uint32_t> (data[pos + 7]));
        }

        pos += 10; // skip frame header

        if (frameSize == 0 || pos + frameSize > tagSize)
            break;

        if (std::strcmp (frameId, "APIC") == 0 && frameSize > 4)
        {
            // APIC frame: encoding(1) + mime(null-term) + pictureType(1) + description(null-term) + data
            auto* apicData = data + pos;
            uint32_t apicPos = 1; // skip text encoding byte

            // Skip MIME type string
            while (apicPos < frameSize && apicData[apicPos] != '\0')
                ++apicPos;
            if (apicPos >= frameSize)
                break;
            ++apicPos; // skip null terminator

            // Skip picture type byte
            if (apicPos >= frameSize)
                break;
            ++apicPos;

            // Skip description string
            while (apicPos < frameSize && apicData[apicPos] != '\0')
                ++apicPos;
            if (apicPos >= frameSize)
                break;
            ++apicPos; // skip null terminator

            // Remaining bytes are image data
            uint32_t imageSize = frameSize - apicPos;
            if (imageSize > 0)
            {
                auto img = juce::ImageFileFormat::loadFrom (apicData + apicPos,
                                                            static_cast<size_t> (imageSize));
                if (img.isValid())
                    return img;
            }
        }

        pos += frameSize;
    }

    return {};
}
