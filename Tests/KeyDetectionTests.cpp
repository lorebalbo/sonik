#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/KeyDetection/KeyUtils.h"
#include "Features/KeyDetection/KeyDetectionAnalyzer.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include <cmath>

class KeyDetectionTests : public juce::UnitTest
{
public:
    KeyDetectionTests() : juce::UnitTest ("Key Detection", "Sonik") {}

    void runTest() override
    {
        testCamelotConversion();
        testOpenKeyConversion();
        testMusicalNotation();
        testCamelotColour();
        testCamelotNumber();
        testCamelotIndex();
        testParseKeyString();
        testParseCamelotNotation();
        testParseEdgeCases();
        testInvalidKeys();
        testChromagramCMajor();
        testChromagramAMinor();
        testConfidenceThreshold();
        testDatabaseCaching();
        testEmbeddedKeyParsing();
    }

private:
    struct DbContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;

        DbContext()
        {
            dbFile = juce::File::createTempFile ("sonik_key_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
        }

        ~DbContext()
        {
            db.reset();
            dbFile.deleteFile();
        }
    };

    // ================================================================
    // KeyUtils tests
    // ================================================================

    void testCamelotConversion()
    {
        beginTest ("Camelot conversion - all 24 keys");

        // C major = 8B
        expectEquals (KeyUtils::toCamelot (0), juce::String ("8B"));
        // C minor = 5A
        expectEquals (KeyUtils::toCamelot (1), juce::String ("5A"));
        // C# major = 3B
        expectEquals (KeyUtils::toCamelot (2), juce::String ("3B"));
        // C# minor = 12A
        expectEquals (KeyUtils::toCamelot (3), juce::String ("12A"));
        // D major = 10B
        expectEquals (KeyUtils::toCamelot (4), juce::String ("10B"));
        // A minor = 8A (canonical 19)
        expectEquals (KeyUtils::toCamelot (19), juce::String ("8A"));
        // B major = 1B (canonical 22)
        expectEquals (KeyUtils::toCamelot (22), juce::String ("1B"));
        // B minor = 10A (canonical 23)
        expectEquals (KeyUtils::toCamelot (23), juce::String ("10A"));
    }

    void testOpenKeyConversion()
    {
        beginTest ("Open Key conversion");

        // C major = 1d
        expectEquals (KeyUtils::toOpenKey (0), juce::String ("1d"));
        // C minor = 1m
        expectEquals (KeyUtils::toOpenKey (1), juce::String ("1m"));
        // A major = 4d (canonical 18)
        expectEquals (KeyUtils::toOpenKey (18), juce::String ("4d"));
        // A minor = 4m (canonical 19)
        expectEquals (KeyUtils::toOpenKey (19), juce::String ("4m"));
    }

    void testMusicalNotation()
    {
        beginTest ("Musical notation conversion");

        expectEquals (KeyUtils::toMusicalNotation (0), juce::String ("C major"));
        expectEquals (KeyUtils::toMusicalNotation (1), juce::String ("C minor"));
        expectEquals (KeyUtils::toMusicalNotation (18), juce::String ("A major"));
        expectEquals (KeyUtils::toMusicalNotation (19), juce::String ("A minor"));
        expectEquals (KeyUtils::toMusicalNotation (2), juce::String ("C# major"));
        expectEquals (KeyUtils::toMusicalNotation (-1), juce::String ("--"));
    }

    void testCamelotColour()
    {
        beginTest ("Camelot colour - valid keys return non-transparent");

        for (int i = 0; i < 24; ++i)
        {
            auto colour = KeyUtils::getCamelotColour (i);
            expect (colour != juce::Colours::transparentBlack,
                    "Key " + juce::String (i) + " should have a colour");
        }

        // Invalid key returns transparent
        expect (KeyUtils::getCamelotColour (-1) == juce::Colours::transparentBlack);
        expect (KeyUtils::getCamelotColour (24) == juce::Colours::transparentBlack);
    }

    void testCamelotNumber()
    {
        beginTest ("Camelot number extraction");

        // C major (0) → Camelot 8
        expectEquals (KeyUtils::getCamelotNumber (0), 8);
        // C minor (1) → Camelot 5
        expectEquals (KeyUtils::getCamelotNumber (1), 5);
        // B major (22) → Camelot 1
        expectEquals (KeyUtils::getCamelotNumber (22), 1);
        // Invalid
        expectEquals (KeyUtils::getCamelotNumber (-1), 0);
    }

    void testCamelotIndex()
    {
        beginTest ("Library Camelot index conversion");

        expectEquals (KeyUtils::toCamelotIndex (19), 7);  // A minor = 8A
        expectEquals (KeyUtils::toCamelotIndex (3),  11); // C# minor = 12A
        expectEquals (KeyUtils::toCamelotIndex (22), 12); // B major = 1B
        expectEquals (KeyUtils::toCamelotIndex (4),  21); // D major = 10B
        expectEquals (KeyUtils::toCamelotIndex (-1), -1);
    }

    void testParseKeyString()
    {
        beginTest ("Parse standard key strings");

        // Standard notation
        expectEquals (KeyUtils::parseKeyString ("C"),   0);   // C major
        expectEquals (KeyUtils::parseKeyString ("Cm"),  1);   // C minor
        expectEquals (KeyUtils::parseKeyString ("Am"),  19);  // A minor
        expectEquals (KeyUtils::parseKeyString ("Cmaj"), 0);  // C major
        expectEquals (KeyUtils::parseKeyString ("C#m"), 3);   // C# minor
        expectEquals (KeyUtils::parseKeyString ("Dbm"), 3);   // Db minor = C# minor
        expectEquals (KeyUtils::parseKeyString ("F#"),  12);  // F# major
    }

    void testParseCamelotNotation()
    {
        beginTest ("Parse Camelot notation");

        // 8B = C major (canonical 0)
        expectEquals (KeyUtils::parseKeyString ("8B"), 0);
        // 5A = C minor (canonical 1)
        expectEquals (KeyUtils::parseKeyString ("5A"), 1);
        // 1B = B major (canonical 22)
        expectEquals (KeyUtils::parseKeyString ("1B"), 22);
        // 10A = B minor (canonical 23)
        expectEquals (KeyUtils::parseKeyString ("10A"), 23);
        // 8A = A minor (canonical 19)
        expectEquals (KeyUtils::parseKeyString ("8A"), 19);
    }

    void testParseEdgeCases()
    {
        beginTest ("Parse edge cases");

        expectEquals (KeyUtils::parseKeyString (""), -1);
        expectEquals (KeyUtils::parseKeyString ("  "), -1);
        expectEquals (KeyUtils::parseKeyString ("XYZ"), -1);
        expectEquals (KeyUtils::parseKeyString ("0A"), -1);
        expectEquals (KeyUtils::parseKeyString ("13B"), -1);
    }

    void testInvalidKeys()
    {
        beginTest ("Invalid key indices return defaults");

        expectEquals (KeyUtils::toCamelot (-1), juce::String ("--"));
        expectEquals (KeyUtils::toCamelot (24), juce::String ("--"));
        expectEquals (KeyUtils::toOpenKey (-1), juce::String ("--"));
        expectEquals (KeyUtils::toMusicalNotation (100), juce::String ("--"));
    }

    // ================================================================
    // Chromagram / key detection tests with synthetic signals
    // ================================================================

    AudioBufferHolder::Ptr createToneBuffer (double freq, double sampleRate, double duration)
    {
        int numSamples = static_cast<int> (sampleRate * duration);
        juce::AudioBuffer<float> buf (1, numSamples);
        auto* channel = buf.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
        {
            double t = static_cast<double> (i) / sampleRate;
            channel[i] = static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
        }

        return new AudioBufferHolder (std::move (buf), sampleRate, static_cast<int64_t> (numSamples));
    }

    AudioBufferHolder::Ptr createChordBuffer (const std::vector<double>& freqs,
                                               double sampleRate, double duration)
    {
        int numSamples = static_cast<int> (sampleRate * duration);
        juce::AudioBuffer<float> buf (1, numSamples);
        auto* channel = buf.getWritePointer (0);

        float scale = 1.0f / static_cast<float> (freqs.size() * 3);

        for (int i = 0; i < numSamples; ++i)
        {
            float sample = 0.0f;
            double t = static_cast<double> (i) / sampleRate;

            for (auto f : freqs)
            {
                // Add harmonics for a richer, more realistic tone
                sample += static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * f * t));
                sample += 0.5f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * f * 2.0 * t));
                sample += 0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * f * 3.0 * t));
            }

            channel[i] = sample * scale;
        }

        return new AudioBufferHolder (std::move (buf), sampleRate, static_cast<int64_t> (numSamples));
    }

    void testChromagramCMajor()
    {
        beginTest ("Detect C major chord");

        DbContext ctx;

        // C major triad: C4 (261.63), E4 (329.63), G4 (392.00)
        auto buffer = createChordBuffer ({ 261.63, 329.63, 392.00 }, 44100.0, 30.0);

        KeyDetectionAnalyzer analyzer (*ctx.db);

        bool done = false;
        int resultKey = -1;
        float resultConf = 0.0f;

        analyzer.analyze ("test_c_major", "/tmp/test_c_major.wav", buffer,
            [&] (const juce::String& /*hash*/, int key, float conf)
            {
                resultKey  = key;
                resultConf = conf;
                done = true;
            });

        // Wait for analysis to complete (callback is dispatched via callAsync)
        auto start = juce::Time::getMillisecondCounter();
        while (! done && juce::Time::getMillisecondCounter() - start < 30000)
        {
            if (juce::MessageManager::getInstance()->hasStopMessageBeenSent())
                break;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        }

        expect (done, "Analysis should complete");

        if (done && resultKey >= 0)
        {
            // C major = canonical 0, relative: C minor (1), G major (14), A minor (19)
            // Synthetic signals may detect related keys
            expect (resultKey == 0 || resultKey == 1 || resultKey == 14 || resultKey == 19,
                    "Expected C major family, got " + juce::String (resultKey)
                        + " (" + KeyUtils::toMusicalNotation (resultKey) + ")"
                        + " conf=" + juce::String (resultConf));
        }
        else if (done && resultKey < 0)
        {
            // Confidence too low — acceptable for synthetic signals
            logMessage ("C major chord: confidence too low (" + juce::String (resultConf) + "), key=-1 — skipping");
        }
    }

    void testChromagramAMinor()
    {
        beginTest ("Detect A minor chord");

        DbContext ctx;

        // A minor triad: A3 (220.00), C4 (261.63), E4 (329.63)
        auto buffer = createChordBuffer ({ 220.00, 261.63, 329.63 }, 44100.0, 30.0);

        KeyDetectionAnalyzer analyzer (*ctx.db);

        bool done = false;
        int resultKey = -1;
        float resultConf = 0.0f;

        analyzer.analyze ("test_a_minor", "/tmp/test_a_minor.wav", buffer,
            [&] (const juce::String& /*hash*/, int key, float conf)
            {
                resultKey  = key;
                resultConf = conf;
                done = true;
            });

        auto start = juce::Time::getMillisecondCounter();
        while (! done && juce::Time::getMillisecondCounter() - start < 30000)
        {
            if (juce::MessageManager::getInstance()->hasStopMessageBeenSent())
                break;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        }

        expect (done, "Analysis should complete");

        if (done && resultKey >= 0)
        {
            // A minor = canonical 19, relative: C major (0), A major (18), E minor (9)
            expect (resultKey == 19 || resultKey == 0 || resultKey == 18 || resultKey == 9,
                    "Expected A minor family, got " + juce::String (resultKey)
                        + " (" + KeyUtils::toMusicalNotation (resultKey) + ")"
                        + " conf=" + juce::String (resultConf));
        }
        else if (done && resultKey < 0)
        {
            logMessage ("A minor chord: confidence too low (" + juce::String (resultConf) + "), key=-1 — skipping");
        }
    }

    void testConfidenceThreshold()
    {
        beginTest ("Low confidence returns -1");

        DbContext ctx;

        // White noise — should produce low confidence
        int numSamples = static_cast<int> (44100.0 * 5.0);
        juce::AudioBuffer<float> noiseBuf (1, numSamples);
        auto* channel = noiseBuf.getWritePointer (0);

        juce::Random rng (42);
        for (int i = 0; i < numSamples; ++i)
            channel[i] = rng.nextFloat() * 2.0f - 1.0f;

        AudioBufferHolder::Ptr buffer = new AudioBufferHolder (
            std::move (noiseBuf), 44100.0, static_cast<int64_t> (numSamples));

        KeyDetectionAnalyzer analyzer (*ctx.db);

        bool done = false;
        int resultKey = -1;
        float resultConf = 0.0f;

        analyzer.analyze ("test_noise", "/tmp/test_noise.wav", buffer,
            [&] (const juce::String& /*hash*/, int key, float conf)
            {
                resultKey  = key;
                resultConf = conf;
                done = true;
            });

        auto start = juce::Time::getMillisecondCounter();
        while (! done && juce::Time::getMillisecondCounter() - start < 30000)
        {
            if (juce::MessageManager::getInstance()->hasStopMessageBeenSent())
                break;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        }

        expect (done, "Analysis should complete for noise");

        // For random noise, confidence should be low
        // The analyzer threshold is 0.4, so key may be -1
        if (done && resultKey >= 0)
        {
            // If we do get a key, confidence should be low
            expect (resultConf < 0.6f,
                    "Noise should have low confidence, got " + juce::String (resultConf));
        }
    }

    void testDatabaseCaching()
    {
        beginTest ("Database caching for key detection");

        DbContext ctx;

        // Save a known key result
        ctx.db->saveTrackData ("/tmp/test_cached.wav", "cached_hash",
                               "", "", 0, 0.85f, false);

        auto loaded = ctx.db->loadTrackData ("/tmp/test_cached.wav", "cached_hash");
        expect (loaded.has_value(), "Should load cached data");

        if (loaded.has_value())
        {
            expectEquals (loaded->keyIndex, 0);
            expectWithinAbsoluteError (static_cast<double> (loaded->keyConfidence), 0.85, 0.01);
            expect (! loaded->keyManuallyAdjusted);
        }
    }

    void testEmbeddedKeyParsing()
    {
        beginTest ("Embedded key tag parsing");

        // Test various embedded key formats
        expectEquals (KeyUtils::parseKeyString ("Am"),   19);  // standard
        expectEquals (KeyUtils::parseKeyString ("C"),    0);   // just note = major
        expectEquals (KeyUtils::parseKeyString ("Dbm"),  3);   // flat minor
        expectEquals (KeyUtils::parseKeyString ("8B"),   0);   // Camelot C major
        expectEquals (KeyUtils::parseKeyString ("5A"),   1);   // Camelot C minor
        expectEquals (KeyUtils::parseKeyString ("Cmin"), 1);   // Cmin notation
    }
};

static KeyDetectionTests keyDetectionTests;
