#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Waveform/WaveformData.h"
#include "Features/Deck/Database/TrackDatabase.h"

class WaveformTests : public juce::UnitTest
{
public:
    WaveformTests() : juce::UnitTest ("Waveform Analysis and Display", "Sonik") {}

    void runTest() override
    {
        testDefaultConstruction();
        testGetBestLevel();
        testSamplePositionToPixelX();
        testPixelXToSamplePosition();
        testSerializeAndDeserialize();
        testEmptyDataSerialization();
        testWaveformPointDefaultValues();
        testWaveformPointPeakAndRmsValues();
        testMipmapLevelSizes();
        testDatabaseStoreAndLoad();
        testDatabaseLoadNonExistent();
        testDatabaseOverwrite();
        testSamplePositionToPixelXAtZero();
        testSamplePositionToPixelXAtEnd();
    }

private:
    // Helper to create a temp database
    struct DbContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;

        DbContext()
        {
            dbFile = juce::File::createTempFile ("sonik_waveform_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
        }

        ~DbContext()
        {
            db.reset();
            dbFile.deleteFile();
        }
    };

    // Helper to build WaveformData with known mipmap structure
    WaveformData::Ptr makeTestData (int basePoints, double sr = 44100.0)
    {
        auto data = WaveformData::Ptr (new WaveformData());
        data->sampleRate   = sr;
        data->totalSamples = static_cast<int64_t> (basePoints) * WaveformData::baseSamplesPerPoint;
        data->contentHash  = "test_hash";
        data->levels.resize (WaveformData::numMipmapLevels);

        // Fill base level
        data->levels[0].resize (static_cast<size_t> (basePoints));
        for (int i = 0; i < basePoints; ++i)
        {
            auto& p = data->levels[0][static_cast<size_t> (i)];
            float v = static_cast<float> (i) / static_cast<float> (basePoints);
            p.peakL     = v;
            p.peakR     = v * 0.8f;
            p.rmsL      = v * 0.5f;
            p.rmsR      = v * 0.4f;
            p.energyLow  = v * 0.3f;
            p.energyMid  = v * 0.2f;
            p.energyHigh = v * 0.1f;
        }

        // Build mipmap levels (halving each time)
        for (int level = 1; level < WaveformData::numMipmapLevels; ++level)
        {
            const auto& prev = data->levels[static_cast<size_t> (level - 1)];
            auto prevSize = static_cast<int> (prev.size());
            int newSize = (prevSize + 1) / 2;
            data->levels[static_cast<size_t> (level)].resize (static_cast<size_t> (newSize));

            for (int j = 0; j < newSize; ++j)
            {
                int idx0 = j * 2;
                int idx1 = juce::jmin (idx0 + 1, prevSize - 1);
                auto& dst = data->levels[static_cast<size_t> (level)][static_cast<size_t> (j)];
                const auto& a = prev[static_cast<size_t> (idx0)];
                const auto& b = prev[static_cast<size_t> (idx1)];
                dst.peakL = juce::jmax (a.peakL, b.peakL);
                dst.peakR = juce::jmax (a.peakR, b.peakR);
                dst.rmsL  = std::sqrt ((a.rmsL * a.rmsL + b.rmsL * b.rmsL) * 0.5f);
                dst.rmsR  = std::sqrt ((a.rmsR * a.rmsR + b.rmsR * b.rmsR) * 0.5f);
                dst.energyLow  = juce::jmax (a.energyLow, b.energyLow);
                dst.energyMid  = juce::jmax (a.energyMid, b.energyMid);
                dst.energyHigh = juce::jmax (a.energyHigh, b.energyHigh);
            }
        }

        return data;
    }

    // -----------------------------------------------------------------------
    void testDefaultConstruction()
    {
        beginTest ("WaveformData - default construction has 0 levels and sampleRate 0");
        WaveformData data;
        expectEquals (static_cast<int> (data.levels.size()), 0);
        expectEquals (data.sampleRate, 0.0);
        expectEquals (data.totalSamples, static_cast<int64_t> (0));
        expect (data.contentHash.isEmpty());
    }

    // -----------------------------------------------------------------------
    void testGetBestLevel()
    {
        beginTest ("WaveformData - getBestLevel returns correct level for samplesPerPixel");
        auto data = makeTestData (1024);

        // baseSamplesPerPoint = 256
        // level 0 spp = 256, level 1 = 512, level 2 = 1024, level 3 = 2048, etc.

        // Small spp should return level 0
        expectEquals (data->getBestLevel (100.0), 0);
        expectEquals (data->getBestLevel (256.0), 0);

        // spp just above 256 should get level 1 (512 >= 300)
        expectEquals (data->getBestLevel (300.0), 1);

        // spp 512 should match level 1
        expectEquals (data->getBestLevel (512.0), 1);

        // spp above all levels returns last level
        expectEquals (data->getBestLevel (100000.0), WaveformData::numMipmapLevels - 1);
    }

    // -----------------------------------------------------------------------
    void testSamplePositionToPixelX()
    {
        beginTest ("WaveformData - samplePositionToPixelX maps correctly");
        auto data = makeTestData (100);

        float width = 800.0f;
        int level = 0;
        int64_t totalSamples = data->totalSamples; // 100 * 256 = 25600

        // Midpoint: sample 12800 should map to ~400
        float midPixel = data->samplePositionToPixelX (totalSamples / 2, level, width);
        expectWithinAbsoluteError (midPixel, 400.0f, 1.0f);

        // Quarter: sample 6400 should map to ~200
        float qtrPixel = data->samplePositionToPixelX (totalSamples / 4, level, width);
        expectWithinAbsoluteError (qtrPixel, 200.0f, 1.0f);
    }

    // -----------------------------------------------------------------------
    void testPixelXToSamplePosition()
    {
        beginTest ("WaveformData - pixelXToSamplePosition is inverse of samplePositionToPixelX");
        auto data = makeTestData (100);

        float width = 800.0f;
        int level = 0;

        // Start from a known sample position, convert to pixel, convert back
        int64_t origSample = 12800;
        float pixel = data->samplePositionToPixelX (origSample, level, width);
        int64_t recovered = data->pixelXToSamplePosition (pixel, level, width);

        // Allow small rounding error (< baseSamplesPerPoint)
        expect (std::abs (recovered - origSample) < WaveformData::baseSamplesPerPoint);
    }

    // -----------------------------------------------------------------------
    void testSerializeAndDeserialize()
    {
        beginTest ("WaveformData - serialize and deserialize preserves data");
        auto original = makeTestData (64);

        auto blob = original->serialize();
        auto restored = WaveformData::deserialize (blob, "test_hash");

        expect (restored != nullptr);
        expectEquals (restored->sampleRate, original->sampleRate);
        expectEquals (restored->totalSamples, original->totalSamples);
        expectEquals (restored->contentHash, juce::String ("test_hash"));
        expectEquals (static_cast<int> (restored->levels.size()),
                      static_cast<int> (original->levels.size()));

        // Verify point values at base level
        for (size_t i = 0; i < original->levels[0].size(); ++i)
        {
            const auto& a = original->levels[0][i];
            const auto& b = restored->levels[0][i];
            expectWithinAbsoluteError (b.peakL, a.peakL, 1e-6f);
            expectWithinAbsoluteError (b.peakR, a.peakR, 1e-6f);
            expectWithinAbsoluteError (b.rmsL, a.rmsL, 1e-6f);
            expectWithinAbsoluteError (b.rmsR, a.rmsR, 1e-6f);
            expectWithinAbsoluteError (b.energyLow, a.energyLow, 1e-6f);
            expectWithinAbsoluteError (b.energyMid, a.energyMid, 1e-6f);
            expectWithinAbsoluteError (b.energyHigh, a.energyHigh, 1e-6f);
        }
    }

    // -----------------------------------------------------------------------
    void testEmptyDataSerialization()
    {
        beginTest ("WaveformData - empty data serializes and deserializes");
        WaveformData empty;
        auto blob = empty.serialize();
        auto restored = WaveformData::deserialize (blob, "empty_hash");

        expect (restored != nullptr);
        expectEquals (restored->sampleRate, 0.0);
        expectEquals (restored->totalSamples, static_cast<int64_t> (0));
        expectEquals (static_cast<int> (restored->levels.size()), 0);
    }

    // -----------------------------------------------------------------------
    void testWaveformPointDefaultValues()
    {
        beginTest ("WaveformPoint - default values are all 0.0f");
        WaveformPoint point;
        expectEquals (point.peakL, 0.0f);
        expectEquals (point.peakR, 0.0f);
        expectEquals (point.rmsL, 0.0f);
        expectEquals (point.rmsR, 0.0f);
        expectEquals (point.energyLow, 0.0f);
        expectEquals (point.energyMid, 0.0f);
        expectEquals (point.energyHigh, 0.0f);
    }

    // -----------------------------------------------------------------------
    void testWaveformPointPeakAndRmsValues()
    {
        beginTest ("WaveformPoint - stores peak and RMS values correctly");
        WaveformPoint point;
        point.peakL     = 0.95f;
        point.peakR     = 0.85f;
        point.rmsL      = 0.70f;
        point.rmsR      = 0.60f;
        point.energyLow  = 0.50f;
        point.energyMid  = 0.40f;
        point.energyHigh = 0.30f;

        expectEquals (point.peakL, 0.95f);
        expectEquals (point.peakR, 0.85f);
        expectEquals (point.rmsL, 0.70f);
        expectEquals (point.rmsR, 0.60f);
        expectEquals (point.energyLow, 0.50f);
        expectEquals (point.energyMid, 0.40f);
        expectEquals (point.energyHigh, 0.30f);
    }

    // -----------------------------------------------------------------------
    void testMipmapLevelSizes()
    {
        beginTest ("WaveformData - mipmap level sizes halve at each level");
        const int baseSize = 1024;
        auto data = makeTestData (baseSize);

        expectEquals (static_cast<int> (data->levels.size()), WaveformData::numMipmapLevels);

        int expectedSize = baseSize;
        for (int i = 0; i < WaveformData::numMipmapLevels; ++i)
        {
            expectEquals (static_cast<int> (data->levels[static_cast<size_t> (i)].size()), expectedSize);
            expectedSize = (expectedSize + 1) / 2;
        }
    }

    // -----------------------------------------------------------------------
    void testDatabaseStoreAndLoad()
    {
        beginTest ("WaveformCache - store and load returns identical data");
        DbContext ctx;

        // Create test waveform data and serialize it
        auto data = makeTestData (32);
        auto blob = data->serialize();

        ctx.db->storeWaveformData ("hash_store_test", blob);

        juce::MemoryBlock loaded;
        bool found = ctx.db->loadWaveformData ("hash_store_test", loaded);
        expect (found);
        expectEquals (loaded.getSize(), blob.getSize());
        expect (loaded == blob);
    }

    // -----------------------------------------------------------------------
    void testDatabaseLoadNonExistent()
    {
        beginTest ("WaveformCache - load non-existent hash returns false");
        DbContext ctx;

        juce::MemoryBlock loaded;
        bool found = ctx.db->loadWaveformData ("no_such_hash", loaded);
        expect (! found);
    }

    // -----------------------------------------------------------------------
    void testDatabaseOverwrite()
    {
        beginTest ("WaveformCache - overwrite existing data returns latest");
        DbContext ctx;

        // Store first version
        auto data1 = makeTestData (16);
        auto blob1 = data1->serialize();
        ctx.db->storeWaveformData ("hash_overwrite", blob1);

        // Store second version (different size)
        auto data2 = makeTestData (32);
        auto blob2 = data2->serialize();
        ctx.db->storeWaveformData ("hash_overwrite", blob2);

        juce::MemoryBlock loaded;
        bool found = ctx.db->loadWaveformData ("hash_overwrite", loaded);
        expect (found);
        expectEquals (loaded.getSize(), blob2.getSize());
        expect (loaded == blob2);
    }

    // -----------------------------------------------------------------------
    void testSamplePositionToPixelXAtZero()
    {
        beginTest ("WaveformData - samplePositionToPixelX at position 0 returns 0");
        auto data = makeTestData (100);

        float pixel = data->samplePositionToPixelX (0, 0, 800.0f);
        expectEquals (pixel, 0.0f);
    }

    // -----------------------------------------------------------------------
    void testSamplePositionToPixelXAtEnd()
    {
        beginTest ("WaveformData - samplePositionToPixelX at end returns near componentWidth");
        auto data = makeTestData (100);

        float width = 800.0f;
        float pixel = data->samplePositionToPixelX (data->totalSamples, 0, width);
        expectWithinAbsoluteError (pixel, width, 1.0f);
    }
};

static WaveformTests waveformTests;
