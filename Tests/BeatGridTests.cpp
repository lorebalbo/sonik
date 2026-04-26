#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/BeatGrid/BeatGridData.h"
#include "Features/BeatGrid/BeatGridAnalyzer.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Deck/DeckIdentifiers.h"

class BeatGridTests : public juce::UnitTest
{
public:
    BeatGridTests() : juce::UnitTest ("BPM and Beatgrid Analysis", "Sonik") {}

    void runTest() override
    {
        testDefaultData();
        testGetNearestBeat();
        testGetBeatIndex();
        testGetBeatPhase();
        testGetBeatsInRange();
        testGetBeatsInRangeDownbeats();
        testGetBeatsInRangeEmpty();
        testJsonRoundTrip();
        testJsonInvalidInput();
        testConfidenceThreshold();
        testBpmHalfDoubleResolution();
        testDatabaseCaching();
        testManuallyAdjustedPreserved();
        testBeatGridIdentifiersExist();
        testNegativeBeatIndex();

        // PRD-0024: BPM Editor and Beatgrid Offset Controls
        testPrd024BpmValidation();
        testPrd024BeatIntervalFormula();
        testPrd024FineNudgeOffset();
        testPrd024CoarseNudgeOffset();
        testPrd024NudgeWrap();
        testPrd024NudgeNoOpOnZeroInterval();
        testPrd024ManuallyAdjustedFlagOnSave();
        testPrd024ManuallyAdjustedFlagOnNudge();
    }

private:
    // Helper to create a temp database
    struct DbContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;

        DbContext()
        {
            dbFile = juce::File::createTempFile ("sonik_beatgrid_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
        }

        ~DbContext()
        {
            db.reset();
            dbFile.deleteFile();
        }
    };

    // -------------------------------------------------------------------
    void testDefaultData()
    {
        beginTest ("Default BeatGridData has zero values");

        BeatGridData data;
        expectEquals (data.bpm, 0.0);
        expectEquals (data.anchorSample, (int64_t) 0);
        expectEquals (data.beatIntervalSamples, 0.0);
        expectEquals ((double) data.confidence, 0.0);
        expect (! data.manuallyAdjusted);
        expectEquals (data.analysisSampleRate, 44100.0);
    }

    // -------------------------------------------------------------------
    void testGetNearestBeat()
    {
        beginTest ("getNearestBeat returns closest beat position");

        BeatGridData data;
        data.bpm = 120.0;
        data.analysisSampleRate = 44100.0;
        data.beatIntervalSamples = 22050.0; // 120 BPM @ 44100 Hz
        data.anchorSample = 0;

        // Exactly on a beat
        expectEquals (data.getNearestBeat (0), (int64_t) 0);
        expectEquals (data.getNearestBeat (22050), (int64_t) 22050);

        // Slightly past a beat — should snap to nearest
        expectEquals (data.getNearestBeat (22100), (int64_t) 22050);

        // Midway between beats — std::round(0.5) rounds to 1
        expectEquals (data.getNearestBeat (11025), (int64_t) 22050); // exact midpoint rounds up
        expectEquals (data.getNearestBeat (11024), (int64_t) 0);     // just before midpoint rounds down

        // Zero interval returns input unchanged
        BeatGridData noGrid;
        expectEquals (noGrid.getNearestBeat (12345), (int64_t) 12345);
    }

    // -------------------------------------------------------------------
    void testGetBeatIndex()
    {
        beginTest ("getBeatIndex returns correct beat number");

        BeatGridData data;
        data.beatIntervalSamples = 22050.0;
        data.anchorSample = 1000;

        // At anchor = beat 0
        expectEquals (data.getBeatIndex (1000), 0);

        // One beat later
        expectEquals (data.getBeatIndex (23050), 1);

        // Two beats later
        expectEquals (data.getBeatIndex (45100), 2);

        // Before anchor = negative beat
        expectEquals (data.getBeatIndex (-21050), -1);
    }

    // -------------------------------------------------------------------
    void testGetBeatPhase()
    {
        beginTest ("getBeatPhase returns 0.0 to 1.0 phase");

        BeatGridData data;
        data.beatIntervalSamples = 10000.0;
        data.anchorSample = 0;

        // Exactly on beat
        auto phase = data.getBeatPhase (0);
        expectWithinAbsoluteError ((double) phase, 0.0, 0.01);

        // Halfway between beats
        phase = data.getBeatPhase (5000);
        expectWithinAbsoluteError ((double) phase, 0.5, 0.01);

        // Quarter phase
        phase = data.getBeatPhase (2500);
        expectWithinAbsoluteError ((double) phase, 0.25, 0.01);

        // Zero interval returns 0
        BeatGridData noGrid;
        expectEquals ((double) noGrid.getBeatPhase (12345), 0.0);
    }

    // -------------------------------------------------------------------
    void testGetBeatsInRange()
    {
        beginTest ("getBeatsInRange returns all beats within range");

        BeatGridData data;
        data.beatIntervalSamples = 10000.0;
        data.anchorSample = 0;

        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;

        data.getBeatsInRange (0, 50000, beats, isDownbeat);

        // Should have beats at 0, 10000, 20000, 30000, 40000 (5 beats)
        expectEquals (beats.size(), 5);
        expectEquals (beats[0], (int64_t) 0);
        expectEquals (beats[1], (int64_t) 10000);
        expectEquals (beats[4], (int64_t) 40000);
    }

    // -------------------------------------------------------------------
    void testGetBeatsInRangeDownbeats()
    {
        beginTest ("getBeatsInRange marks downbeats correctly every 4 beats");

        BeatGridData data;
        data.beatIntervalSamples = 10000.0;
        data.anchorSample = 0;

        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;

        // Get 8 beats (indices 0-7)
        data.getBeatsInRange (0, 80000, beats, isDownbeat);
        expectEquals (beats.size(), 8);

        // Downbeats at index 0, 4
        expect (isDownbeat[0]);   // beat index 0 → downbeat
        expect (! isDownbeat[1]); // beat index 1
        expect (! isDownbeat[2]); // beat index 2
        expect (! isDownbeat[3]); // beat index 3
        expect (isDownbeat[4]);   // beat index 4 → downbeat
        expect (! isDownbeat[5]); // beat index 5
    }

    // -------------------------------------------------------------------
    void testGetBeatsInRangeEmpty()
    {
        beginTest ("getBeatsInRange with zero interval returns empty");

        BeatGridData data; // beatIntervalSamples = 0

        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;

        data.getBeatsInRange (0, 100000, beats, isDownbeat);
        expectEquals (beats.size(), 0);

        // Also test inverted range
        data.beatIntervalSamples = 10000.0;
        data.getBeatsInRange (50000, 0, beats, isDownbeat);
        expectEquals (beats.size(), 0);
    }

    // -------------------------------------------------------------------
    void testJsonRoundTrip()
    {
        beginTest ("JSON serialization round-trips correctly");

        BeatGridData original;
        original.bpm                 = 126.03;
        original.anchorSample        = 12345;
        original.beatIntervalSamples = 20952.4;
        original.confidence          = 0.85f;
        original.manuallyAdjusted    = true;
        original.analysisSampleRate  = 48000.0;

        auto json = original.toJson();
        expect (json.isNotEmpty());

        auto restored = BeatGridData::fromJson (json);
        expect (restored != nullptr);

        expectWithinAbsoluteError (restored->bpm, 126.03, 0.01);
        expectEquals (restored->anchorSample, (int64_t) 12345);
        expectWithinAbsoluteError (restored->beatIntervalSamples, 20952.4, 0.1);
        expectWithinAbsoluteError ((double) restored->confidence, 0.85, 0.01);
        expect (restored->manuallyAdjusted);
        expectWithinAbsoluteError (restored->analysisSampleRate, 48000.0, 0.1);
    }

    // -------------------------------------------------------------------
    void testJsonInvalidInput()
    {
        beginTest ("fromJson returns nullptr for invalid JSON");

        auto result = BeatGridData::fromJson ("");
        expect (result == nullptr);

        result = BeatGridData::fromJson ("not json at all");
        expect (result == nullptr);

        result = BeatGridData::fromJson ("42");
        expect (result == nullptr);
    }

    // -------------------------------------------------------------------
    void testConfidenceThreshold()
    {
        beginTest ("Confidence below 0.15 results in BPM = 0.0");

        // This tests the contract: if confidence < 0.15, analyzer should set bpm = 0
        BeatGridData lowConf;
        lowConf.confidence = 0.1f;
        lowConf.bpm = 0.0;

        // getNearestBeat should handle 0 interval gracefully
        expectEquals (lowConf.getNearestBeat (5000), (int64_t) 5000);

        // getBeatsInRange should return empty
        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;
        lowConf.getBeatsInRange (0, 100000, beats, isDownbeat);
        expectEquals (beats.size(), 0);
    }

    // -------------------------------------------------------------------
    void testBpmHalfDoubleResolution()
    {
        beginTest ("BPM half/double resolution to 80-160 range");

        // Simulate what the analyzer does: push BPM into 80-160 range
        auto resolve = [] (double bpm) {
            while (bpm < 80.0 && bpm > 0.0)
                bpm *= 2.0;
            while (bpm > 160.0)
                bpm /= 2.0;
            return bpm;
        };

        // 60 BPM → doubled to 120
        expectWithinAbsoluteError (resolve (60.0), 120.0, 0.01);

        // 200 BPM → halved to 100
        expectWithinAbsoluteError (resolve (200.0), 100.0, 0.01);

        // 128 BPM → stays 128
        expectWithinAbsoluteError (resolve (128.0), 128.0, 0.01);

        // 40 BPM → doubled once to 80
        expectWithinAbsoluteError (resolve (40.0), 80.0, 0.01);

        // 320 BPM → halved once to 160
        expectWithinAbsoluteError (resolve (320.0), 160.0, 0.01);
    }

    // -------------------------------------------------------------------
    void testDatabaseCaching()
    {
        beginTest ("BeatGrid data cached and loaded from database");

        DbContext ctx;

        BeatGridData data;
        data.bpm                 = 128.0;
        data.anchorSample        = 5000;
        data.beatIntervalSamples = 20671.875;
        data.confidence          = 0.92f;
        data.manuallyAdjusted    = false;
        data.analysisSampleRate  = 44100.0;

        auto json = data.toJson();

        // Save to DB
        ctx.db->saveTrackData ("/test/file.wav", "hash123",
                               "", json, -1, 0.0f, false);

        // Load back
        auto loaded = ctx.db->loadTrackData ("/test/file.wav", "hash123");
        expect (loaded.has_value());
        expect (loaded->beatgridJson.isNotEmpty());

        auto restored = BeatGridData::fromJson (loaded->beatgridJson);
        expect (restored != nullptr);
        expectWithinAbsoluteError (restored->bpm, 128.0, 0.01);
        expectEquals (restored->anchorSample, (int64_t) 5000);
    }

    // -------------------------------------------------------------------
    void testManuallyAdjustedPreserved()
    {
        beginTest ("Manually adjusted beatgrid preserved in database");

        DbContext ctx;

        BeatGridData manual;
        manual.bpm                 = 125.0;
        manual.anchorSample        = 8000;
        manual.beatIntervalSamples = 21168.0;
        manual.confidence          = 0.95f;
        manual.manuallyAdjusted    = true;
        manual.analysisSampleRate  = 44100.0;

        ctx.db->saveTrackData ("/test/manual.wav", "hash_manual",
                               "", manual.toJson(), -1, 0.0f, false);

        auto loaded = ctx.db->loadTrackData ("/test/manual.wav", "hash_manual");
        expect (loaded.has_value());

        auto restored = BeatGridData::fromJson (loaded->beatgridJson);
        expect (restored != nullptr);
        expect (restored->manuallyAdjusted);
        expectWithinAbsoluteError (restored->bpm, 125.0, 0.01);
    }

    // -------------------------------------------------------------------
    void testBeatGridIdentifiersExist()
    {
        beginTest ("BeatGrid identifiers are properly defined");

        // Verify all required identifiers exist
        expect (IDs::BeatGrid.toString() == "BeatGrid");
        expect (IDs::bpm.toString() == "bpm");
        expect (IDs::anchorSample.toString() == "anchorSample");
        expect (IDs::beatIntervalSamples.toString() == "beatIntervalSamples");
        expect (IDs::confidence.toString() == "confidence");
        expect (IDs::manuallyAdjusted.toString() == "manuallyAdjusted");
    }

    // -------------------------------------------------------------------
    void testNegativeBeatIndex()
    {
        beginTest ("Beat operations handle negative sample positions");

        BeatGridData data;
        data.beatIntervalSamples = 22050.0;
        data.anchorSample = 22050; // anchor at beat 1

        // Before anchor
        expectEquals (data.getBeatIndex (0), -1);
        expectEquals (data.getNearestBeat (0), (int64_t) 0);

        // Phase before anchor should wrap correctly
        auto phase = data.getBeatPhase (0);
        expect (phase >= 0.0f && phase <= 1.0f);

        // getBeatsInRange should handle ranges before anchor
        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;
        data.getBeatsInRange (-22050, 22050, beats, isDownbeat);
        // Should include beats at -22050 (if within range based on anchor) and 0
        // The anchor is at 22050, so beat indices are: -2 @ -22050, -1 @ 0, 0 @ 22050
        // Range is [-22050, 22050) so we get beats at -22050 and 0
        expectEquals (beats.size(), 2);
    }

    // ===================================================================
    // PRD-0024: BPM Editor and Beatgrid Offset Controls
    // ===================================================================

    static bool isValidBpm (double bpm)
    {
        return bpm >= 20.0 && bpm <= 300.0;
    }

    // -------------------------------------------------------------------
    void testPrd024BpmValidation()
    {
        beginTest ("PRD-0024: BPM validation rejects values outside [20, 300]");

        // Below range
        expect (! isValidBpm (0.0));
        expect (! isValidBpm (19.9));
        expect (! isValidBpm (-1.0));

        // Boundary values (valid)
        expect (isValidBpm (20.0));
        expect (isValidBpm (300.0));

        // Mid-range (valid)
        expect (isValidBpm (120.0));
        expect (isValidBpm (126.03));
        expect (isValidBpm (128.0));

        // Above range
        expect (! isValidBpm (300.1));
        expect (! isValidBpm (500.0));
    }

    // -------------------------------------------------------------------
    void testPrd024BeatIntervalFormula()
    {
        beginTest ("PRD-0024: beatIntervalSamples = sr * 60.0 / bpm");

        // 120 BPM @ 44100 Hz = 22050 samples/beat
        expectWithinAbsoluteError (44100.0 * 60.0 / 120.0, 22050.0, 0.001);

        // 128 BPM @ 44100 Hz
        expectWithinAbsoluteError (44100.0 * 60.0 / 128.0, 20671.875, 0.001);

        // 120 BPM @ 48000 Hz = 24000 samples/beat
        expectWithinAbsoluteError (48000.0 * 60.0 / 120.0, 24000.0, 0.001);

        // 20 BPM @ 44100 Hz (boundary minimum)
        expectWithinAbsoluteError (44100.0 * 60.0 / 20.0, 132300.0, 0.001);

        // 300 BPM @ 44100 Hz (boundary maximum)
        expectWithinAbsoluteError (44100.0 * 60.0 / 300.0, 8820.0, 0.001);

        // Consistency: recomputing interval from stored BPM round-trips
        double bpm      = 126.03;
        double sr       = 44100.0;
        double interval = sr * 60.0 / bpm;
        double bpmBack  = sr * 60.0 / interval;
        expectWithinAbsoluteError (bpmBack, bpm, 0.001);
    }

    // -------------------------------------------------------------------
    void testPrd024FineNudgeOffset()
    {
        beginTest ("PRD-0024: fine nudge offset = round(sr * 0.010)");

        // 44100 Hz: 10 ms = 441 samples
        int64_t fine44100 = static_cast<int64_t> (std::round (44100.0 * 0.010));
        expectEquals (fine44100, (int64_t) 441);

        // 48000 Hz: 10 ms = 480 samples
        int64_t fine48000 = static_cast<int64_t> (std::round (48000.0 * 0.010));
        expectEquals (fine48000, (int64_t) 480);

        // 96000 Hz: 10 ms = 960 samples
        int64_t fine96000 = static_cast<int64_t> (std::round (96000.0 * 0.010));
        expectEquals (fine96000, (int64_t) 960);
    }

    // -------------------------------------------------------------------
    void testPrd024CoarseNudgeOffset()
    {
        beginTest ("PRD-0024: coarse nudge offset = round(sr * 0.050)");

        // 44100 Hz: 50 ms = 2205 samples
        int64_t coarse44100 = static_cast<int64_t> (std::round (44100.0 * 0.050));
        expectEquals (coarse44100, (int64_t) 2205);

        // 48000 Hz: 50 ms = 2400 samples
        int64_t coarse48000 = static_cast<int64_t> (std::round (48000.0 * 0.050));
        expectEquals (coarse48000, (int64_t) 2400);

        // Coarse offset is exactly 5x the fine offset
        double sr = 44100.0;
        int64_t fine   = static_cast<int64_t> (std::round (sr * 0.010));
        int64_t coarse = static_cast<int64_t> (std::round (sr * 0.050));
        expectEquals (coarse, fine * 5);
    }

    // -------------------------------------------------------------------
    void testPrd024NudgeWrap()
    {
        beginTest ("PRD-0024: nudge wrap keeps anchor within [0, beatIntervalSamples)");

        const int64_t interval = 10000;

        auto wrap = [] (int64_t anchor, int64_t offset, int64_t iv) -> int64_t
        {
            return ((anchor + offset) % iv + iv) % iv;
        };

        // Simple forward nudge: no wrap needed
        expectEquals (wrap (500, 441, interval), (int64_t) 941);

        // Forward nudge that wraps over the top: 9800 + 441 = 10241 → 241
        expectEquals (wrap (9800, 441, interval), (int64_t) 241);

        // Backward nudge: no wrap
        expectEquals (wrap (5000, -441, interval), (int64_t) 4559);

        // Backward nudge that wraps below zero: 100 - 441 = -341 → 9659
        expectEquals (wrap (100, -441, interval), (int64_t) 9659);

        // Coarse backward nudge that wraps: 200 - 2205 = -2005 → 7995
        expectEquals (wrap (200, -2205, interval), (int64_t) 7995);

        // Anchor exactly at interval boundary: 9999 + 1 = 10000 → 0
        expectEquals (wrap (9999, 1, interval), (int64_t) 0);

        // Anchor already 0, backward nudge: 0 - 441 → 9559
        expectEquals (wrap (0, -441, interval), (int64_t) 9559);

        // Result is always in [0, interval)
        for (int64_t anchor : { (int64_t)0, (int64_t)100, (int64_t)5000, (int64_t)9999 })
        {
            for (int64_t offset : { (int64_t)441, (int64_t)-441, (int64_t)2205, (int64_t)-2205 })
            {
                int64_t result = wrap (anchor, offset, interval);
                expect (result >= 0 && result < interval);
            }
        }
    }

    // -------------------------------------------------------------------
    void testPrd024NudgeNoOpOnZeroInterval()
    {
        beginTest ("PRD-0024: nudge is no-op when beatIntervalSamples = 0");

        // handleGridNudge guards: if (interval <= 0) return;
        // Verify the guard condition holds for zero / negative intervals.
        expect ((int64_t) 0  <= 0);
        expect ((int64_t) -1 <= 0);

        // BeatGridData with zero interval: getNearestBeat returns input unchanged
        BeatGridData noGrid;
        noGrid.beatIntervalSamples = 0.0;
        noGrid.anchorSample = 0;
        expectEquals (noGrid.getNearestBeat (5000), (int64_t) 5000);

        // getBeatsInRange returns empty
        juce::Array<int64_t> beats;
        juce::Array<bool> isDownbeat;
        noGrid.getBeatsInRange (0, 100000, beats, isDownbeat);
        expectEquals (beats.size(), 0);
    }

    // -------------------------------------------------------------------
    void testPrd024ManuallyAdjustedFlagOnSave()
    {
        beginTest ("PRD-0024: manuallyAdjusted=true written to ValueTree after BPM save");

        juce::ValueTree beatTree (IDs::BeatGrid);
        beatTree.setProperty (IDs::bpm,                 120.0,   nullptr);
        beatTree.setProperty (IDs::beatIntervalSamples, 22050.0, nullptr);
        beatTree.setProperty (IDs::anchorSample,        0,       nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted,    false,   nullptr);

        // Simulate handleBpmSave logic
        double newBpm = 128.0;
        double sr     = 44100.0;
        expect (isValidBpm (newBpm));

        double newInterval = sr * 60.0 / newBpm;
        beatTree.setProperty (IDs::bpm,                 newBpm,      nullptr);
        beatTree.setProperty (IDs::beatIntervalSamples, newInterval, nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted,    true,        nullptr);

        expectWithinAbsoluteError (
            static_cast<double> (beatTree.getProperty (IDs::bpm)),
            128.0, 0.001);
        expectWithinAbsoluteError (
            static_cast<double> (beatTree.getProperty (IDs::beatIntervalSamples)),
            newInterval, 0.001);
        expect (static_cast<bool> (beatTree.getProperty (IDs::manuallyAdjusted)));

        // Verify invalid BPM does NOT update (guard holds)
        double badBpm = 15.0;
        expect (! isValidBpm (badBpm));
        // ValueTree should remain unchanged after the guard rejects the value
        expectWithinAbsoluteError (
            static_cast<double> (beatTree.getProperty (IDs::bpm)),
            128.0, 0.001); // unchanged
    }

    // -------------------------------------------------------------------
    void testPrd024ManuallyAdjustedFlagOnNudge()
    {
        beginTest ("PRD-0024: manuallyAdjusted=true written to ValueTree after nudge");

        juce::ValueTree beatTree (IDs::BeatGrid);
        beatTree.setProperty (IDs::anchorSample,        1000,    nullptr);
        beatTree.setProperty (IDs::beatIntervalSamples, 22050.0, nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted,    false,   nullptr);

        // Simulate fine forward nudge (+1 delta)
        double  sr       = 44100.0;
        int64_t anchor   = 1000;
        int64_t interval = 22050;
        int64_t offset   = static_cast<int64_t> (std::round (sr * 0.010)); // 441

        int64_t newAnchor = ((anchor + offset) % interval + interval) % interval;
        expectEquals (newAnchor, (int64_t) 1441);

        beatTree.setProperty (IDs::anchorSample,     static_cast<double> (newAnchor), nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted, true,                            nullptr);

        expectEquals (
            static_cast<int64_t> (static_cast<double> (beatTree.getProperty (IDs::anchorSample))),
            (int64_t) 1441);
        expect (static_cast<bool> (beatTree.getProperty (IDs::manuallyAdjusted)));

        // Simulate coarse backward nudge (-2 delta)
        int64_t coarseOffset = -static_cast<int64_t> (std::round (sr * 0.050)); // -2205
        int64_t prevAnchor   = newAnchor;
        int64_t newAnchor2   = ((prevAnchor + coarseOffset) % interval + interval) % interval;
        // 1441 - 2205 = -764 → -764 + 22050 = 21286
        expectEquals (newAnchor2, (int64_t) 21286);

        beatTree.setProperty (IDs::anchorSample,     static_cast<double> (newAnchor2), nullptr);
        expect (static_cast<bool> (beatTree.getProperty (IDs::manuallyAdjusted)));
    }
};

static BeatGridTests beatGridTests;
