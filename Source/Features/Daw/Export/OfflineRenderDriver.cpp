//==============================================================================
// PRD-0099: OfflineRenderDriver implementation. See OfflineRenderDriver.h for
// the full contract. This file owns ONLY the loop / seek-seed / synchronous
// gating / tail / progress / cancellation; it reuses TimelineRenderer::renderBlock
// and AutomationApplier::tick unchanged.
//==============================================================================

#include "OfflineRenderDriver.h"

#include "../Automation/AutomationApplier.h"
#include "../Playback/DawTransport.h"
#include "../State/DawState.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Daw
{

//==============================================================================
// OfflineRenderConfig factories
//==============================================================================

OfflineRenderConfig OfflineRenderConfig::wholeArrangement (ArrangementSnapshot snap,
                                                           double sampleRate,
                                                           int blockSz)
{
    OfflineRenderConfig c;
    c.snapshot         = std::move (snap);
    c.rangeMode        = RangeMode::WholeArrangement;
    c.renderSampleRate = sampleRate;
    c.blockSize        = blockSz;
    // Whole-arrangement default: engine-silence capped tail (PRD-0099 §1.5.3).
    c.tailPolicy       = TailPolicy::EngineSilenceCap;
    return c;
}

OfflineRenderConfig OfflineRenderConfig::region (ArrangementSnapshot snap,
                                                 int64_t startSample,
                                                 int64_t endSample,
                                                 double sampleRate,
                                                 int blockSz)
{
    OfflineRenderConfig c;
    c.snapshot         = std::move (snap);
    c.rangeMode        = RangeMode::Region;
    c.rangeStartSample = startSample;
    c.rangeEndSample   = endSample;
    c.renderSampleRate = sampleRate;
    c.blockSize        = blockSz;
    // Region default: render exactly as bounded, no tail (PRD-0099 §1.5.3).
    c.tailPolicy       = TailPolicy::None;
    return c;
}

//==============================================================================
// Construction / destruction
//==============================================================================

OfflineRenderDriver::OfflineRenderDriver (OfflineRenderConfig config,
                                          ReaderProvider     readerProvider,
                                          AutomationApplier* applier,
                                          DawTransport*      applierTransport)
    : config_           (std::move (config)),
      readerProvider_   (std::move (readerProvider)),
      applier_          (applier),
      applierTransport_ (applierTransport)
{
    jassert (config_.blockSize > 0);
    jassert (config_.renderSampleRate > 0.0);
    // If an applier is supplied it must have a transport to seek per block.
    jassert (applier_ == nullptr || applierTransport_ != nullptr);

    // Own a streamer pool sized to the snapshot's clip count (bounded). One slot
    // per distinct clip so clips that share a source never collide on the ring.
    const int clipCount = juce::jmax (1, config_.snapshot.totalClipCount());
    pool_ = std::make_unique<ClipStreamerPool> (juce::jmin (clipCount + 1,
                                                            ClipStreamerPool::kPoolSize));

    renderer_ = std::make_unique<TimelineRenderer> (publisher_, *pool_, playhead_);
    renderer_->prepare (config_.renderSampleRate,
                        config_.blockSize,
                        kMaxLanes,
                        kMaxClipsPerLane);
}

OfflineRenderDriver::~OfflineRenderDriver()
{
    // RAII: stopping every streamer joins its reader thread. The pool destructor
    // does this too, but we do it explicitly so a cancelled render frees promptly.
    if (pool_ != nullptr)
    {
        for (int i = 0; i < pool_->poolSize(); ++i)
            if (auto* s = pool_->getStreamer (i))
                s->stop();
    }
}

//==============================================================================
// Range / length helpers
//==============================================================================

int64_t OfflineRenderDriver::arrangementEndSample() const
{
    int64_t end = 0;
    const auto& snap = config_.snapshot;
    for (int lane = 0; lane < snap.laneCount; ++lane)
    {
        const auto& ln = snap.lanes[lane];
        for (int ci = 0; ci < ln.count; ++ci)
            end = juce::jmax (end, ln.events[ci].timelineEndSample);
    }
    return end;
}

void OfflineRenderDriver::resolveRange (int64_t& startOut, int64_t& endOut) const
{
    if (config_.rangeMode == OfflineRenderConfig::RangeMode::Region)
    {
        startOut = juce::jmax ((int64_t) 0, config_.rangeStartSample);
        endOut   = juce::jmax (startOut, config_.rangeEndSample);
    }
    else
    {
        startOut = 0;
        endOut   = arrangementEndSample();
    }
}

int64_t OfflineRenderDriver::rangeSamples() const
{
    int64_t s = 0, e = 0;
    resolveRange (s, e);
    return e - s;
}

int64_t OfflineRenderDriver::effectiveMaxTailSamples() const
{
    switch (config_.tailPolicy)
    {
        case OfflineRenderConfig::TailPolicy::None:
            return 0;

        case OfflineRenderConfig::TailPolicy::FixedLength:
            return juce::jmax ((int64_t) 0, config_.tailFixedSamples);

        case OfflineRenderConfig::TailPolicy::EngineSilenceCap:
        {
            const int64_t cap = (config_.tailMaxSamples > 0)
                                    ? config_.tailMaxSamples
                                    : (int64_t) std::llround (config_.renderSampleRate * 10.0);
            return cap;
        }
    }
    return 0;
}

int64_t OfflineRenderDriver::maxTotalSamples() const
{
    return rangeSamples() + effectiveMaxTailSamples();
}

//==============================================================================
// Seek-seed: prime each clip's streamer at the source sample it would be reading
// at the range start, so a region render reproduces in-flight read state and the
// region's first block carries the correct source position (PRD-0099 §1.5.2).
//==============================================================================

void OfflineRenderDriver::primeStreamersForRange (int64_t rangeStart,
                                                  std::vector<uint64_t>& silentOut)
{
    // Work on a mutable copy of the snapshot; we rewrite per-clip read handles.
    working_ = config_.snapshot;

    const double projectRate = config_.renderSampleRate;

    for (int lane = 0; lane < working_.laneCount; ++lane)
    {
        auto& ln = working_.lanes[lane];
        for (int ci = 0; ci < ln.count; ++ci)
        {
            ClipEvent& ev = ln.events[ci];

            // A clip entirely before the range start never plays in-range and is
            // not seeded (it would have a read offset past its end). We still
            // give it a handle so a tail/region edge that re-touches it behaves,
            // but seed the read at the range-start-relative source position.
            // Assign a stable slot keyed by the clip's lane+index (unique).
            const juce::String slotKey =
                juce::String (lane) + ":" + juce::String (ci) + ":" + juce::String ((juce::int64) ev.sourceFileId);
            const int32_t slot = pool_->resolveHandle (slotKey);
            if (slot < 0)
            {
                ev.sourceReadHandle = -1; // pool full: render this clip silent
                silentOut.push_back (ev.sourceFileId);
                continue;
            }

            // Resolve a reader for this clip. nullptr => unresolved source =>
            // render its span as silence and record it (PRD-0099 §1.5.8). We do
            // NOT abort.
            std::unique_ptr<juce::AudioFormatReader> reader =
                readerProvider_ ? readerProvider_ (ev) : nullptr;

            if (reader == nullptr)
            {
                ev.sourceReadHandle = -1; // renderBlock skips it -> silent span
                silentOut.push_back (ev.sourceFileId);
                continue;
            }

            const double sourceRate = (reader->sampleRate > 0.0) ? reader->sampleRate
                                                                 : projectRate;

            // The source sample this clip would be reading at rangeStart: advance
            // into the crop by (rangeStart - timelineStart) when the range starts
            // mid-clip; otherwise start at the crop start (clamped to crop end).
            const int64_t offsetIntoClip =
                juce::jmax ((int64_t) 0, rangeStart - ev.timelineStartSample);
            int64_t primeStartProject = ev.sourceStartSample + offsetIntoClip;
            primeStartProject = juce::jmin (primeStartProject, ev.sourceEndSample);

            // Convert project-rate source positions to the reader's native rate
            // (the streamer resamples source->render rate internally), mirroring
            // DawPlaybackController::makeResolver.
            const double  srcPerProject = sourceRate / projectRate;
            const int64_t readerStart =
                (int64_t) std::llround ((double) primeStartProject * srcPerProject);
            const int64_t readerEnd =
                (int64_t) std::llround ((double) ev.sourceEndSample * srcPerProject);

            ClipStreamer* streamer = pool_->getStreamer (slot);
            jassert (streamer != nullptr);

            // Render-scoped synchronous full-read: each block will block until
            // the required samples are decoded (PRD-0099 §1.5.1).
            streamer->setSynchronousMode (true);
            streamer->prime (std::move (reader), projectRate, readerStart, readerEnd);

            ev.sourceReadHandle = slot;
        }
    }

    // Publish the seeded snapshot for renderBlock to consume.
    publisher_.publish (working_);
}

//==============================================================================
// Synchronous gating: for every clip active in [playhead, playhead+numSamples),
// block until its streamer holds the samples renderBlock will pull this block.
// This is pure loop bookkeeping mirroring renderBlock's overlap math — it adds
// NO render logic; it only decides how many samples to wait for per clip.
//==============================================================================

namespace
{
    void waitForActiveClips (const ArrangementSnapshot& snap,
                             ClipStreamerPool& pool,
                             int64_t playhead,
                             int numSamples)
    {
        for (int lane = 0; lane < snap.laneCount; ++lane)
        {
            const LaneSnapshot& ln = snap.lanes[lane];
            for (int ci = 0; ci < ln.count; ++ci)
            {
                const ClipEvent& ev = ln.events[ci];

                if (ev.timelineEndSample <= playhead)
                    continue;
                if (ev.timelineStartSample >= playhead + numSamples)
                    break; // lane is sorted by timelineStartSample
                if (ev.sourceReadHandle < 0)
                    continue;

                const int blockStart = (int) juce::jmax ((int64_t) 0,
                                                         ev.timelineStartSample - playhead);
                const int blockEnd = (int) juce::jmin ((int64_t) numSamples,
                                                       ev.timelineEndSample - playhead);
                const int copyLen = blockEnd - blockStart;
                if (copyLen <= 0)
                    continue;

                if (auto* s = pool.getStreamer (ev.sourceReadHandle))
                    if (s->isSynchronousMode())
                        s->waitUntilReady (copyLen);
            }
        }
    }
}

//==============================================================================
// Public render entry points
//==============================================================================

RenderResult OfflineRenderDriver::render (juce::AudioBuffer<float>& destBuffer,
                                          std::atomic<float>*       progress,
                                          std::atomic<bool>*        cancelToken)
{
    const int64_t total = maxTotalSamples();
    destBuffer.setSize (2, (int) juce::jmax ((int64_t) 0, total), false, true, true);
    destBuffer.clear();

    int64_t written = 0;

    BlockSink sink =
        [&destBuffer, &written] (const juce::AudioBuffer<float>& block,
                                 int numSamples,
                                 int64_t /*blockStartSample*/)
        {
            const int dest = (int) written;
            const int n = juce::jmin (numSamples, destBuffer.getNumSamples() - dest);
            if (n > 0)
                for (int ch = 0; ch < 2; ++ch)
                    destBuffer.copyFrom (ch, dest, block, ch, 0, n);
            written += numSamples;
        };

    ProgressReporter prog;
    if (progress != nullptr)
        prog = [progress] (float f) { progress->store (f, std::memory_order_relaxed); };

    RenderResult result = runLoop (sink, prog, cancelToken);

    // Trim the buffer to the exact rendered length (silence-capped tail may end
    // before maxTotalSamples).
    if (result.samplesRendered < destBuffer.getNumSamples())
        destBuffer.setSize (2, (int) juce::jmax ((int64_t) 0, result.samplesRendered),
                            true, true, true);

    return result;
}

RenderResult OfflineRenderDriver::render (const BlockSink&        sink,
                                          const ProgressReporter& progress,
                                          std::atomic<bool>*      cancelToken)
{
    return runLoop (sink, progress, cancelToken);
}

//==============================================================================
// The deterministic block loop.
//==============================================================================

RenderResult OfflineRenderDriver::runLoop (const BlockSink&        sink,
                                           const ProgressReporter& progress,
                                           std::atomic<bool>*      cancelToken)
{
    RenderResult result;

    int64_t rangeStart = 0, rangeEnd = 0;
    resolveRange (rangeStart, rangeEnd);

    // ── Seed: prime every clip's read position to the range-start in-flight
    //    state (PRD-0099 §1.5.2) and publish the seeded snapshot.
    primeStreamersForRange (rangeStart, result.silentClipSourceIds);

    // Put the renderer playhead at the range start so renderBlock's overlap math
    // and the streamers' linear reads stay aligned for the whole loop.
    playhead_.store (rangeStart, std::memory_order_release);

    // Bring the applier's transport to a Playing state seeded at the range start
    // so tick() evaluates lanes at the correct sample (PRD-0099 §1.5.7).
    const bool haveApplier = (applier_ != nullptr && applierTransport_ != nullptr);
    if (haveApplier)
    {
        applierTransport_->play();          // Stopped -> Playing (resets to 0)
        applierTransport_->seek (rangeStart);
    }

    const int    blockSize    = config_.blockSize;
    const int64_t inRange     = rangeEnd - rangeStart;
    const int64_t maxTail     = effectiveMaxTailSamples();
    const int64_t hardEnd     = rangeEnd + maxTail; // upper bound incl. tail

    // Pre-allocated stereo block buffer reused every iteration.
    juce::AudioBuffer<float> blockBuf (2, blockSize);

    int64_t pos          = rangeStart;       // absolute timeline sample
    int64_t produced     = 0;                // samples written to the sink
    int     silentTailBlocks = 0;            // consecutive near-silent tail blocks
    float   lastPublished    = -1.0f;
    auto    lastPublishTime  = std::chrono::steady_clock::now();

    while (pos < hardEnd)
    {
        // Cancel is checked EVERY block (cheap atomic load), between blocks so a
        // block is either fully written or not started (PRD-0099 §1.5.6).
        if (cancelToken != nullptr && cancelToken->load (std::memory_order_relaxed))
        {
            result.status          = RenderResult::Status::Cancelled;
            result.samplesRendered = produced;
            if (haveApplier) applierTransport_->stop();
            return result;
        }

        const bool inTail = (pos >= rangeEnd);

        // Final in-range block may be shorter; tail blocks are full blockSize but
        // never exceed the hard end.
        int numSamples = blockSize;
        if (! inTail)
            numSamples = (int) juce::jmin ((int64_t) blockSize, rangeEnd - pos);
        else
            numSamples = (int) juce::jmin ((int64_t) blockSize, hardEnd - pos);

        if (numSamples <= 0)
            break;

        // ── Per-block evaluation (mirrors the live per-block sequence) ────────
        // 1. Set the playhead for this block (renderBlock + applier read it).
        playhead_.store (pos, std::memory_order_release);

        // 2. Evaluate master tempo + automation at the block-start playhead via
        //    the SAME applier the live path uses. tick() walks the master-tempo
        //    lane (PRD-0089) into the tempo sink and every continuous/boolean
        //    lane into its sink — identical to live (PRD-0099 §1.5.7).
        if (haveApplier)
        {
            applierTransport_->seek (pos);
            applier_->tick();
        }

        // 3. Synchronous full-read: ensure each active clip's streamer holds the
        //    samples renderBlock will pull this block (PRD-0099 §1.5.1). For tail
        //    blocks there are no in-range onsets, but a clip may still be decaying
        //    (its timelineEnd > pos); gating handles that uniformly.
        waitForActiveClips (working_, *pool_, pos, numSamples);

        // 4. Sum the block via the UNCHANGED engine inner loop.
        blockBuf.clear();
        juce::AudioBuffer<float> view (blockBuf.getArrayOfWritePointers(), 2, numSamples);
        renderer_->renderBlock (view, numSamples);

        // 5. Emit the block to the sink.
        sink (view, numSamples, pos);
        produced += numSamples;
        pos      += numSamples;

        // ── Tail termination for the silence-capped policy ───────────────────
        if (inTail
            && config_.tailPolicy == OfflineRenderConfig::TailPolicy::EngineSilenceCap)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                peak = juce::jmax (peak, view.getMagnitude (ch, 0, numSamples));

            if (peak <= config_.tailSilenceThreshold)
            {
                if (++silentTailBlocks >= config_.tailSilenceBlocks)
                    break; // sustained silence: stop the tail
            }
            else
            {
                silentTailBlocks = 0;
            }
        }

        // ── Progress: computed every block, published coarsely ───────────────
        if (progress)
        {
            const int64_t denom = juce::jmax ((int64_t) 1, inRange + maxTail);
            const float frac = juce::jlimit (0.0f, 1.0f, (float) ((double) produced / (double) denom));
            const auto now = std::chrono::steady_clock::now();
            const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds> (now - lastPublishTime).count();
            if (frac - lastPublished >= 0.01f || sinceMs >= 50)
            {
                progress (frac);
                lastPublished   = frac;
                lastPublishTime = now;
            }
        }
    }

    if (haveApplier)
        applierTransport_->stop();

    result.status          = RenderResult::Status::Completed;
    result.samplesRendered = produced;

    if (progress)
        progress (1.0f); // reaches exactly 1.0 on Completed (PRD-0099 §1.5.6)

    return result;
}

} // namespace Daw
