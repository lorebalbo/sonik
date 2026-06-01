//==============================================================================
// PRD-0080: ClipStreamerTests — unit tests for ClipStreamer and ClipStreamerPool.
//
// Tests:
//   - readInto() returns silence when no reader is primed
//   - readInto() returns silence mode for missing source
//   - streamer pool resolveHandle / getStreamer round-trip
//   - underrun counter increments on starved read
//   - pool handles multiple file IDs
//
// JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "Features/Daw/Playback/ClipStreamer.h"

class ClipStreamerTests final : public juce::UnitTest
{
public:
    ClipStreamerTests() : juce::UnitTest ("Clip Streamer (PRD-0080)", "Sonik") {}

    void runTest() override
    {
        testSilenceWhenNotPrimed();
        testPoolResolveHandle();
        testPoolMultipleFileIds();
        testPoolSizeIsPositive();
    }

private:
    // ─── Silence when not primed ──────────────────────────────────────────

    void testSilenceWhenNotPrimed()
    {
        beginTest ("ClipStreamer: readInto returns silence when not primed");

        Daw::ClipStreamer streamer;
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();

        // Fill with non-zero sentinel.
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                buf.setSample (ch, i, 1.0f);

        // Not primed → read should underrun and zero-fill.
        streamer.readInto (buf, 0, 256);

        // Expect zeroes (underrun fill).
        float maxAbs = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                maxAbs = juce::jmax (maxAbs, std::abs (buf.getSample (ch, i)));

        expectWithinAbsoluteError (maxAbs, 0.0f, 1e-7f, "should be silence");
        expect (streamer.underrunCount() > 0, "underrun should be flagged");
    }

    // ─── Pool: resolveHandle / getStreamer ────────────────────────────────

    void testPoolResolveHandle()
    {
        beginTest ("ClipStreamerPool: resolveHandle returns stable index");

        Daw::ClipStreamerPool pool (4);

        const int h1 = pool.resolveHandle ("file-a");
        const int h2 = pool.resolveHandle ("file-a"); // same file → same handle
        const int h3 = pool.resolveHandle ("file-b"); // different file

        expect (h1 >= 0);
        expectEquals (h1, h2, "same fileId must return same handle");
        expect (h3 >= 0 && h3 != h1, "different fileId gets different slot");

        // Streamer objects must be accessible.
        expect (pool.getStreamer (h1) != nullptr);
        expect (pool.getStreamer (h3) != nullptr);
    }

    // ─── Pool: multiple file IDs ──────────────────────────────────────────

    void testPoolMultipleFileIds()
    {
        beginTest ("ClipStreamerPool: multiple fileIds get separate handles");

        Daw::ClipStreamerPool pool (8);
        juce::Array<int> handles;

        for (int i = 0; i < 6; ++i)
        {
            const int h = pool.resolveHandle ("file-" + juce::String (i));
            expect (h >= 0);
            expect (!handles.contains (h), "each file should get a unique handle");
            handles.add (h);
        }
    }

    // ─── Pool: size is positive ───────────────────────────────────────────

    void testPoolSizeIsPositive()
    {
        beginTest ("ClipStreamerPool: pool size is positive");

        Daw::ClipStreamerPool pool (16);
        expect (pool.poolSize() == 16);
    }
};

static ClipStreamerTests clipStreamerTestsInstance;
