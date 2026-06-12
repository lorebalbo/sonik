#include "AudioFileLoader.h"
#include "AudioEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

// ============================================================================
// FLAC binary parsing helpers (file-local)
// JUCE's FlacAudioFormat reader populates only STREAMINFO (sample rate, channels,
// bits), not Vorbis Comment or PICTURE metadata blocks.  We parse them here.
// ============================================================================

namespace
{

inline uint32_t readLE32 (const uint8_t* p) noexcept
{
    return  static_cast<uint32_t> (p[0])
         | (static_cast<uint32_t> (p[1]) <<  8)
         | (static_cast<uint32_t> (p[2]) << 16)
         | (static_cast<uint32_t> (p[3]) << 24);
}

inline uint32_t readBE32 (const uint8_t* p) noexcept
{
    return (static_cast<uint32_t> (p[0]) << 24)
         | (static_cast<uint32_t> (p[1]) << 16)
         | (static_cast<uint32_t> (p[2]) <<  8)
         |  static_cast<uint32_t> (p[3]);
}

/// Iterates FLAC metadata blocks up to and including the first VORBIS_COMMENT
/// block (type 4) and returns its key=value pairs as a case-insensitive
/// StringPairArray with lowercase keys.
juce::StringPairArray extractFlacVorbisComments (const juce::File& file)
{
    juce::StringPairArray result (false); // case-insensitive

    juce::FileInputStream stream (file);
    if (stream.failedToOpen())
        return result;

    // FLAC magic: "fLaC"
    char magic[4];
    if (stream.read (magic, 4) != 4
        || magic[0] != 'f' || magic[1] != 'L'
        || magic[2] != 'a' || magic[3] != 'C')
        return result;

    bool lastBlock = false;
    while (! lastBlock && ! stream.isExhausted())
    {
        char hdr[4];
        if (stream.read (hdr, 4) != 4)
            break;

        const auto* h  = reinterpret_cast<const uint8_t*> (hdr);
        lastBlock      = (h[0] & 0x80) != 0;
        int blockType  =  h[0] & 0x7F;
        uint32_t blockLen = (static_cast<uint32_t> (h[1]) << 16)
                          | (static_cast<uint32_t> (h[2]) <<  8)
                          |  static_cast<uint32_t> (h[3]);

        if (blockType == 4) // VORBIS_COMMENT
        {
            if (blockLen > 4u * 1024u * 1024u)
                break; // sanity: skip impossibly large blocks

            juce::MemoryBlock body;
            body.setSize (blockLen);
            if (static_cast<uint32_t> (stream.read (body.getData(),
                                                     static_cast<int> (blockLen))) != blockLen)
                break;

            const auto* d = static_cast<const uint8_t*> (body.getData());
            uint32_t pos  = 0;

            // Skip vendor string
            if (pos + 4 > blockLen) break;
            uint32_t vendorLen = readLE32 (d + pos);
            pos += 4;
            if (pos + vendorLen > blockLen) break;
            pos += vendorLen;

            // Comment list
            if (pos + 4 > blockLen) break;
            uint32_t count = readLE32 (d + pos);
            pos += 4;

            for (uint32_t i = 0; i < count && pos + 4 <= blockLen; ++i)
            {
                uint32_t cLen = readLE32 (d + pos);
                pos += 4;
                if (pos + cLen > blockLen) break;

                juce::String comment (reinterpret_cast<const char*> (d + pos),
                                      static_cast<size_t> (cLen));
                pos += cLen;

                int eq = comment.indexOfChar ('=');
                if (eq > 0)
                    result.set (comment.substring (0, eq).toLowerCase(),
                                comment.substring (eq + 1));
            }

            break; // found what we need — stop iterating blocks
        }
        else
        {
            stream.skipNextBytes (static_cast<int64_t> (blockLen));
        }
    }

    return result;
}

/// Iterates FLAC metadata blocks and returns the first valid image found in a
/// PICTURE block (type 6).
juce::Image extractFlacPicture (const juce::File& file)
{
    juce::FileInputStream stream (file);
    if (stream.failedToOpen())
        return {};

    char magic[4];
    if (stream.read (magic, 4) != 4
        || magic[0] != 'f' || magic[1] != 'L'
        || magic[2] != 'a' || magic[3] != 'C')
        return {};

    bool lastBlock = false;
    while (! lastBlock && ! stream.isExhausted())
    {
        char hdr[4];
        if (stream.read (hdr, 4) != 4)
            break;

        const auto* h  = reinterpret_cast<const uint8_t*> (hdr);
        lastBlock      = (h[0] & 0x80) != 0;
        int blockType  =  h[0] & 0x7F;
        uint32_t blockLen = (static_cast<uint32_t> (h[1]) << 16)
                          | (static_cast<uint32_t> (h[2]) <<  8)
                          |  static_cast<uint32_t> (h[3]);

        if (blockType == 6 && blockLen >= 32) // PICTURE
        {
            if (blockLen > 30u * 1024u * 1024u)
            {
                stream.skipNextBytes (static_cast<int64_t> (blockLen));
                continue;
            }

            juce::MemoryBlock body;
            body.setSize (blockLen);
            if (static_cast<uint32_t> (stream.read (body.getData(),
                                                     static_cast<int> (blockLen))) != blockLen)
                break;

            const auto* d = static_cast<const uint8_t*> (body.getData());
            uint32_t pos  = 0;

            // Picture type (4 bytes, BE)
            if (pos + 4 > blockLen) continue;
            pos += 4;

            // MIME type (4-byte BE length + string)
            if (pos + 4 > blockLen) continue;
            uint32_t mimeLen = readBE32 (d + pos); pos += 4;
            if (pos + mimeLen > blockLen) continue;
            pos += mimeLen;

            // Description (4-byte BE length + string)
            if (pos + 4 > blockLen) continue;
            uint32_t descLen = readBE32 (d + pos); pos += 4;
            if (pos + descLen > blockLen) continue;
            pos += descLen;

            // Width, height, color depth, color count (4 × 4 bytes)
            if (pos + 16 > blockLen) continue;
            pos += 16;

            // Image data (4-byte BE length + raw bytes)
            if (pos + 4 > blockLen) continue;
            uint32_t imageLen = readBE32 (d + pos); pos += 4;
            if (imageLen == 0 || pos + imageLen > blockLen) continue;

            auto img = juce::ImageFileFormat::loadFrom (d + pos,
                                                        static_cast<size_t> (imageLen));
            if (img.isValid())
                return img;
        }
        else
        {
            stream.skipNextBytes (static_cast<int64_t> (blockLen));
        }
    }

    return {};
}

} // anonymous namespace

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

        // JUCE's FlacAudioFormat reader only populates STREAMINFO fields
        // (sampleRate, numChannels, bitsPerSample) — it does NOT read Vorbis
        // Comment tags into metadataValues.  Parse the binary blocks directly.
        auto fileExt = sourceFile.getFileExtension().toLowerCase();
        if (fileExt == ".flac")
        {
            auto vc       = extractFlacVorbisComments (sourceFile);
            meta.title    = vc.getValue ("title",  sourceFile.getFileNameWithoutExtension());
            meta.artist   = vc.getValue ("artist", "");
            meta.album    = vc.getValue ("album",  "");

            auto keyTag   = vc.getValue ("initialkey", "");
            if (keyTag.isEmpty()) keyTag = vc.getValue ("key", "");
            meta.initialKeyString = keyTag;
        }
        else
        {
            meta.title  = reader->metadataValues.getValue ("title",  sourceFile.getFileNameWithoutExtension());
            meta.artist = reader->metadataValues.getValue ("artist", "");
            meta.album  = reader->metadataValues.getValue ("album",  "");

            // Extract embedded key tag (ID3 TKEY, Vorbis INITIALKEY/KEY)
            auto keyTag = reader->metadataValues.getValue ("TKEY",       "");
            if (keyTag.isEmpty()) keyTag = reader->metadataValues.getValue ("INITIALKEY", "");
            if (keyTag.isEmpty()) keyTag = reader->metadataValues.getValue ("KEY",        "");
            if (keyTag.isEmpty()) keyTag = reader->metadataValues.getValue ("initialkey", "");
            if (keyTag.isEmpty()) keyTag = reader->metadataValues.getValue ("key",        "");
            meta.initialKeyString = keyTag;
        }
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
        auto* ldr = &loader;
        juce::MessageManager::callAsync ([ldr, metaCopy, deckId]()
        {
            ldr->deckStateManager.loadTrack (deckId, metaCopy);
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
            auto* progressLdr = &loader;
            juce::MessageManager::callAsync ([progressLdr, deckIdCopy, prog]()
            {
                auto deckTree = progressLdr->deckStateManager.getDeckState (deckIdCopy);
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
        auto* ldr2 = &loader;
        juce::MessageManager::callAsync ([ldr2, holderPtr, metaFinal, deckIdFinal]()
        {
            ldr2->deliverBuffer (deckIdFinal, holderPtr, metaFinal);
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
        auto* ldr = &loader;
        juce::MessageManager::callAsync ([ldr, deckIdCopy, message]()
        {
            auto deckTree = ldr->deckStateManager.getDeckState (deckIdCopy);
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
        // setDeckBuffer resets atomic playbackStatus to stopped; mirror that in the ValueTree
        deckTree.setProperty (IDs::playbackStatus,  "stopped", nullptr);

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
    auto ext = file.getFileExtension().toLowerCase();

    if (ext == ".flac")
        return extractFlacPicture (file);

    if (ext != ".mp3")
        return {};

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
