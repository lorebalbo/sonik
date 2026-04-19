#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "Features/StemSeparation/SpectralProcessor.h"
#include "Features/StemSeparation/StemData.h"
#include "Features/StemSeparation/StemCache.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioBufferHolder.h"

// ============================================================================
// SpectralProcessor Tests
// ============================================================================
class SpectralProcessorTests : public juce::UnitTest
{
public:
    SpectralProcessorTests() : juce::UnitTest ("SpectralProcessor", "Sonik") {}

    void runTest() override
    {
        testNumBins();
        testNumFramesKnownLengths();
        testNumFramesEdgeCases();
        testForwardOutputSize();
        testForwardZeroInput();
        testRoundtripSineWave();
        testRoundtripDCSigal();
    }

private:
    void testNumBins()
    {
        beginTest ("SpectralProcessor - numBins returns FFT/2 + 1 = 2049");
        expectEquals (SpectralProcessor::numBins(), 2049);
    }

    void testNumFramesKnownLengths()
    {
        beginTest ("SpectralProcessor - numFrames returns correct count for known signal lengths");

        // For signalLength == kWindowSize (4096), should be exactly 1 frame
        expectEquals (SpectralProcessor::numFrames (4096), 1);

        // For signalLength == kWindowSize + kHopSize (4096+1024=5120), should be 2 frames
        expectEquals (SpectralProcessor::numFrames (5120), 2);

        // General formula: (signalLength - 4096) / 1024 + 1
        // signalLength = 44100 (1 second at 44.1kHz)
        int expected = (44100 - 4096) / 1024 + 1; // = 40004/1024 + 1 = 39 + 1 = 40
        expectEquals (SpectralProcessor::numFrames (44100), expected);
    }

    void testNumFramesEdgeCases()
    {
        beginTest ("SpectralProcessor - numFrames edge cases");
        expectEquals (SpectralProcessor::numFrames (0), 0);
        expectEquals (SpectralProcessor::numFrames (-1), 0);
        // signalLength < kWindowSize still produces 1 frame per the formula
        // (4095 - 4096) / 1024 + 1 = (-1)/1024 + 1 = 0 + 1 = 1 (C++ truncation toward zero)
        expectEquals (SpectralProcessor::numFrames (4095), 1);
    }

    void testForwardOutputSize()
    {
        beginTest ("SpectralProcessor - forward STFT produces correct output size");

        SpectralProcessor proc;
        const int signalLength = 8192;
        std::vector<float> signal (static_cast<size_t> (signalLength), 0.0f);

        // Fill with a sine wave
        for (int i = 0; i < signalLength; ++i)
            signal[static_cast<size_t> (i)] = std::sin (
                2.0f * juce::MathConstants<float>::pi * 440.0f
                * static_cast<float> (i) / 44100.0f);

        std::vector<float> output;
        proc.forward (signal.data(), signalLength, output);

        int nFrames = SpectralProcessor::numFrames (signalLength);
        int nBins   = SpectralProcessor::numBins();
        auto expectedSize = static_cast<size_t> (nFrames * nBins * 2);

        expectEquals (output.size(), expectedSize);
    }

    void testForwardZeroInput()
    {
        beginTest ("SpectralProcessor - STFT of zero input produces zero-magnitude output");

        SpectralProcessor proc;
        const int signalLength = 8192;
        std::vector<float> signal (static_cast<size_t> (signalLength), 0.0f);

        std::vector<float> output;
        proc.forward (signal.data(), signalLength, output);

        // All values should be zero (or very near zero)
        float maxMag = 0.0f;
        for (auto v : output)
            maxMag = std::max (maxMag, std::abs (v));

        expect (maxMag < 1e-10f,
                "Zero input should produce zero-magnitude spectrogram");
    }

    void testRoundtripSineWave()
    {
        beginTest ("SpectralProcessor - forward/inverse roundtrip preserves sine wave");

        SpectralProcessor proc;

        // Use a signal long enough for several frames
        const int signalLength = 16384;
        std::vector<float> signal (static_cast<size_t> (signalLength), 0.0f);

        // Generate a 440 Hz sine wave
        for (int i = 0; i < signalLength; ++i)
            signal[static_cast<size_t> (i)] = std::sin (
                2.0f * juce::MathConstants<float>::pi * 440.0f
                * static_cast<float> (i) / 44100.0f);

        // Forward STFT
        std::vector<float> spectrogram;
        proc.forward (signal.data(), signalLength, spectrogram);

        int nFrames = SpectralProcessor::numFrames (signalLength);
        expect (nFrames > 0, "Should have at least one frame");

        // Inverse STFT
        std::vector<float> reconstructed;
        int reconstructedLength = proc.inverse (spectrogram.data(), nFrames, reconstructed);

        expect (reconstructedLength > 0, "Reconstructed length should be > 0");

        // Compare in the stable region (skip edges where overlap-add is incomplete)
        // The stable region starts after the first full window and ends before the last
        int stableStart = SpectralProcessor::kWindowSize;
        int stableEnd   = juce::jmin (signalLength, reconstructedLength) - SpectralProcessor::kWindowSize;

        if (stableEnd <= stableStart)
        {
            logMessage ("Signal too short for stable region comparison — skipping");
            return;
        }

        float maxError = 0.0f;
        for (int i = stableStart; i < stableEnd; ++i)
        {
            float err = std::abs (signal[static_cast<size_t> (i)]
                                  - reconstructed[static_cast<size_t> (i)]);
            maxError = std::max (maxError, err);
        }

        expect (maxError < 0.01f,
                juce::String ("Roundtrip max error in stable region: ")
                    + juce::String (maxError, 6)
                    + " (should be < 0.01)");
    }

    void testRoundtripDCSigal()
    {
        beginTest ("SpectralProcessor - forward/inverse roundtrip preserves DC signal");

        SpectralProcessor proc;
        const int signalLength = 16384;
        const float dcValue = 0.5f;
        std::vector<float> signal (static_cast<size_t> (signalLength), dcValue);

        std::vector<float> spectrogram;
        proc.forward (signal.data(), signalLength, spectrogram);

        int nFrames = SpectralProcessor::numFrames (signalLength);
        std::vector<float> reconstructed;
        int reconstructedLength = proc.inverse (spectrogram.data(), nFrames, reconstructed);

        int stableStart = SpectralProcessor::kWindowSize;
        int stableEnd   = juce::jmin (signalLength, reconstructedLength) - SpectralProcessor::kWindowSize;

        if (stableEnd <= stableStart)
            return;

        float maxError = 0.0f;
        for (int i = stableStart; i < stableEnd; ++i)
        {
            float err = std::abs (dcValue - reconstructed[static_cast<size_t> (i)]);
            maxError = std::max (maxError, err);
        }

        expect (maxError < 0.01f,
                juce::String ("DC roundtrip max error: ") + juce::String (maxError, 6));
    }
};

static SpectralProcessorTests spectralProcessorTests;

// ============================================================================
// StemData Tests
// ============================================================================
class StemDataTests : public juce::UnitTest
{
public:
    StemDataTests() : juce::UnitTest ("StemData", "Sonik") {}

    void runTest() override
    {
        testStemNames();
        testStemFilenames();
        testDefaultNullptrs();
        testOutOfRangeNames();
    }

private:
    void testStemNames()
    {
        beginTest ("StemData - stemName returns correct names for each StemIndex");
        expectEquals (juce::String (StemData::stemName (StemData::Vocals)), juce::String ("vocals"));
        expectEquals (juce::String (StemData::stemName (StemData::Drums)),  juce::String ("drums"));
        expectEquals (juce::String (StemData::stemName (StemData::Bass)),   juce::String ("bass"));
        expectEquals (juce::String (StemData::stemName (StemData::Other)),  juce::String ("other"));
    }

    void testStemFilenames()
    {
        beginTest ("StemData - stemFilename returns correct filenames with .wav");
        expectEquals (juce::String (StemData::stemFilename (StemData::Vocals)), juce::String ("vocals.wav"));
        expectEquals (juce::String (StemData::stemFilename (StemData::Drums)),  juce::String ("drums.wav"));
        expectEquals (juce::String (StemData::stemFilename (StemData::Bass)),   juce::String ("bass.wav"));
        expectEquals (juce::String (StemData::stemFilename (StemData::Other)),  juce::String ("other.wav"));
    }

    void testDefaultNullptrs()
    {
        beginTest ("StemData - default-constructed has nullptr stems");
        auto data = new StemData();
        StemData::Ptr ptr (data);

        expect (ptr->getVocals() == nullptr, "Vocals should be nullptr");
        expect (ptr->getDrums()  == nullptr, "Drums should be nullptr");
        expect (ptr->getBass()   == nullptr, "Bass should be nullptr");
        expect (ptr->getOther()  == nullptr, "Other should be nullptr");
    }

    void testOutOfRangeNames()
    {
        beginTest ("StemData - out-of-range index returns unknown");
        expectEquals (juce::String (StemData::stemName (-1)),  juce::String ("unknown"));
        expectEquals (juce::String (StemData::stemName (4)),   juce::String ("unknown"));
        expectEquals (juce::String (StemData::stemFilename (-1)), juce::String ("unknown.wav"));
        expectEquals (juce::String (StemData::stemFilename (99)), juce::String ("unknown.wav"));
    }
};

static StemDataTests stemDataTests;

// ============================================================================
// TrackDatabase Stem Record Tests
// ============================================================================
class TrackDatabaseStemTests : public juce::UnitTest
{
public:
    TrackDatabaseStemTests() : juce::UnitTest ("TrackDatabase Stem Records", "Sonik") {}

    void runTest() override
    {
        testHasStemRecordFalseForNonExistent();
        testInsertAndHasStemRecord();
        testDeleteStemRecord();
        testGetPendingStemHashes();
        testGetAllStemRecords();
    }

private:
    struct DbContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;

        DbContext()
        {
            dbFile = juce::File::createTempFile ("sonik_stem_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
        }

        ~DbContext()
        {
            db.reset();
            dbFile.deleteFile();
        }
    };

    void testHasStemRecordFalseForNonExistent()
    {
        beginTest ("TrackDatabase - hasStemRecord returns false for non-existent hash");
        DbContext ctx;
        expect (! ctx.db->hasStemRecord ("nonexistent_hash_12345"),
                "Should return false for a hash that doesn't exist");
    }

    void testInsertAndHasStemRecord()
    {
        beginTest ("TrackDatabase - insertStemRecord + hasStemRecord returns true for complete");
        DbContext ctx;

        const juce::String hash = "test_stem_hash_abc";
        const juce::String model = "htdemucs_v4";

        // Insert as pending first — hasStemRecord checks for 'complete' status
        ctx.db->insertStemRecord (hash, model, "pending");
        expect (! ctx.db->hasStemRecord (hash),
                "Pending record should NOT be found by hasStemRecord");

        // Update to complete
        ctx.db->updateStemRecord (hash, "complete", 1024,
                                  "/path/vocals.wav", "/path/drums.wav",
                                  "/path/bass.wav", "/path/other.wav");
        expect (ctx.db->hasStemRecord (hash),
                "Complete record should be found by hasStemRecord");
    }

    void testDeleteStemRecord()
    {
        beginTest ("TrackDatabase - deleteStemRecord removes the record");
        DbContext ctx;

        const juce::String hash = "delete_test_hash";
        ctx.db->insertStemRecord (hash, "htdemucs_v4", "pending");
        ctx.db->updateStemRecord (hash, "complete", 512,
                                  "/v.wav", "/d.wav", "/b.wav", "/o.wav");
        expect (ctx.db->hasStemRecord (hash), "Should exist after insert + complete");

        ctx.db->deleteStemRecord (hash);
        expect (! ctx.db->hasStemRecord (hash),
                "Should not exist after deletion");
    }

    void testGetPendingStemHashes()
    {
        beginTest ("TrackDatabase - getPendingStemHashes returns pending hashes");
        DbContext ctx;

        ctx.db->insertStemRecord ("pending_1", "htdemucs_v4", "pending");
        ctx.db->insertStemRecord ("pending_2", "htdemucs_v4", "pending");
        ctx.db->insertStemRecord ("complete_1", "htdemucs_v4", "pending");
        ctx.db->updateStemRecord ("complete_1", "complete", 256,
                                  "/v.wav", "/d.wav", "/b.wav", "/o.wav");

        auto pending = ctx.db->getPendingStemHashes();
        expectEquals (pending.size(), 2);
        expect (pending.contains ("pending_1"), "Should contain pending_1");
        expect (pending.contains ("pending_2"), "Should contain pending_2");
        expect (! pending.contains ("complete_1"), "Should NOT contain complete_1");
    }

    void testGetAllStemRecords()
    {
        beginTest ("TrackDatabase - getAllStemRecords returns records ordered by created_at");
        DbContext ctx;

        // Insert multiple records with slight time separation
        ctx.db->insertStemRecord ("rec_a", "htdemucs_v4", "pending");
        ctx.db->updateStemRecord ("rec_a", "complete", 100,
                                  "/v.wav", "/d.wav", "/b.wav", "/o.wav");

        ctx.db->insertStemRecord ("rec_b", "htdemucs_v4", "pending");
        ctx.db->updateStemRecord ("rec_b", "complete", 200,
                                  "/v.wav", "/d.wav", "/b.wav", "/o.wav");

        auto records = ctx.db->getAllStemRecords();
        expectEquals (static_cast<int> (records.size()), 2);

        // They should be ordered by created_at ascending
        expect (records[0].createdAt <= records[1].createdAt,
                "Records should be ordered by created_at ascending");
    }
};

static TrackDatabaseStemTests trackDatabaseStemTests;

// ============================================================================
// StemCache Tests
// ============================================================================
class StemCacheTests : public juce::UnitTest
{
public:
    StemCacheTests() : juce::UnitTest ("StemCache", "Sonik") {}

    void runTest() override
    {
        testGetCacheDirectory();
        testHasCachedStemsReturnsFalse();
        testWriteAndReadStems();
        testCleanupPendingRecords();
    }

private:
    struct CacheTestContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<StemCache> cache;

        CacheTestContext()
        {
            dbFile = juce::File::createTempFile ("sonik_cache_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            cache = std::make_unique<StemCache> (*db);
        }

        ~CacheTestContext()
        {
            cache.reset();
            db.reset();
            dbFile.deleteFile();
        }
    };

    void testGetCacheDirectory()
    {
        beginTest ("StemCache - getCacheDirectory returns valid path containing Sonik/Stems");
        auto dir = StemCache::getCacheDirectory();
        auto path = dir.getFullPathName();
        expect (path.contains ("Sonik"), "Cache dir should contain 'Sonik'");
        expect (path.contains ("Stems"), "Cache dir should contain 'Stems'");
    }

    void testHasCachedStemsReturnsFalse()
    {
        beginTest ("StemCache - hasCachedStems returns false for non-existent hash");
        CacheTestContext ctx;
        expect (! ctx.cache->hasCachedStems ("nonexistent_hash_xyz"),
                "Should return false for a hash with no cached stems");
    }

    void testWriteAndReadStems()
    {
        beginTest ("StemCache - write stems to disk then read back");
        CacheTestContext ctx;

        const juce::String hash = "write_read_test_hash";
        const juce::String model = "htdemucs_v4";
        const double sampleRate = 44100.0;
        const int numSamples = 4410; // 0.1 second

        // Create StemData with simple test buffers
        StemData stems;
        for (int i = 0; i < StemData::NumStems; ++i)
        {
            juce::AudioBuffer<float> audioBuf (2, numSamples);
            // Fill with a unique value per stem for identification
            float value = static_cast<float> (i + 1) * 0.1f;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < numSamples; ++s)
                    audioBuf.setSample (ch, s, value);
            auto holder = new AudioBufferHolder (std::move (audioBuf), sampleRate,
                                                  static_cast<int64_t> (numSamples));
            stems.stems[static_cast<size_t> (i)] = AudioBufferHolder::Ptr (holder);
        }

        // Insert pending record
        ctx.cache->insertPendingRecord (hash, model);

        // Write
        bool writeOk = ctx.cache->writeStemsToDisk (hash, model, stems, sampleRate);
        expect (writeOk, "writeStemsToDisk should succeed");

        // Verify files exist
        expect (ctx.cache->hasCachedStems (hash),
                "hasCachedStems should return true after writing");

        // Read back
        auto loaded = ctx.cache->loadCachedStems (hash, sampleRate);
        expect (loaded != nullptr, "loadCachedStems should return non-null");

        if (loaded != nullptr)
        {
            for (int i = 0; i < StemData::NumStems; ++i)
            {
                auto stem = loaded->stems[static_cast<size_t> (i)];
                expect (stem != nullptr,
                        juce::String ("Stem ") + StemData::stemName (i) + " should be loaded");

                if (stem != nullptr)
                {
                    // Check the buffer has correct size
                    expectEquals (stem->getBuffer().getNumSamples(), numSamples);
                    expectEquals (stem->getBuffer().getNumChannels(), 2);

                    // Check values are approximately correct (WAV I/O may introduce tiny rounding)
                    float expected = static_cast<float> (i + 1) * 0.1f;
                    float actual = stem->getBuffer().getSample (0, numSamples / 2);
                    expect (std::abs (actual - expected) < 0.001f,
                            juce::String ("Stem ") + StemData::stemName (i)
                                + " value mismatch: expected " + juce::String (expected, 4)
                                + " got " + juce::String (actual, 4));
                }
            }
        }

        // Cleanup: delete the cache entry so we don't leave files on disk
        ctx.cache->deleteCacheEntry (hash);
    }

    void testCleanupPendingRecords()
    {
        beginTest ("StemCache - cleanupOnStartup removes pending records");
        CacheTestContext ctx;

        // Insert pending records
        ctx.db->insertStemRecord ("orphan_1", "htdemucs_v4", "pending");
        ctx.db->insertStemRecord ("orphan_2", "htdemucs_v4", "pending");

        auto pending = ctx.db->getPendingStemHashes();
        expectEquals (pending.size(), 2);

        // Cleanup should remove them
        ctx.cache->cleanupOnStartup();

        pending = ctx.db->getPendingStemHashes();
        expectEquals (pending.size(), 0);
    }
};

static StemCacheTests stemCacheTests;
