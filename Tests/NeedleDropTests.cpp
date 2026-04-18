#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Waveform/WaveformData.h"
#include "Features/Waveform/OverviewWaveform.h"
#include "Features/Waveform/DetailWaveform.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Quantize/QuantizeService.h"

class NeedleDropTests : public juce::UnitTest
{
public:
    NeedleDropTests() : juce::UnitTest ("Needle Drop and Waveform Seeking", "Sonik") {}

    void runTest() override
    {
        testOverviewPixelToSampleLinear();
        testOverviewPixelToSampleAtZero();
        testOverviewPixelToSampleAtEnd();
        testOverviewPixelToSampleClampLeft();
        testOverviewPixelToSampleClampRight();
        testOverviewPixelToSampleNoData();
        testOverviewPixelToSampleQuarter();
        testOverviewPixelToSampleThreeQuarter();
        testDetailPixelToSampleCenter();
        testDetailPixelToSampleLeftEdge();
        testDetailPixelToSampleRightEdge();
        testDetailPixelToSampleClampLow();
        testDetailPixelToSampleClampHigh();
        testDetailPixelToSampleNoData();
        testDetailPixelToSampleNoAudioState();
        testDetailPixelToSampleZoomLevel();
        testOverviewInverseConsistency();
        testDetailPositionSymmetry();
        testQuantizeSnapOnBeat();
        testQuantizeSnapOffBeat();
        testQuantizeSnapDisabled();
        testQuantizeSnapNoInterval();
    }

private:
    WaveformData::Ptr makeTestData (int64_t total, double sr = 44100.0)
    {
        auto data = WaveformData::Ptr (new WaveformData());
        data->sampleRate   = sr;
        data->totalSamples = total;
        data->contentHash  = "needle_drop_test";

        int basePoints = static_cast<int> (total / WaveformData::baseSamplesPerPoint);
        if (basePoints < 1) basePoints = 1;

        data->levels.resize (WaveformData::numMipmapLevels);
        data->levels[0].resize (static_cast<size_t> (basePoints));

        for (int level = 1; level < WaveformData::numMipmapLevels; ++level)
        {
            int prevSize = static_cast<int> (data->levels[static_cast<size_t> (level - 1)].size());
            data->levels[static_cast<size_t> (level)].resize (static_cast<size_t> ((prevSize + 1) / 2));
        }

        return data;
    }

    void testOverviewPixelToSampleLinear()
    {
        beginTest ("Overview pixelToSample - 50% maps to mid-track");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        int64_t mid = overview.pixelXToSamplePosition (500.0f);
        expect (mid >= total / 2 - 1 && mid <= total / 2 + 1);
    }

    void testOverviewPixelToSampleAtZero()
    {
        beginTest ("Overview pixelToSample - 0px maps to sample 0");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        overview.setWaveformData (makeTestData (441000));
        expect (overview.pixelXToSamplePosition (0.0f) == 0);
    }

    void testOverviewPixelToSampleAtEnd()
    {
        beginTest ("Overview pixelToSample - max px maps to totalSamples-1");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        expect (overview.pixelXToSamplePosition (1000.0f) == total - 1);
    }

    void testOverviewPixelToSampleClampLeft()
    {
        beginTest ("Overview pixelToSample - negative pixel clamped to 0");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        overview.setWaveformData (makeTestData (441000));
        expect (overview.pixelXToSamplePosition (-50.0f) == 0);
    }

    void testOverviewPixelToSampleClampRight()
    {
        beginTest ("Overview pixelToSample - beyond width clamped to max");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        expect (overview.pixelXToSamplePosition (2000.0f) == total - 1);
    }

    void testOverviewPixelToSampleNoData()
    {
        beginTest ("Overview pixelToSample - no data returns 0");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        expect (overview.pixelXToSamplePosition (500.0f) == 0);
    }

    void testOverviewPixelToSampleQuarter()
    {
        beginTest ("Overview pixelToSample - 25% maps to quarter-track");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        int64_t quarter = overview.pixelXToSamplePosition (250.0f);
        int64_t expected = total / 4;
        expect (std::abs (quarter - expected) <= 1);
    }

    void testOverviewPixelToSampleThreeQuarter()
    {
        beginTest ("Overview pixelToSample - 75% maps to three-quarter-track");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        int64_t threeQ = overview.pixelXToSamplePosition (750.0f);
        int64_t expected = total * 3 / 4;
        expect (std::abs (threeQ - expected) <= 1);
    }

    void testDetailPixelToSampleCenter()
    {
        beginTest ("Detail pixelToSample - center pixel maps to playhead");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 120;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        int64_t playhead = 44100 * 60;
        audioState.playheadPosition.store (playhead, std::memory_order_relaxed);
        int64_t center = detail.pixelXToSamplePosition (500.0f);
        expect (std::abs (center - playhead) <= 1);
    }

    void testDetailPixelToSampleLeftEdge()
    {
        beginTest ("Detail pixelToSample - left edge = playhead - halfVisible");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 120;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        int64_t playhead = 44100 * 60;
        audioState.playheadPosition.store (playhead, std::memory_order_relaxed);
        int64_t leftSample = detail.pixelXToSamplePosition (0.0f);
        int64_t expected = playhead - static_cast<int64_t> (8.0 * 44100.0);
        expect (std::abs (leftSample - expected) <= 2);
    }

    void testDetailPixelToSampleRightEdge()
    {
        beginTest ("Detail pixelToSample - right edge = playhead + halfVisible");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 120;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        int64_t playhead = 44100 * 60;
        audioState.playheadPosition.store (playhead, std::memory_order_relaxed);
        int64_t rightSample = detail.pixelXToSamplePosition (1000.0f);
        int64_t expected = playhead + static_cast<int64_t> (8.0 * 44100.0);
        expect (std::abs (rightSample - expected) <= 2);
    }

    void testDetailPixelToSampleClampLow()
    {
        beginTest ("Detail pixelToSample - clamps to 0 near track start");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 10;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        audioState.playheadPosition.store (0, std::memory_order_relaxed);
        expect (detail.pixelXToSamplePosition (0.0f) == 0);
    }

    void testDetailPixelToSampleClampHigh()
    {
        beginTest ("Detail pixelToSample - clamps to totalSamples-1 near track end");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 10;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        audioState.playheadPosition.store (total - 1, std::memory_order_relaxed);
        int64_t rightSample = detail.pixelXToSamplePosition (1000.0f);
        expect (rightSample == total - 1);
    }

    void testDetailPixelToSampleNoData()
    {
        beginTest ("Detail pixelToSample - no data returns 0");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        expect (detail.pixelXToSamplePosition (500.0f) == 0);
    }

    void testDetailPixelToSampleNoAudioState()
    {
        beginTest ("Detail pixelToSample - no audio state returns 0");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        detail.setWaveformData (makeTestData (44100 * 120));
        expect (detail.pixelXToSamplePosition (500.0f) == 0);
    }

    void testDetailPixelToSampleZoomLevel()
    {
        beginTest ("Detail pixelToSample - default zoom spans ~16 seconds");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 120;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        int64_t playhead = 44100 * 60;
        audioState.playheadPosition.store (playhead, std::memory_order_relaxed);
        int64_t leftSample = detail.pixelXToSamplePosition (0.0f);
        int64_t rightSample = detail.pixelXToSamplePosition (1000.0f);
        int64_t span = rightSample - leftSample;
        double seconds = static_cast<double> (span) / 44100.0;
        expect (std::abs (seconds - 16.0) < 0.5);
    }

    void testOverviewInverseConsistency()
    {
        beginTest ("Overview pixel-to-sample roundtrip within +-1px");
        OverviewWaveform overview;
        overview.setSize (1000, 60);
        int64_t total = 441000;
        overview.setWaveformData (makeTestData (total));
        for (float px : { 0.0f, 100.0f, 250.0f, 500.0f, 750.0f, 999.0f })
        {
            int64_t sample = overview.pixelXToSamplePosition (px);
            expect (sample >= 0 && sample < total);
            float backPx = static_cast<float> (sample) / static_cast<float> (total) * 1000.0f;
            expect (std::abs (backPx - px) <= 1.5f);
        }
    }

    void testDetailPositionSymmetry()
    {
        beginTest ("Detail left and right equidistant from center");
        DetailWaveform detail;
        detail.setSize (1000, 200);
        DeckAudioState audioState;
        int64_t total = 44100 * 120;
        detail.setWaveformData (makeTestData (total));
        detail.setAudioState (&audioState);
        int64_t playhead = 44100 * 60;
        audioState.playheadPosition.store (playhead, std::memory_order_relaxed);
        int64_t leftSample  = detail.pixelXToSamplePosition (0.0f);
        int64_t rightSample = detail.pixelXToSamplePosition (1000.0f);
        int64_t center      = detail.pixelXToSamplePosition (500.0f);
        int64_t leftDist  = center - leftSample;
        int64_t rightDist = rightSample - center;
        expect (std::abs (leftDist - rightDist) <= 2);
    }

    void testQuantizeSnapOnBeat()
    {
        beginTest ("Quantize snap - position on beat unchanged");
        double interval = 44100.0 * 60.0 / 120.0;
        int64_t pos = 220500;
        int64_t snapped = QuantizeService::snapToNearestBeat (pos, 0, interval);
        expect (snapped == pos);
    }

    void testQuantizeSnapOffBeat()
    {
        beginTest ("Quantize snap - position off-beat snapped to nearest");
        double interval = 44100.0 * 60.0 / 120.0;
        int64_t pos = 95000;
        int64_t snapped = QuantizeService::snapToNearestBeat (pos, 0, interval);
        expect (snapped == 88200);
        int64_t pos2 = 105000;
        int64_t snapped2 = QuantizeService::snapToNearestBeat (pos2, 0, interval);
        expect (snapped2 == 110250);
    }

    void testQuantizeSnapDisabled()
    {
        beginTest ("Quantize snap - zero interval returns position unchanged");
        int64_t pos = 95000;
        int64_t snapped = QuantizeService::snapToNearestBeat (pos, 0, 0.0);
        expect (snapped == pos);
    }

    void testQuantizeSnapNoInterval()
    {
        beginTest ("Quantize snap - negative interval returns position unchanged");
        int64_t pos = 95000;
        int64_t snapped = QuantizeService::snapToNearestBeat (pos, 0, -100.0);
        expect (snapped == pos);
    }
};

static NeedleDropTests needleDropTests;
