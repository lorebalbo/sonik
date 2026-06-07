#pragma once
//==============================================================================
// EPIC-0010: DawPlaybackController — message-thread glue that connects the
// arrangement compiler to the audio playback layer.
//
// Responsibilities:
//   - Owns the ClipStreamerPool and an AudioFormatManager.
//   - Provides a ClipHandleResolver that, for each clip in the arrangement,
//       1. assigns a dedicated streamer slot keyed by the *clipId* (so clips
//          sharing a source — e.g. Original/Vocal/Instrumental of one song —
//          never collide), and
//       2. opens the correct source reader (ClipSourceResolver) and primes the
//          streamer at the source offset matching the current playhead, so
//          play-from-here and seek both line up sample-accurately.
//   - Reconciles the project sample rate (44.1 kHz) with the runtime/device
//     rate via a scale factor handed to the ArrangementCompiler.
//
// MESSAGE THREAD ONLY. The pool it owns is read by the audio thread through the
// TimelineRenderer, but all priming/slot assignment happens here off the audio
// thread.
//==============================================================================

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ClipStreamer.h"
#include "ClipSourceResolver.h"
#include "ArrangementCompiler.h"
#include "DawTransport.h"
#include "../State/DawState.h"
#include "../../Deck/Database/TrackDatabase.h"

namespace Daw
{

class DawPlaybackController
{
public:
    DawPlaybackController (TrackDatabase& database, DawTransport& transport)
        : transport_ (transport)
        , sourceResolver_ (database, formatManager_)
    {
        formatManager_.registerBasicFormats();
    }

    /// Pool consumed by the AudioEngine's TimelineRenderer (audio thread reads).
    ClipStreamerPool& getPool() noexcept { return pool_; }

    /// PRD-0098: inject the import-source publisher so imported clips
    /// ("import:<hash>") resolve to the atomic in-memory reader (audible after
    /// publish, silence before). Message thread, set once at construction time.
    void setImportPublisher (Daw::Import::ImportSourcePublisher* publisher) noexcept
    {
        sourceResolver_.setImportPublisher (publisher);
    }

    /// Update the runtime/device sample rate (call when the device starts/changes).
    void setRuntimeSampleRate (double runtimeRate) noexcept
    {
        if (runtimeRate > 0.0)
            runtimeSampleRate_ = runtimeRate;
    }

    /// Multiplier converting project-rate sample positions to runtime rate.
    double sampleRateScale() const noexcept
    {
        return runtimeSampleRate_ / DawState::kProjectSampleRate;
    }

    /// Build a compiler wired to this controller's resolver + current rate scale.
    ArrangementCompiler makeCompiler()
    {
        return ArrangementCompiler (makeResolver(), sampleRateScale());
    }

    /// The per-clip resolver: assigns a streamer slot and primes it.
    ArrangementCompiler::ClipHandleResolver makeResolver()
    {
        return [this] (const ArrangementCompiler::ClipResolveRequest& req) -> int32_t
        {
            if (req.clipId.isEmpty())
                return -1;

            const int32_t slot = pool_.resolveHandle (req.clipId);
            if (slot < 0)
                return -1; // pool full

            // Open the source reader for this lane's audio (may be nullptr →
            // streamer plays silence for a missing/unseparated source).
            auto reader = sourceResolver_.resolve (req.sourceFileId, req.laneKind);

            const double projectRate = DawState::kProjectSampleRate;
            const double sourceRate  = (reader != nullptr && reader->sampleRate > 0.0)
                                           ? reader->sampleRate
                                           : projectRate;

            // Align the prime offset to the current playhead so that
            // play-from-here and seek both read the right source sample first.
            const int64_t playheadRuntime = transport_.getPlayheadSample();
            const double  scale           = sampleRateScale();
            const int64_t playheadProject =
                (playheadRuntime <= 0 || scale <= 0.0)
                    ? 0
                    : (int64_t) std::llround ((double) playheadRuntime / scale);

            const int64_t offsetIntoClip =
                juce::jmax ((int64_t) 0, playheadProject - req.timelineStartSample);
            int64_t primeStartProject = req.sourceStartSample + offsetIntoClip;
            primeStartProject = juce::jmin (primeStartProject, req.sourceEndSample);

            // Convert project-rate source positions to the reader's native rate.
            const double  srcPerProject = sourceRate / projectRate;
            const int64_t readerStart   = (int64_t) std::llround ((double) primeStartProject * srcPerProject);

            // EPIC-0009: prime a short continuation tail past the crop end so a
            // butt-joined clip can render the crossfade tail the renderer reads
            // (kClipFadeSamples extra RUNTIME samples). Converted to the reader's
            // native rate. Harmless for non-joined clips — those samples are
            // primed but never read. Reading past the true source end yields
            // silence (the reader clamps), which is the correct fallback.
            const int64_t tailSourceSamples =
                (int64_t) std::ceil ((double) kClipFadeSamples * sourceRate / runtimeSampleRate_) + 2;
            const int64_t readerEnd      = (int64_t) std::llround ((double) req.sourceEndSample * srcPerProject)
                                           + tailSourceSamples;

            // Prime the streamer to emit runtime-rate samples (resampling from
            // the source rate inside the streamer's reader loop).
            pool_.prime (slot, std::move (reader), runtimeSampleRate_, readerStart, readerEnd);
            return slot;
        };
    }

private:
    DawTransport&            transport_;
    juce::AudioFormatManager formatManager_;
    ClipStreamerPool         pool_;
    ClipSourceResolver       sourceResolver_;
    double                   runtimeSampleRate_ { DawState::kProjectSampleRate };
};

} // namespace Daw
