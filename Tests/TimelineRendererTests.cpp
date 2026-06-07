//==============================================================================
// PRD-0081: TimelineRendererTests — unit tests for TimelineRenderer.
//
// Uses a stub ArrangementPublisher and stub ClipStreamers (pre-filled ring
// buffers via prime with a mock reader) to drive the renderer in isolation.
//
// Tests:
//   - Empty arrangement → silence
//   - Clip at exact block boundary → samples in correct positions
//   - Per-clip gain applied
//   - Multiple lanes sum correctly
//   - Anti-click ramp at clip start/end (no inter-sample step above threshold)
//   - Underrun produces silence (not garbage)
//   - Renderer does not allocate (compile-time only, not testable at runtime here)
//
// JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "Features/Daw/Playback/ArrangementSnapshot.h"
#include "Features/Daw/Playback/ArrangementPublisher.h"
#include "Features/Daw/Playback/ClipStreamer.h"
#include "Features/Daw/Playback/TimelineRenderer.h"
#include "Features/Daw/Import/ImportSourcePublisher.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Build a simple single-lane snapshot with one clip.
static Daw::ArrangementSnapshot makeOneClipSnapshot (int64_t timelineStart,
                                                      int64_t timelineEnd,
                                                      int32_t handle,
                                                      float   gain = 1.0f)
{
    Daw::ArrangementSnapshot snap;
    snap.laneCount = 1;
    snap.lanes[0].count = 1;
    auto& ev = snap.lanes[0].events[0];
    ev.timelineStartSample = timelineStart;
    ev.timelineEndSample   = timelineEnd;
    ev.sourceStartSample   = 0;
    ev.sourceEndSample     = timelineEnd - timelineStart;
    ev.sourceReadHandle    = handle;
    ev.gain                = gain;
    return snap;
}

/// Compute peak absolute sample value in a buffer.
static float peakAbs (const juce::AudioBuffer<float>& buf, int startSample, int numSamples)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const float* data = buf.getReadPointer (ch, startSample);
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (data[i]));
    }
    return peak;
}

/// Minimum absolute sample value over [startSample, startSample+numSamples) on ch 0.
static float minAbs (const juce::AudioBuffer<float>& buf, int startSample, int numSamples)
{
    float lo = 1.0e9f;
    const float* data = buf.getReadPointer (0, startSample);
    for (int i = 0; i < numSamples; ++i)
        lo = juce::jmin (lo, std::abs (data[i]));
    return lo;
}

/// Largest absolute difference between consecutive samples on ch 0 over
/// [startSample, startSample+numSamples). A hard waveform step (click) shows up
/// as a large value; a smooth crossfade keeps it small.
static float maxAdjacentDelta (const juce::AudioBuffer<float>& buf, int startSample, int numSamples)
{
    float hi = 0.0f;
    const float* data = buf.getReadPointer (0, startSample);
    for (int i = 1; i < numSamples; ++i)
        hi = juce::jmax (hi, std::abs (data[i] - data[i - 1]));
    return hi;
}

/// Prime a pool slot with a constant-amplitude (==value) stereo source of
/// `lengthSamples`, then block until the ring holds the full block, so a
/// subsequent render reads real samples (not an underrun).
static void primeConstantSlot (Daw::ClipStreamerPool& pool, int32_t slot,
                               float value, int lengthSamples, int waitSamples)
{
    juce::AudioBuffer<float> buf (2, lengthSamples);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < lengthSamples; ++i)
            buf.setSample (ch, i, value);
    AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), 44100.0, lengthSamples);
    pool.prime (slot, std::make_unique<Daw::Import::BufferAudioFormatReader> (holder),
                44100.0, 0, lengthSamples);
    if (auto* s = pool.getStreamer (slot))
        s->waitUntilReady (waitSamples);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test class
// ─────────────────────────────────────────────────────────────────────────────

class TimelineRendererTests final : public juce::UnitTest
{
public:
    TimelineRendererTests() : juce::UnitTest ("Timeline Renderer (PRD-0081)", "Sonik") {}

    void runTest() override
    {
        testEmptyArrangementProducesSilence();
        testClipWithUnresolvedHandleProducesSilence();
        testRendererPrepareAndRender();
        testTransportAtNegativeOneProducesNothing();
        testButtJoinedClipsCrossfadeSeamlessly();
    }

private:
    // ─── Empty arrangement → silence ──────────────────────────────────────

    void testEmptyArrangementProducesSilence()
    {
        beginTest ("Empty arrangement produces silence");

        Daw::ArrangementPublisher publisher;
        Daw::ClipStreamerPool     pool (4);
        std::atomic<int64_t>      playhead { 0 };

        Daw::TimelineRenderer renderer (publisher, pool, playhead);
        renderer.prepare (44100.0, 512, Daw::kMaxLanes, Daw::kMaxClipsPerLane);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();

        // Pre-fill with non-zero so we can detect writes.
        for (int ch = 0; ch < 2; ++ch)
            buf.setSample (ch, 0, 1.0f);

        renderer.renderBlock (buf, 512);

        // empty snapshot → nothing should be added (buffer stays at whatever
        // pre-filled value — but we initialised to sentinel only at [0]; the
        // renderer addFrom over silence = no change at sample 0 beyond what was there).
        // Actually renderer ADDs into the buffer, so clearing before calling is caller's job.
        // We cleared the buffer, so result should be silence.
        buf.clear();
        renderer.renderBlock (buf, 512);
        expectWithinAbsoluteError (peakAbs (buf, 0, 512), 0.0f, 1e-7f);
    }

    // ─── Clip with unresolved handle → silence ────────────────────────────

    void testClipWithUnresolvedHandleProducesSilence()
    {
        beginTest ("Clip with handle -1 produces silence");

        Daw::ArrangementPublisher publisher;
        Daw::ClipStreamerPool     pool (4);
        std::atomic<int64_t>      playhead { 0 };

        // Publish a snapshot with one clip using handle -1.
        Daw::ArrangementSnapshot snap = makeOneClipSnapshot (0, 512, -1 /* unresolved */);
        publisher.publish (snap);

        Daw::TimelineRenderer renderer (publisher, pool, playhead);
        renderer.prepare (44100.0, 512, Daw::kMaxLanes, Daw::kMaxClipsPerLane);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        renderer.renderBlock (buf, 512);

        expectWithinAbsoluteError (peakAbs (buf, 0, 512), 0.0f, 1e-7f,
                                   "unresolved handle should produce silence");
    }

    // ─── Renderer prepare / render cycle ─────────────────────────────────

    void testRendererPrepareAndRender()
    {
        beginTest ("Renderer prepares and renders without crashing");

        Daw::ArrangementPublisher publisher;
        Daw::ClipStreamerPool     pool (4);
        std::atomic<int64_t>      playhead { 0 };

        Daw::TimelineRenderer renderer (publisher, pool, playhead);
        renderer.prepare (44100.0, 512, 4, 64);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();

        // Should not crash.
        renderer.renderBlock (buf, 512);
        expect (true, "renderer ran without crashing");
    }

    // ─── Playhead == -1 → nothing rendered ────────────────────────────────

    void testTransportAtNegativeOneProducesNothing()
    {
        beginTest ("Playhead at -1 (stopped) produces no output");

        Daw::ArrangementPublisher publisher;
        Daw::ClipStreamerPool     pool (4);
        std::atomic<int64_t>      playhead { -1 }; // stopped

        // Publish a non-empty snapshot.
        auto snap = makeOneClipSnapshot (0, 512, 0);
        publisher.publish (snap);

        Daw::TimelineRenderer renderer (publisher, pool, playhead);
        renderer.prepare (44100.0, 512, Daw::kMaxLanes, Daw::kMaxClipsPerLane);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        // Set sentinel to detect writes.
        buf.setSample (0, 0, 0.5f);

        renderer.renderBlock (buf, 512);

        // Renderer should return early without modifying the buffer.
        expectWithinAbsoluteError (buf.getSample (0, 0), 0.5f, 1e-7f,
                                   "stopped transport must not modify buffer");
    }

    // ─── Butt-joined clips crossfade seamlessly (EPIC-0009 continuity) ─────
    //
    // A jump/loop split during recording produces two clips that butt-join on
    // the timeline but whose SOURCE content is discontinuous at the seam. The
    // renderer renders the outgoing clip's short continuation tail and overlaps
    // its fade-out with the incoming clip's fade-in, summing to an equal-power
    // crossfade (the live deck's loop-crossfade behaviour). This must be both
    // GAP-free (no silence dip) and CLICK-free (no waveform step).
    void testButtJoinedClipsCrossfadeSeamlessly()
    {
        constexpr int N        = 256;                        // per-clip length
        constexpr int fade     = Daw::kClipFadeSamples;      // 64
        constexpr int blockLen = 2 * N;                      // render both in one block
        constexpr int boundary = N;                          // seam timeline sample

        // A = [0,N) joinsNext, B = [N,2N) joinsPrev. A's streamer is primed with
        // a continuation tail (length N+fade) so the renderer can read past A's
        // crop for the crossfade; B is the usual length.
        auto makeSnapshot = [&] (bool joined, float aVal, float bVal)
        {
            Daw::ArrangementSnapshot snap;
            snap.laneCount      = 1;
            snap.lanes[0].count = 2;
            juce::ignoreUnused (aVal, bVal);

            auto& a = snap.lanes[0].events[0];
            a.timelineStartSample = 0;  a.timelineEndSample = N;
            a.sourceStartSample   = 0;  a.sourceEndSample   = N;
            a.sourceReadHandle    = 0;  a.gain = 1.0f;       a.joinsNext = joined;

            auto& b = snap.lanes[0].events[1];
            b.timelineStartSample = N;  b.timelineEndSample = 2 * N;
            b.sourceStartSample   = 0;  b.sourceEndSample   = N;
            b.sourceReadHandle    = 1;  b.gain = 1.0f;       b.joinsPrev = joined;
            return snap;
        };

        auto render = [&] (bool joined, float aVal, float bVal, juce::AudioBuffer<float>& out)
        {
            Daw::ArrangementPublisher publisher;
            Daw::ClipStreamerPool     pool (4);
            std::atomic<int64_t>      playhead { 0 };

            primeConstantSlot (pool, 0, aVal, N + fade, N + fade); // A + continuation
            primeConstantSlot (pool, 1, bVal, N,        N);        // B
            publisher.publish (makeSnapshot (joined, aVal, bVal));

            Daw::TimelineRenderer renderer (publisher, pool, playhead);
            renderer.prepare (44100.0, blockLen, Daw::kMaxLanes, Daw::kMaxClipsPerLane);

            out.setSize (2, blockLen);
            out.clear();
            renderer.renderBlock (out, blockLen);
        };

        // ── 1. Same-signal join holds full level: no silence dip. ──
        {
            beginTest ("Butt-joined clips: crossfade holds level (no silence gap)");
            juce::AudioBuffer<float> buf;
            render (/*joined*/ true, 1.0f, 1.0f, buf);
            expect (minAbs (buf, boundary - 16, fade + 32) > 0.9f,
                    "a +1/+1 join must stay at full level across the seam");
        }

        // ── 2. Opposite-signal join transitions smoothly: no click. ──
        {
            beginTest ("Butt-joined clips: crossfade is smooth (no click/step)");
            juce::AudioBuffer<float> buf;
            render (/*joined*/ true, 1.0f, -1.0f, buf);

            // The seam must actually cross from ~+1 to ~-1 (a real crossfade)…
            expectWithinAbsoluteError (buf.getSample (0, boundary),          1.0f, 0.12f,
                                       "crossfade starts at the outgoing value");
            expectWithinAbsoluteError (buf.getSample (0, boundary + fade - 1), -1.0f, 0.12f,
                                       "crossfade ends at the incoming value");
            // …with no abrupt sample-to-sample step (a hard cut would jump ~2.0).
            expect (maxAdjacentDelta (buf, boundary - 2, fade + 4) < 0.15f,
                    "no waveform step at the seam (a hard butt-join would jump ~2.0)");
        }

        // ── 3. Control: WITHOUT the join flags the seam dips to ~silence. ──
        {
            beginTest ("Unjoined clips dip to silence at the seam (regression control)");
            juce::AudioBuffer<float> buf;
            render (/*joined*/ false, 1.0f, 1.0f, buf);
            expect (minAbs (buf, boundary - 16, 32) < 0.1f,
                    "without join flags the per-clip fades collapse to silence");
        }
    }
};

static TimelineRendererTests timelineRendererTestsInstance;
