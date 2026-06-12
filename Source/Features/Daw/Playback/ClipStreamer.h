#pragma once
//==============================================================================
// PRD-0080: ClipStreamer — pre-rolls source audio off the audio thread into a
// lock-free ring buffer so the render engine (PRD-0081) can read project-rate
// samples from the audio thread without any disk I/O.
//
// ARCHITECTURE:
//   - Background reader thread reads from an AudioFormatReader (or silence for
//     missing files), resamples to project rate, and writes into a ring buffer.
//   - Audio thread calls readInto() which copies from the ring buffer — no I/O,
//     no allocation, no lock.
//   - Re-prime (seek/scrub/edit) is always triggered from off the audio thread.
//
// AUDIO THREAD CONTRACT (CLAUDE.md):
//   readInto()  → no allocation, no lock, no I/O.
//   All other public methods → message thread (or internal reader thread).
//==============================================================================

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <vector>
#include <unordered_map>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ArrangementSnapshot.h"
#include "../../TimeStretch/TimeStretcher.h"

namespace Daw
{

//==============================================================================
// ClipStreamer
//==============================================================================

class ClipStreamer
{
public:
    //--------------------------------------------------------------------------
    // Construction / destruction
    //--------------------------------------------------------------------------

    /// @param ringCapacitySamples  Pre-allocated ring buffer length in project-
    ///                             rate samples (stereo).  Default = 2s @ 44.1kHz.
    explicit ClipStreamer (int ringCapacitySamples = 44100 * 2)
        : ringBuffer_   (2, ringCapacitySamples)
        , fifo_         (ringCapacitySamples)
    {
        fifo_.reset();
    }

    ~ClipStreamer()
    {
        stop();
    }

    ClipStreamer (const ClipStreamer&)            = delete;
    ClipStreamer& operator= (const ClipStreamer&) = delete;

    //--------------------------------------------------------------------------
    // Configuration (message thread)
    //--------------------------------------------------------------------------

    /// Called on the message thread before playback or after a seek/edit.
    /// Sets the clip crop window and source file, then starts the background reader.
    ///
    /// @param reader          Heap-owning AudioFormatReader for the source file
    ///                        (may be nullptr for missing-source silence).
    /// @param projectSampleRate Project rate (used for resampling + buffer timing).
    /// @param sourceStartSample Crop start in SOURCE sample rate.
    /// @param sourceEndSample   Crop end (exclusive) in SOURCE sample rate.
    /// @param playbackConsumeRate Source samples consumed per output sample for
    ///                            tempo time-stretch (= masterBpm/sourceBpm). 1.0 =>
    ///                            no stretch. Folded into the resampler so the clip
    ///                            plays at the project tempo (EPIC elastic). NOTE:
    ///                            this is a varispeed re-time (pitch follows tempo);
    ///                            pitch-preserving stretch is layered on top later.
    void prime (std::unique_ptr<juce::AudioFormatReader> reader,
                double   projectSampleRate,
                int64_t  sourceStartSample,
                int64_t  sourceEndSample,
                double   playbackConsumeRate = 1.0,
                bool     keyLock             = false)
    {
        // Signal reader thread to stop, then wait.
        stop();

        // Store configuration.
        {
            std::lock_guard<std::mutex> lk (configMutex_);
            reader_              = std::move (reader);
            projectSampleRate_   = projectSampleRate;
            sourceSampleRate_    = (reader_ != nullptr && reader_->sampleRate > 0.0)
                                       ? reader_->sampleRate
                                       : projectSampleRate;
            sourceStartSample_   = sourceStartSample;
            sourceEndSample_     = sourceEndSample;
            readerPosition_      = sourceStartSample;
            playbackConsumeRate_ = (playbackConsumeRate > 0.0) ? playbackConsumeRate : 1.0;
            interpL_.reset();
            interpR_.reset();

            // PITCH-PRESERVED (key lock) mode: when the clip is both key-locked AND
            // actually stretched, route the SR-converted source through Rubberband
            // (time ratio = 1/consumeRate) so it retimes to the master tempo WITHOUT
            // changing pitch. Otherwise the plain varispeed resampler is used.
            const bool stretching = std::abs (playbackConsumeRate_ - 1.0) > 1.0e-6;
            pitchPreserve_   = keyLock && stretching;
            stretchTimeRatio_ = (playbackConsumeRate_ > 0.0) ? (1.0 / playbackConsumeRate_) : 1.0;
            stretcherPrimed_  = false;
            if (pitchPreserve_)
            {
                if (stretcher_ == nullptr)
                    stretcher_ = std::make_unique<TimeStretcher> (projectSampleRate_, 2, kStretchBlock);
                else
                    stretcher_->reset();
            }
        }

        // Reset ring buffer.
        fifo_.reset();
        readPosition_.store (0, std::memory_order_relaxed);
        generation_.fetch_add (1, std::memory_order_release);
        underrunCount_.store (0, std::memory_order_relaxed);
        exhausted_ = false;
        exhaustedFlag_.store (false, std::memory_order_release);

        // Restart background reader thread.
        running_.store (true, std::memory_order_relaxed);
        readerThread_ = std::thread ([this] { readerLoop(); });
    }

    /// Stop the background reader (called on destruction or re-prime).
    void stop()
    {
        running_.store (false, std::memory_order_relaxed);
        cv_.notify_all();
        producedCv_.notify_all();
        if (readerThread_.joinable())
            readerThread_.join();
    }

    //--------------------------------------------------------------------------
    // PRD-0099: Offline synchronous-full-read seam (BACKGROUND THREAD ONLY).
    //
    // The live (audio-thread) path tolerates a prefetch miss by emitting silence
    // (see readInto). An offline render must NEVER substitute silence for a slow
    // read, so the offline driver puts the streamer into synchronous mode and
    // calls waitUntilReady(n) on its OWN background thread before each readInto.
    // waitUntilReady blocks the CALLING (non-audio) thread until either the ring
    // holds >= n project-rate samples OR the source is exhausted (genuine end of
    // content), so the subsequent readInto consumes real samples and only fills
    // silence past the true source end. Live behaviour is unaffected: the flag
    // defaults off and the audio thread never calls waitUntilReady.
    //--------------------------------------------------------------------------

    /// Enable/disable synchronous full-read mode for the streamer's lifetime
    /// (render-scoped). Message/background thread only. Does NOT affect readInto;
    /// it only gates whether waitUntilReady is meaningful for the caller.
    void setSynchronousMode (bool enabled) noexcept
    {
        synchronousMode_.store (enabled, std::memory_order_relaxed);
    }

    bool isSynchronousMode() const noexcept
    {
        return synchronousMode_.load (std::memory_order_relaxed);
    }

    /// Block the calling (background) thread until at least `numSamples`
    /// project-rate samples are ready in the ring, OR the reader has exhausted
    /// the source (no more samples will ever arrive). Returns the number of
    /// samples actually available at the moment it returns (>= numSamples unless
    /// the source ended first). BACKGROUND THREAD ONLY — never the audio thread.
    int waitUntilReady (int numSamples) noexcept
    {
        auto ready = [this, numSamples] {
            return ! running_.load (std::memory_order_relaxed)
                || fifo_.getNumReady() >= numSamples
                || exhaustedFlag_.load (std::memory_order_acquire);
        };

        std::unique_lock<std::mutex> lk (producedMutex_);
        while (! ready())
        {
            // Kick the reader thread: readInto() notifies cv_ from the audio
            // thread WITHOUT configMutex_ (RT constraint), so that wakeup can
            // land between the reader's predicate check and its block and be
            // lost — leaving producer and consumer both asleep (deadlock seen
            // in the offline region render). Re-notifying here from the
            // non-RT driver thread breaks the cycle; the bounded wait_for
            // covers the symmetric race on producedCv_.
            cv_.notify_one();
            producedCv_.wait_for (lk, std::chrono::milliseconds (2), ready);
        }
        return fifo_.getNumReady();
    }

    /// Returns true if the source is valid (reader != nullptr or silence mode).
    bool isActive() const noexcept
    {
        return running_.load (std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Audio thread API
    //--------------------------------------------------------------------------

    /// Read `numSamples` of project-rate stereo audio into `dest` starting at
    /// `destStartSample`.  Real-time safe: no allocation, no lock, no I/O.
    ///
    /// If fewer samples than requested are available (underrun), the missing
    /// region is zeroed and the underrun counter is incremented.
    void readInto (juce::AudioBuffer<float>& dest,
                   int destStartSample,
                   int numSamples) noexcept
    {
        const int avail = fifo_.getNumReady();
        const int canRead = juce::jmin (avail, numSamples);
        const int starved = numSamples - canRead;

        if (canRead > 0)
        {
            int b1start, b1size, b2start, b2size;
            fifo_.prepareToRead (canRead, b1start, b1size, b2start, b2size);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* ring = ringBuffer_.getReadPointer (ch);
                float*       out  = dest.getWritePointer (ch, destStartSample);

                if (b1size > 0) std::memcpy (out,          ring + b1start, (size_t) b1size * sizeof (float));
                if (b2size > 0) std::memcpy (out + b1size, ring + b2start, (size_t) b2size * sizeof (float));
            }

            fifo_.finishedRead (canRead);
            readPosition_.fetch_add (canRead, std::memory_order_relaxed);
        }

        // Zero-fill the starved region.
        if (starved > 0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                float* out = dest.getWritePointer (ch, destStartSample + canRead);
                std::memset (out, 0, (size_t) starved * sizeof (float));
            }
            underrunCount_.fetch_add (1, std::memory_order_relaxed);
        }

        // Wake the reader thread.
        cv_.notify_one();
    }

    //--------------------------------------------------------------------------
    // Diagnostics (any thread)
    //--------------------------------------------------------------------------

    int underrunCount() const noexcept
    {
        return underrunCount_.load (std::memory_order_relaxed);
    }

    int generation() const noexcept
    {
        return generation_.load (std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Pool-management slot id (set by the pool, never by the streamer itself)
    //--------------------------------------------------------------------------
    int slotIndex { -1 };

private:
    //--------------------------------------------------------------------------
    // Background reader loop
    //--------------------------------------------------------------------------

    void readerLoop()
    {
        // Max project-rate output samples produced per iteration.
        constexpr int kMaxOutChunk = 1024;
        // Source-rate input scratch must hold kMaxOutChunk * maxRatio samples.
        // Max supported source/project ratio ~8 (e.g. 352.8kHz -> 44.1kHz).
        constexpr int kInScratchSamples = kMaxOutChunk * 8 + 16;

        juce::AudioBuffer<float> inScratch  (2, kInScratchSamples);
        juce::AudioBuffer<float> outScratch (2, kMaxOutChunk);

        // Pitch-preserved path scratch: SR-converted source feeding the stretcher,
        // and the stretcher's time-stretched output bound for the ring.
        juce::AudioBuffer<float> stretchIn  (2, kStretchBlock);
        juce::AudioBuffer<float> stretchOut (2, kStretchBlock);

        while (running_.load (std::memory_order_relaxed))
        {
            // Wait until there is room in the ring buffer or we are stopping.
            {
                std::unique_lock<std::mutex> lk (configMutex_);
                cv_.wait (lk, [this] {
                    return !running_.load (std::memory_order_relaxed)
                        || (fifo_.getFreeSpace() > 0 && !exhausted_);
                });

                if (!running_.load (std::memory_order_relaxed))
                    break;
            }

            // PRD-0099: RAII notifier — on EVERY exit path of this iteration
            // (including all `continue`s below) mirror exhaustion into the
            // atomic and wake any synchronous offline reader blocked in
            // waitUntilReady(). Reader thread only; the audio thread never sees it.
            struct ProducedNotifier
            {
                ClipStreamer& s;
                ~ProducedNotifier()
                {
                    // Publish under producedMutex_ so a consumer inside
                    // waitUntilReady either observes the new state in its
                    // predicate or is already blocked when notify_all fires —
                    // never the lost-wakeup window in between.
                    {
                        std::lock_guard<std::mutex> lk (s.producedMutex_);
                        s.exhaustedFlag_.store (s.exhausted_, std::memory_order_release);
                    }
                    s.producedCv_.notify_all();
                }
            } producedNotifier { *this };

            if (exhausted_)
                continue;

            // How many project-rate samples to write this iteration.
            const int toWrite = juce::jmin (fifo_.getFreeSpace(), kMaxOutChunk);
            if (toWrite <= 0)
                continue;

            // ----------------------------------------------------------------
            // PITCH-PRESERVED (key lock) path: SR-convert the source to runtime
            // rate, time-stretch it with Rubberband (pitch unchanged), and write
            // the stretched output to the ring. Fed in source order from the clip
            // start, so output tracks the source start; the algorithm's pipeline
            // fill is absorbed by the ring pre-roll.
            // ----------------------------------------------------------------
            if (pitchPreserve_ && stretcher_ != nullptr)
            {
                std::lock_guard<std::mutex> lk (configMutex_);

                if (reader_ == nullptr)
                {
                    exhausted_ = true;
                    continue;
                }

                const double srRatio = (projectSampleRate_ > 0.0)
                                           ? (sourceSampleRate_ / projectSampleRate_) : 1.0;

                // SR-convert up to `want` runtime samples from the source into `dst`,
                // advancing readerPosition_; returns the runtime samples produced.
                auto srConvert = [&] (int want, juce::AudioBuffer<float>& dst) -> int
                {
                    if (want <= 0 || readerPosition_ >= sourceEndSample_)
                        return 0;
                    if (std::abs (srRatio - 1.0) < 1.0e-9)
                    {
                        const int64_t remaining = sourceEndSample_ - readerPosition_;
                        const int n = (int) juce::jmin ((int64_t) want, remaining);
                        if (n <= 0) return 0;
                        reader_->read (&dst, 0, n, readerPosition_, true, true);
                        if (reader_->numChannels == 1) dst.copyFrom (1, 0, dst, 0, 0, n);
                        readerPosition_ += n;
                        return n;
                    }
                    const int64_t remainingIn = sourceEndSample_ - readerPosition_;
                    int inWanted = (int) std::ceil ((double) want * srRatio) + 2;
                    inWanted = juce::jmin (inWanted, kInScratchSamples);
                    const int inAvail = (int) juce::jmin ((int64_t) inWanted, remainingIn);
                    if (inAvail <= 0) return 0;
                    reader_->read (&inScratch, 0, inAvail, readerPosition_, true, true);
                    if (reader_->numChannels == 1) inScratch.copyFrom (1, 0, inScratch, 0, 0, inAvail);
                    int numOut = (inAvail >= inWanted) ? want
                                                       : (int) std::floor ((double) inAvail / srRatio);
                    numOut = juce::jlimit (0, want, numOut);
                    if (numOut <= 0) return 0;
                    const int usedL = interpL_.process (srRatio, inScratch.getReadPointer (0),
                                                        dst.getWritePointer (0), numOut);
                    interpR_.process (srRatio, inScratch.getReadPointer (1),
                                      dst.getWritePointer (1), numOut);
                    readerPosition_ += usedL;
                    return numOut;
                };

                stretcherPrimed_ = true; // (reserved; sequential feed needs no priming)

                // Feed a source chunk (or silence once the source ends, to flush the
                // pipeline tail) and retrieve up to `toWrite` stretched samples.
                int got = 0;
                if (readerPosition_ < sourceEndSample_)
                {
                    const int inN = srConvert (kStretchBlock, stretchIn);
                    if (inN > 0)
                        got = stretcher_->process (stretchIn.getArrayOfReadPointers(), inN,
                                                   stretchOut.getArrayOfWritePointers(), toWrite,
                                                   stretchTimeRatio_);
                }
                else
                {
                    stretchIn.clear();
                    got = stretcher_->process (stretchIn.getArrayOfReadPointers(),
                                               juce::jmin (kStretchBlock, toWrite),
                                               stretchOut.getArrayOfWritePointers(), toWrite,
                                               stretchTimeRatio_);
                    if (got <= 0)
                    {
                        exhausted_ = true;
                        continue;
                    }
                }

                if (got > 0)
                {
                    int w1start, w1size, w2start, w2size;
                    fifo_.prepareToWrite (got, w1start, w1size, w2start, w2size);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float* src  = stretchOut.getReadPointer (ch);
                        float*       ring = ringBuffer_.getWritePointer (ch);
                        if (w1size > 0) std::memcpy (ring + w1start, src,          (size_t) w1size * sizeof (float));
                        if (w2size > 0) std::memcpy (ring + w2start, src + w1size, (size_t) w2size * sizeof (float));
                    }
                    fifo_.finishedWrite (got);
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lk (configMutex_);

                const bool hasSamples = reader_ != nullptr
                                        && readerPosition_ < sourceEndSample_;
                if (!hasSamples)
                {
                    // Silence (missing source or past end of source).
                    if (reader_ == nullptr || readerPosition_ >= sourceEndSample_)
                    {
                        exhausted_ = true;
                        continue;
                    }
                }

                // Resample ratio = source-rate conversion * tempo time-stretch.
                // playbackConsumeRate_ > 1 consumes source faster (clip plays
                // faster, e.g. a 128-BPM clip on a 140 master), < 1 slower.
                const double ratio = ((projectSampleRate_ > 0.0)
                                         ? (sourceSampleRate_ / projectSampleRate_)
                                         : 1.0)
                                     * playbackConsumeRate_;

                int producedOut = 0;

                if (std::abs (ratio - 1.0) < 1.0e-9)
                {
                    // No resampling: read project-rate == source-rate directly.
                    const int64_t remaining = sourceEndSample_ - readerPosition_;
                    const int n = (int) juce::jmin ((int64_t) toWrite, remaining);
                    reader_->read (&outScratch, 0, n, readerPosition_, true, true);
                    if (reader_->numChannels == 1)
                        outScratch.copyFrom (1, 0, outScratch, 0, 0, n);
                    readerPosition_ += n;
                    producedOut = n;
                }
                else
                {
                    // Resample source-rate -> project-rate via Lagrange interpolation.
                    const int64_t remainingIn = sourceEndSample_ - readerPosition_;
                    int inWanted = (int) std::ceil ((double) toWrite * ratio) + 2;
                    inWanted = juce::jmin (inWanted, kInScratchSamples);
                    const int inAvail = (int) juce::jmin ((int64_t) inWanted, remainingIn);
                    if (inAvail <= 0)
                    {
                        exhausted_ = true;
                        continue;
                    }

                    reader_->read (&inScratch, 0, inAvail, readerPosition_, true, true);
                    if (reader_->numChannels == 1)
                        inScratch.copyFrom (1, 0, inScratch, 0, 0, inAvail);

                    // Number of outputs we can produce from inAvail inputs.
                    int numOut = (inAvail >= inWanted)
                                     ? toWrite
                                     : (int) std::floor ((double) inAvail / ratio);
                    numOut = juce::jlimit (0, toWrite, numOut);
                    if (numOut <= 0)
                    {
                        // Not enough input to make a sample; if at source end, finish.
                        if (remainingIn <= inAvail)
                            exhausted_ = true;
                        continue;
                    }

                    const int usedL = interpL_.process (ratio,
                                                        inScratch.getReadPointer (0),
                                                        outScratch.getWritePointer (0),
                                                        numOut);
                    interpR_.process (ratio,
                                      inScratch.getReadPointer (1),
                                      outScratch.getWritePointer (1),
                                      numOut);
                    readerPosition_ += usedL;
                    producedOut = numOut;
                }

                if (readerPosition_ >= sourceEndSample_)
                    exhausted_ = true;

                if (producedOut <= 0)
                    continue;

                // Write produced project-rate samples into the ring buffer.
                int w1start, w1size, w2start, w2size;
                fifo_.prepareToWrite (producedOut, w1start, w1size, w2start, w2size);

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* src  = outScratch.getReadPointer (ch);
                    float*       ring = ringBuffer_.getWritePointer (ch);

                    if (w1size > 0) std::memcpy (ring + w1start, src,          (size_t) w1size * sizeof (float));
                    if (w2size > 0) std::memcpy (ring + w2start, src + w1size, (size_t) w2size * sizeof (float));
                }

                fifo_.finishedWrite (producedOut);
            }
            // ProducedNotifier (declared above) fires here on iteration exit,
            // mirroring exhaustion and waking any synchronous offline reader.
        }

        // Final wake on shutdown so a blocked waitUntilReady() never deadlocks.
        // Store under producedMutex_ (see ProducedNotifier) to close the
        // lost-wakeup window against a concurrently-entering waiter.
        {
            std::lock_guard<std::mutex> lk (producedMutex_);
            exhaustedFlag_.store (true, std::memory_order_release);
        }
        producedCv_.notify_all();
    }

    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------

    // Configuration (protected by configMutex_ during non-audio thread access).
    std::mutex                               configMutex_;
    std::unique_ptr<juce::AudioFormatReader> reader_;
    double                                   projectSampleRate_ { 44100.0 };
    double                                   sourceSampleRate_  { 44100.0 };
    juce::LagrangeInterpolator               interpL_;
    juce::LagrangeInterpolator               interpR_;
    int64_t                                  sourceStartSample_ { 0 };
    int64_t                                  sourceEndSample_   { 0 };
    int64_t                                  readerPosition_    { 0 };
    double                                   playbackConsumeRate_ { 1.0 }; // tempo stretch
    bool                                     exhausted_         { false };

    // Pitch-preserved (key lock) time-stretch via Rubberband (reader thread only).
    static constexpr int                     kStretchBlock      { 1024 };
    std::unique_ptr<TimeStretcher>           stretcher_;
    bool                                     pitchPreserve_     { false };
    double                                   stretchTimeRatio_  { 1.0 };  // output/input duration
    bool                                     stretcherPrimed_   { false };

    // Ring buffer (pre-allocated, non-heap after construction).
    juce::AudioBuffer<float>     ringBuffer_;
    juce::AbstractFifo           fifo_;

    // Shared atomic state.
    std::atomic<int64_t>   readPosition_  { 0 };
    std::atomic<int>       underrunCount_ { 0 };
    std::atomic<int>       generation_    { 0 };
    std::atomic<bool>      running_       { false };

    // Background reader thread.
    std::thread               readerThread_;
    std::condition_variable   cv_;

    // PRD-0099: offline synchronous-full-read seam. `synchronousMode_` is a
    // render-scoped flag (live default: false). `exhaustedFlag_` mirrors the
    // mutex-protected `exhausted_` so waitUntilReady() can observe end-of-source
    // without taking configMutex_. `producedCv_` is signalled by the reader after
    // each ring write / on exhaustion so an offline consumer can block for data.
    std::atomic<bool>         synchronousMode_ { false };
    std::atomic<bool>         exhaustedFlag_   { false };
    std::mutex                producedMutex_;
    std::condition_variable   producedCv_;
};

//==============================================================================
// ClipStreamerPool
//==============================================================================

/// Pre-allocated pool of ClipStreamers.  Message-thread owned.
/// Resolves sourceFileId → slot index for the ArrangementCompiler.
class ClipStreamerPool
{
public:
    static constexpr int kPoolSize = Daw::kMaxLanes * Daw::kMaxClipsPerLane / 4;
    // Use a smaller default pool for practical DJ sets.
    static constexpr int kDefaultPoolSize = 48;

    explicit ClipStreamerPool (int poolSize = kDefaultPoolSize)
    {
        streamers_.reserve ((size_t) poolSize);
        for (int i = 0; i < poolSize; ++i)
        {
            auto s = std::make_unique<ClipStreamer>();
            s->slotIndex = i;
            streamers_.push_back (std::move (s));
        }
        slotByFileId_.reserve (64);
    }

    ~ClipStreamerPool()
    {
        // Stop all streamers before destruction.
        for (auto& s : streamers_)
            s->stop();
    }

    ClipStreamerPool (const ClipStreamerPool&) = delete;
    ClipStreamerPool& operator= (const ClipStreamerPool&) = delete;

    //--------------------------------------------------------------------------
    // Message-thread API
    //--------------------------------------------------------------------------

    /// Resolve a sourceFileId to a slot index, allocating a new slot if needed.
    /// Returns -1 if the pool is full (no free slot available).
    int32_t resolveHandle (const juce::String& sourceFileId)
    {
        // Check if already assigned.
        auto it = slotByFileId_.find (sourceFileId);
        if (it != slotByFileId_.end())
            return it->second;

        // Find a free (non-running) slot.
        for (auto& s : streamers_)
        {
            if (!s->isActive() && slotToFileId_.find (s->slotIndex) == slotToFileId_.end())
            {
                slotByFileId_[sourceFileId]    = s->slotIndex;
                slotToFileId_[s->slotIndex]    = sourceFileId;
                return s->slotIndex;
            }
        }

        return -1; // pool full
    }

    /// Get the ClipStreamer for a given slot index.
    ClipStreamer* getStreamer (int32_t slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= (int) streamers_.size())
            return nullptr;
        return streamers_[(size_t) slotIndex].get();
    }

    /// Release the slot for a sourceFileId (called when a clip is deleted).
    void releaseHandle (const juce::String& sourceFileId)
    {
        auto it = slotByFileId_.find (sourceFileId);
        if (it == slotByFileId_.end()) return;

        const int slot = it->second;
        if (auto* s = getStreamer (slot))
            s->stop();

        slotToFileId_.erase (slot);
        slotByFileId_.erase (it);
    }

    /// Prime a slot given a format reader and crop window.
    void prime (int32_t slotIndex,
                std::unique_ptr<juce::AudioFormatReader> reader,
                double projectSampleRate,
                int64_t sourceStartSample,
                int64_t sourceEndSample,
                double playbackConsumeRate = 1.0,
                bool keyLock = false)
    {
        if (auto* s = getStreamer (slotIndex))
            s->prime (std::move (reader), projectSampleRate,
                      sourceStartSample, sourceEndSample, playbackConsumeRate, keyLock);
    }

    int poolSize() const noexcept { return (int) streamers_.size(); }

private:
    std::vector<std::unique_ptr<ClipStreamer>> streamers_;
    std::unordered_map<juce::String, int>      slotByFileId_;
    std::unordered_map<int, juce::String>      slotToFileId_;
};

} // namespace Daw
