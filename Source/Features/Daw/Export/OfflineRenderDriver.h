#pragma once
//==============================================================================
// PRD-0099: OfflineRenderDriver — the deterministic, non-real-time block loop
// that drives the EXISTING timeline render engine (PRD-0081 TimelineRenderer)
// and automation applier (PRD-0092 AutomationApplier) across an arrangement (or
// a region) on a BACKGROUND thread, capturing the summed float output into a
// buffer or a per-block sink for the exporter (PRD-0100).
//
// WHAT THIS OWNS (and ONLY this):  the loop, the seek/seed to the range start,
// synchronous full-read gating of the clip streamers, the tail, progress, and
// cancellation. It RE-USES, unchanged:
//   - TimelineRenderer::renderBlock  for clip resolution + gain + ramps + summing
//   - AutomationApplier::tick        for continuous / boolean / master-tempo eval
//   - ClipStreamer (PRD-0080)        for source reads (placed in synchronous mode)
// It re-implements NONE of that. It adds no second render or automation path.
//
// THREADING (CLAUDE.md): render() runs on a caller-supplied BACKGROUND thread.
// It NEVER touches the audio thread, never takes an audio-device callback, and
// never blocks the JUCE message thread. Because it touches no real-time thread,
// it is free to allocate / lock / wait (it does all three) — but the inner
// summing loop it calls (TimelineRenderer::renderBlock) remains the same
// alloc-free / lock-free code the live engine runs.
//
// DETERMINISM (PRD-0099 §1.5.1): clip reads are SYNCHRONOUS full-reads — each
// block blocks until the required source samples are decoded, so the render
// never substitutes silence on a slow read and is bit-identical to an ideal
// glitch-free playback. This is the only sanctioned behavioural difference from
// the live path; everything downstream of the read is the identical engine code.
//
// NO SINGLETONS: every dependency is injected. Tests build a synthetic
// ArrangementSnapshot, supply in-memory readers via a ReaderProvider, and
// (optionally) inject an AutomationApplier + its DawTransport to assert parity.
//==============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "../Playback/ArrangementSnapshot.h"
#include "../Playback/ArrangementPublisher.h"
#include "../Playback/ClipStreamer.h"
#include "../Playback/TimelineRenderer.h"

namespace Daw
{

// Forward declarations (the applier + transport are optional injected deps).
class AutomationApplier;
class DawTransport;

//==============================================================================
// OfflineRenderConfig — the render contract supplied at construction.
//==============================================================================

struct OfflineRenderConfig
{
    //--------------------------------------------------------------------------
    // Render range. WholeArrangement renders [0, arrangementEnd); Region renders
    // the explicit half-open [startSample, endSample) at the render sample rate.
    //--------------------------------------------------------------------------
    enum class RangeMode { WholeArrangement, Region };

    //--------------------------------------------------------------------------
    // Tail policy (PRD-0099 §1.5.3):
    //   None              — stop exactly at the range end (region default).
    //   FixedLength       — append tailFixedSamples of no-new-onset blocks.
    //   EngineSilenceCap  — keep rendering until the summed output stays below
    //                       tailSilenceThreshold for tailSilenceBlocks blocks,
    //                       capped at tailMaxSamples (whole-arrangement default).
    //--------------------------------------------------------------------------
    enum class TailPolicy { None, FixedLength, EngineSilenceCap };

    /// The compiled render model to render (PRD-0079/0081). Sample positions are
    /// expressed at the render sample rate (the caller compiles at that rate).
    ArrangementSnapshot snapshot {};

    RangeMode rangeMode { RangeMode::WholeArrangement };

    /// Explicit half-open range, used only when rangeMode == Region. For
    /// WholeArrangement these are ignored and derived from the snapshot.
    int64_t rangeStartSample { 0 };
    int64_t rangeEndSample   { 0 };

    /// The single numeric output rate. The driver emits float at this rate and
    /// performs NO bit-depth / dither / format conversion (PRD-0099 §1.5.4).
    double renderSampleRate { 44100.0 };

    /// Offline loop block size. Defaults to the live engine block size so any
    /// block-boundary automation quantisation matches playback (PRD-0099 §1.5.5).
    int blockSize { kDefaultBlockSize };

    /// Tail.
    TailPolicy tailPolicy           { TailPolicy::EngineSilenceCap };
    int64_t    tailFixedSamples     { 0 };
    int64_t    tailMaxSamples       { 0 };       // 0 => default of 10s @ rate
    float      tailSilenceThreshold { 1.0e-5f };
    int        tailSilenceBlocks    { 8 };

    /// The live engine's default processing block size (PRD-0099 §1.5.5).
    static constexpr int kDefaultBlockSize = 512;

    /// Convenience constructor for a whole-arrangement render.
    static OfflineRenderConfig wholeArrangement (ArrangementSnapshot snap,
                                                 double sampleRate,
                                                 int blockSz = kDefaultBlockSize);

    /// Convenience constructor for a region render (defaults to no tail).
    static OfflineRenderConfig region (ArrangementSnapshot snap,
                                       int64_t startSample,
                                       int64_t endSample,
                                       double sampleRate,
                                       int blockSz = kDefaultBlockSize);
};

//==============================================================================
// RenderResult — the outcome handed back from render().
//==============================================================================

struct RenderResult
{
    enum class Status { Completed, Cancelled };

    Status  status            { Status::Completed };

    /// Samples written to the sink (range + tail for Completed; partial count up
    /// to the last fully-written block for Cancelled — never a mid-block count).
    int64_t samplesRendered   { 0 };

    /// PRD-0099 §1.5.8 / PRD-0097: clips whose source could not be resolved and
    /// were therefore rendered as silence for their span. Identified by the
    /// ClipEvent::sourceFileId hash; never causes an abort.
    std::vector<uint64_t> silentClipSourceIds {};

    bool wasCancelled() const noexcept { return status == Status::Cancelled; }
    bool hasSilentClips() const noexcept { return ! silentClipSourceIds.empty(); }
};

//==============================================================================
// OfflineRenderDriver
//==============================================================================

class OfflineRenderDriver
{
public:
    //--------------------------------------------------------------------------
    // Injected seams.
    //--------------------------------------------------------------------------

    /// Resolves one clip to a fresh, synchronous-readable AudioFormatReader.
    /// Returns nullptr for an unresolved / missing source — the driver then
    /// renders that clip's span as silence and records its sourceFileId in the
    /// result (PRD-0099 §1.5.8). Called once per distinct clip before the loop,
    /// on the background render thread. In production this wraps a
    /// ClipSourceResolver; in tests it returns in-memory BufferAudioFormatReaders.
    using ReaderProvider =
        std::function<std::unique_ptr<juce::AudioFormatReader> (const ClipEvent&)>;

    /// Per-block sink (the streaming form PRD-0100 uses). Receives a stereo
    /// buffer view of exactly `numSamples` valid samples for the block at
    /// absolute timeline position `blockStartSample`. Channels 0/1 = L/R.
    using BlockSink =
        std::function<void (const juce::AudioBuffer<float>& block,
                            int numSamples,
                            int64_t blockStartSample)>;

    /// Progress reporter: receives a fraction in [0, 1]. Published at a coarse
    /// cadence (PRD-0099 §1.5.6); reaches exactly 1.0 on Completed.
    using ProgressReporter = std::function<void (float fraction)>;

    //--------------------------------------------------------------------------
    // Construction.
    //
    //   config         : the render contract (snapshot, range, rate, tail).
    //   readerProvider : per-clip reader supply (nullptr-returning => silence).
    //   applier        : OPTIONAL PRD-0092 applier driven per block at the
    //                    block-start playhead for automation/tempo parity. May be
    //                    nullptr (no automation / a pure summing render).
    //   applierTransport : the DawTransport the applier reads its playhead from.
    //                    The driver seeks it per block so tick() evaluates at the
    //                    correct sample. Required iff applier != nullptr.
    //--------------------------------------------------------------------------
    OfflineRenderDriver (OfflineRenderConfig config,
                         ReaderProvider     readerProvider,
                         AutomationApplier* applier          = nullptr,
                         DawTransport*      applierTransport = nullptr);

    ~OfflineRenderDriver();

    OfflineRenderDriver (const OfflineRenderDriver&)            = delete;
    OfflineRenderDriver& operator= (const OfflineRenderDriver&) = delete;

    //--------------------------------------------------------------------------
    // Render into a pre-sized buffer (the test/whole-buffer form). The buffer is
    // resized to (totalSamples = range + tail) x 2 and filled. BACKGROUND THREAD.
    //--------------------------------------------------------------------------
    RenderResult render (juce::AudioBuffer<float>& destBuffer,
                         std::atomic<float>*       progress    = nullptr,
                         std::atomic<bool>*        cancelToken = nullptr);

    //--------------------------------------------------------------------------
    // Render to a per-block sink (the streaming form). BACKGROUND THREAD.
    //--------------------------------------------------------------------------
    RenderResult render (const BlockSink&          sink,
                         const ProgressReporter&   progress    = {},
                         std::atomic<bool>*        cancelToken = nullptr);

    //--------------------------------------------------------------------------
    // The resolved total output length (range + tail upper bound) is only known
    // exactly after a render for the silence-capped tail; this returns the range
    // length plus the MAXIMUM tail so callers can pre-size a buffer safely.
    //--------------------------------------------------------------------------
    int64_t maxTotalSamples() const;

    /// The in-range sample count (rangeEnd - rangeStart), tail excluded.
    int64_t rangeSamples() const;

private:
    //--------------------------------------------------------------------------
    // Internal block loop shared by both render() overloads.
    //--------------------------------------------------------------------------
    RenderResult runLoop (const BlockSink&        sink,
                          const ProgressReporter& progress,
                          std::atomic<bool>*      cancelToken);

    // Resolve the explicit [start, end) range from the config + snapshot.
    void resolveRange (int64_t& startOut, int64_t& endOut) const;

    // Compute the arrangement end (max timelineEndSample across all lanes).
    int64_t arrangementEndSample() const;

    // Assign streamer slots, prime each clip at the seek-seeded source offset,
    // rewrite the working snapshot's sourceReadHandles, record unresolved clips.
    void primeStreamersForRange (int64_t rangeStart,
                                 std::vector<uint64_t>& silentOut);

    // Effective max tail in samples for the configured policy.
    int64_t effectiveMaxTailSamples() const;

    //--------------------------------------------------------------------------
    OfflineRenderConfig config_;
    ReaderProvider      readerProvider_;
    AutomationApplier*  applier_          { nullptr };
    DawTransport*       applierTransport_ { nullptr };

    // The render-scoped engine plumbing the driver owns. TimelineRenderer is
    // constructed against these and its renderBlock is reused unchanged.
    ArrangementPublisher          publisher_;
    std::unique_ptr<ClipStreamerPool> pool_;
    std::atomic<int64_t>          playhead_ { 0 };
    std::unique_ptr<TimelineRenderer> renderer_;

    // The working snapshot (a mutable copy of config_.snapshot with rewritten
    // sourceReadHandles), published to the renderer.
    ArrangementSnapshot working_ {};
};

} // namespace Daw
