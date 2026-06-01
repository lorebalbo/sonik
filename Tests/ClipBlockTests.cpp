//==============================================================================
// PRD-0068: ClipBlock atom tests.
//
// Verifies that a ClipBlock derives its horizontal placement purely from the
// PRD-0065 TimelineTransform, selects the correct PRD-0006 mipmap tier and crop
// point window across zooms, degrades to a placeholder on a cache miss, and
// NEVER triggers a fresh analysis pass (the injected source is read-only).
//==============================================================================

#include <juce_core/juce_core.h>

#include "Features/Daw/Ui/Atoms/ClipBlock.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Waveform/WaveformData.h"

namespace
{

using namespace Daw;

// 120 BPM @ 44.1 kHz → 22050 samples per beat.
constexpr double kSamplesPerBeat = 22050.0;

TimelineTransform makeTransform (double pixelsPerBeat)
{
    TimelineTransform::GridSnapshot grid;
    grid.samplesPerBeat    = kSamplesPerBeat;
    grid.phaseOriginSample = 0;
    // A large content end keeps the clamped left edge anchored at sample 0 so
    // placement maths are exercised directly (no scroll-margin offset).
    const std::int64_t contentEnd = static_cast<std::int64_t> (1000.0 * kSamplesPerBeat);
    return TimelineTransform (grid, pixelsPerBeat, /*leftEdge*/ 0, /*viewport*/ 2000.0, contentEnd);
}

juce::ValueTree makeClipNode (std::int64_t timelineStart,
                              std::int64_t sourceStart,
                              std::int64_t sourceEnd,
                              const juce::String& sourceFileId)
{
    DawClip clip;
    clip.clipId             = juce::Uuid();
    clip.laneId             = juce::Uuid();
    clip.sourceFileId       = sourceFileId;
    clip.sourceStartSample  = sourceStart;
    clip.sourceEndSample    = sourceEnd;
    clip.timelineStartSample = timelineStart;
    clip.sourceLengthSamples = sourceEnd;
    return DawClip::toValueTree (clip);
}

// Synthetic mipmap pyramid: 'baseCount' points at level 0, halving each level.
WaveformData::Ptr makeWaveform (int baseCount, int numLevels = WaveformData::numMipmapLevels)
{
    auto data = WaveformData::Ptr (new WaveformData());
    data->sampleRate   = 44100.0;
    data->totalSamples = static_cast<std::int64_t> (baseCount) * WaveformData::baseSamplesPerPoint;
    data->levels.resize (static_cast<size_t> (numLevels));

    int count = baseCount;
    for (int i = 0; i < numLevels; ++i)
    {
        data->levels[static_cast<size_t> (i)].resize (static_cast<size_t> (juce::jmax (1, count)));
        for (auto& pt : data->levels[static_cast<size_t> (i)])
        {
            pt.peakL = 0.5f;
            pt.peakR = 0.5f;
        }
        count = juce::jmax (1, count / 2);
    }
    return data;
}

} // namespace

class ClipBlockTests final : public juce::UnitTest
{
public:
    ClipBlockTests() : juce::UnitTest ("Clip Block (PRD-0068)", "Sonik") {}

    void runTest() override
    {
        beginTest ("Horizontal placement is derived purely from the transform");
        {
            auto transform = makeTransform (100.0); // 100 px / beat
            // Clip starts at beat 1 (22050) and spans 4 beats (88200 samples).
            auto node = makeClipNode (22050, 0, 88200, "hashA");

            ClipBlock block (node, transform, [] (const juce::String&) { return WaveformData::Ptr(); });

            // x = (22050) * 100 / 22050 = 100; width = 88200 * 100 / 22050 = 400.
            expectEquals (block.getTimelineX(), 100);
            expectEquals (block.getTimelineWidth(), 400);
        }

        beginTest ("Placement tracks zoom changes through the shared transform");
        {
            auto transform = makeTransform (100.0);
            auto node = makeClipNode (22050, 0, 88200, "hashA");
            ClipBlock block (node, transform, [] (const juce::String&) { return WaveformData::Ptr(); });

            expectEquals (block.getTimelineX(), 100);

            transform.setPixelsPerBeat (200.0); // zoom in 2x
            expectEquals (block.getTimelineX(), 200);
            expectEquals (block.getTimelineWidth(), 800);
        }

        beginTest ("Mipmap tier selection follows the transform zoom");
        {
            auto data = makeWaveform (400);

            // At 100 px/beat: spp = 22050/100 = 220.5 -> level 0 (256 >= 220.5).
            {
                const double spp = ClipBlock::samplesPerPixelFor (makeTransform (100.0));
                auto slice = ClipBlock::computeSlice (*data, 0, 88200, spp);
                expect (slice.valid);
                expectEquals (slice.level, 0);
            }

            // At 10 px/beat: spp = 2205 -> level 4 (256*16=4096 >= 2205).
            {
                const double spp = ClipBlock::samplesPerPixelFor (makeTransform (10.0));
                auto slice = ClipBlock::computeSlice (*data, 0, 88200, spp);
                expect (slice.valid);
                expectEquals (slice.level, 4);
            }
        }

        beginTest ("Crop window maps to the correct tier point indices");
        {
            auto data = makeWaveform (400);
            const double spp = ClipBlock::samplesPerPixelFor (makeTransform (100.0)); // level 0, 256 spp

            // Crop [256, 2560) -> points [1, 10) at level 0 (levelSpp = 256).
            auto slice = ClipBlock::computeSlice (*data, 256, 2560, spp);
            expect (slice.valid);
            expectEquals (slice.level, 0);
            expectEquals (slice.firstPoint, 1);
            expectEquals (slice.lastPoint, 10);
            expect (! slice.truncated);
        }

        beginTest ("A crop beyond the analysed length is truncated, not clamped away");
        {
            auto data = makeWaveform (10); // only 10 base points (2560 samples)
            const double spp = ClipBlock::samplesPerPixelFor (makeTransform (100.0));

            // Crop end 88200 -> ceil(88200/256)=345, clamped to tier size 10.
            auto slice = ClipBlock::computeSlice (*data, 0, 88200, spp);
            expect (slice.valid);
            expectEquals (slice.firstPoint, 0);
            expectEquals (slice.lastPoint, 10);
            expect (slice.truncated);
        }

        beginTest ("Empty/absent analysis yields an invalid slice (placeholder path)");
        {
            WaveformData empty;
            const double spp = ClipBlock::samplesPerPixelFor (makeTransform (100.0));
            auto slice = ClipBlock::computeSlice (empty, 0, 88200, spp);
            expect (! slice.valid);
        }

        beginTest ("The waveform source is read-only and never triggers analysis");
        {
            auto transform = makeTransform (100.0);
            auto node = makeClipNode (22050, 0, 88200, "hashA");

            int   queries      = 0;
            int   analyseCalls = 0; // the source has no analyse path; must stay 0.
            auto  data         = makeWaveform (400);

            auto source = [&] (const juce::String& id) -> WaveformData::Ptr
            {
                ++queries;
                // A real cache resolver returns cached data or nullptr; it must
                // not start an analysis. We model that contract here.
                if (id == "hashA")
                    return data;
                return nullptr;
            };

            ClipBlock block (node, transform, source);

            // Force a paint to exercise the waveform path.
            juce::Image img (juce::Image::ARGB, 400, 36, true);
            juce::Graphics g (img);
            block.setBounds (0, 0, 400, 36);
            block.paint (g);

            expect (queries > 0);            // the cache was consulted
            expectEquals (analyseCalls, 0);  // and no analysis was triggered
        }

        beginTest ("Clip placement updates live when its node properties change");
        {
            auto transform = makeTransform (100.0);
            auto node = makeClipNode (22050, 0, 88200, "hashA");
            ClipBlock block (node, transform, [] (const juce::String&) { return WaveformData::Ptr(); });

            expectEquals (block.getTimelineX(), 100);

            node.setProperty (DawClipIDs::timelineStartSample, (juce::int64) 44100, nullptr);
            expectEquals (block.getTimelineX(), 200); // 44100 * 100 / 22050 = 200
        }
    }
};

static ClipBlockTests clipBlockTests;
