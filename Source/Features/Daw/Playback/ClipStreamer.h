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
// AUDIO THREAD CONTRACT (AGENTS.md):
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
        : ringCapacity_ (ringCapacitySamples)
        , ringBuffer_   (2, ringCapacitySamples)
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
    void prime (std::unique_ptr<juce::AudioFormatReader> reader,
                double   projectSampleRate,
                int64_t  sourceStartSample,
                int64_t  sourceEndSample)
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
            interpL_.reset();
            interpR_.reset();
        }

        // Reset ring buffer.
        fifo_.reset();
        readPosition_.store (0, std::memory_order_relaxed);
        generation_.fetch_add (1, std::memory_order_release);
        underrunCount_.store (0, std::memory_order_relaxed);
        exhausted_ = false;

        // Restart background reader thread.
        running_.store (true, std::memory_order_relaxed);
        readerThread_ = std::thread ([this] { readerLoop(); });
    }

    /// Stop the background reader (called on destruction or re-prime).
    void stop()
    {
        running_.store (false, std::memory_order_relaxed);
        cv_.notify_all();
        if (readerThread_.joinable())
            readerThread_.join();
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

            if (exhausted_)
                continue;

            // How many project-rate samples to write this iteration.
            const int toWrite = juce::jmin (fifo_.getFreeSpace(), kMaxOutChunk);
            if (toWrite <= 0)
                continue;

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

                const double ratio = (projectSampleRate_ > 0.0)
                                         ? (sourceSampleRate_ / projectSampleRate_)
                                         : 1.0;

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
                continue;
            }
        }
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
    bool                                     exhausted_         { false };

    // Ring buffer (pre-allocated, non-heap after construction).
    const int                    ringCapacity_;
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
                int64_t sourceEndSample)
    {
        if (auto* s = getStreamer (slotIndex))
            s->prime (std::move (reader), projectSampleRate, sourceStartSample, sourceEndSample);
    }

    int poolSize() const noexcept { return (int) streamers_.size(); }

private:
    std::vector<std::unique_ptr<ClipStreamer>> streamers_;
    std::unordered_map<juce::String, int>      slotByFileId_;
    std::unordered_map<int, juce::String>      slotToFileId_;
};

} // namespace Daw
