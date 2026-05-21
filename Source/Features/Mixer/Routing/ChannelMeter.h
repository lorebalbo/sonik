#pragma once
//==============================================================================
// PRD-0058: ChannelMeter — per-strip / per-master metering DSP.
//
// Computes, per stereo side:
//   - Instantaneous peak with 300 ms exponential decay.
//   - Peak-hold with 1.5 s linear decay.
//   - RMS over a 300 ms rectangular window (running sum over a ring of
//     squared samples).
//   - Clip latch that engages on any sample with |x| >= 1.0 and auto-clears
//     3.0 s after the last clip-sample event. The latch may also be cleared
//     manually from the message thread by writing `false` into the clip
//     atomic; the audio thread observes the manual clear on its next block
//     and resets its internal sample counter so the latch does not re-fire
//     from stale state.
//
// Audio-thread contract (AGENTS.md):
//   - prepare() runs on the message thread before the audio callback starts.
//     It is the only path that allocates the RMS ring buffers.
//   - processBlock() and reset() never allocate, never lock, never do I/O.
//   - All publishes to the MixerMeterSnapshot use relaxed atomic stores.
//==============================================================================

#include "../State/MixerMeterSnapshot.h"

#include <algorithm>
#include <cmath>
#include <vector>

class ChannelMeter
{
public:
    /// Pre-allocate ring buffers and reset internal state.
    /// Must be called on the message thread before the audio callback uses
    /// processBlock().
    void prepare (double sampleRate)
    {
        sampleRate_         = sampleRate > 0.0 ? sampleRate : 44100.0;
        rmsWindowSamples_   = std::max (1, static_cast<int> (std::round (sampleRate_ * 0.3)));
        clipHoldSamples_    = std::max (1, static_cast<int> (std::round (sampleRate_ * 3.0)));

        rmsBufL_.assign (static_cast<size_t> (rmsWindowSamples_), 0.0f);
        rmsBufR_.assign (static_cast<size_t> (rmsWindowSamples_), 0.0f);

        // exp(-1 / (sr * 0.3)) — 300 ms exponential decay for peak.
        peakDecayCoeff_         = std::exp (-1.0f / static_cast<float> (sampleRate_ * 0.3));
        // 1 / (sr * 1.5) — linear decay reaches zero in 1.5 s.
        peakHoldDecayPerSample_ = 1.0f / static_cast<float> (sampleRate_ * 1.5);

        reset();
    }

    /// Reset all running DSP state. Cheap and audio-thread safe.
    void reset() noexcept
    {
        std::fill (rmsBufL_.begin(), rmsBufL_.end(), 0.0f);
        std::fill (rmsBufR_.begin(), rmsBufR_.end(), 0.0f);
        rmsSumL_          = 0.0;
        rmsSumR_          = 0.0;
        rmsIdx_           = 0;
        peakL_            = 0.0f;
        peakR_            = 0.0f;
        peakHoldL_        = 0.0f;
        peakHoldR_        = 0.0f;
        // Initialise the counter past the hold so a stale clip is not held
        // open across a reset.
        samplesSinceClip_ = clipHoldSamples_ + 1;
    }

    /// Wire the meter slot this instance publishes into. May be nullptr —
    /// in that case processBlock() is a no-op (used by unit tests that do
    /// not care about meters).
    void setSlot (ChannelMeterSlots* slot) noexcept { slot_ = slot; }

    /// Process one audio block. Reads `L`/`R` only (does not modify them).
    /// Publishes the latest peak/peakHold/RMS/clip values to the configured
    /// MixerMeterSnapshot slot via relaxed atomic stores.
    void processBlock (const float* L, const float* R, int numSamples) noexcept
    {
        if (slot_ == nullptr || numSamples <= 0)
            return;

        // PRD-0058 §1.5.5: a manual clear from the message thread writes
        // `false` to the clip atomic. The audio thread observes that here
        // and resets the elapsed counter so the auto-clear logic does not
        // immediately latch the indicator back on from a stale sample
        // tracked just before the user clicked.
        bool clipLatch = slot_->clip.load (std::memory_order_relaxed);
        if (! clipLatch)
            samplesSinceClip_ = clipHoldSamples_ + 1;

        for (int i = 0; i < numSamples; ++i)
        {
            const float sL = L[i];
            const float sR = R[i];
            const float aL = std::fabs (sL);
            const float aR = std::fabs (sR);

            // Instantaneous peak with exponential decay.
            peakL_ = std::max (peakL_ * peakDecayCoeff_, aL);
            peakR_ = std::max (peakR_ * peakDecayCoeff_, aR);

            // Peak-hold: linear decay clamped to never fall below the
            // current instantaneous peak. A fresh peak therefore resets the
            // hold instantly upward.
            peakHoldL_ = std::max (peakHoldL_ - peakHoldDecayPerSample_, peakL_);
            peakHoldR_ = std::max (peakHoldR_ - peakHoldDecayPerSample_, peakR_);
            if (peakHoldL_ < 0.0f) peakHoldL_ = 0.0f;
            if (peakHoldR_ < 0.0f) peakHoldR_ = 0.0f;

            // RMS ring: replace the oldest squared sample, maintain a
            // running sum, advance the write index.
            const float sqL = sL * sL;
            const float sqR = sR * sR;
            const size_t idx = static_cast<size_t> (rmsIdx_);
            rmsSumL_ += static_cast<double> (sqL) - static_cast<double> (rmsBufL_[idx]);
            rmsSumR_ += static_cast<double> (sqR) - static_cast<double> (rmsBufR_[idx]);
            rmsBufL_[idx] = sqL;
            rmsBufR_[idx] = sqR;
            if (rmsSumL_ < 0.0) rmsSumL_ = 0.0;
            if (rmsSumR_ < 0.0) rmsSumR_ = 0.0;
            ++rmsIdx_;
            if (rmsIdx_ >= rmsWindowSamples_)
                rmsIdx_ = 0;

            // Clip latch: any sample at or beyond ±1.0 latches the flag and
            // restarts the 3.0 s auto-clear counter.
            if (aL >= 1.0f || aR >= 1.0f)
            {
                clipLatch         = true;
                samplesSinceClip_ = 0;
            }
            else if (samplesSinceClip_ <= clipHoldSamples_)
            {
                ++samplesSinceClip_;
            }
        }

        // Auto-clear after 3.0 s of no clip-sample event.
        if (clipLatch && samplesSinceClip_ > clipHoldSamples_)
            clipLatch = false;

        const double invWin = 1.0 / static_cast<double> (rmsWindowSamples_);
        const float  rmsL   = static_cast<float> (std::sqrt (std::max (0.0, rmsSumL_ * invWin)));
        const float  rmsR   = static_cast<float> (std::sqrt (std::max (0.0, rmsSumR_ * invWin)));

        slot_->levelPeakL    .store (peakL_,     std::memory_order_relaxed);
        slot_->levelPeakR    .store (peakR_,     std::memory_order_relaxed);
        slot_->levelPeakHoldL.store (peakHoldL_, std::memory_order_relaxed);
        slot_->levelPeakHoldR.store (peakHoldR_, std::memory_order_relaxed);
        slot_->levelRmsL     .store (rmsL,       std::memory_order_relaxed);
        slot_->levelRmsR     .store (rmsR,       std::memory_order_relaxed);
        slot_->clip          .store (clipLatch,  std::memory_order_relaxed);
    }

    // Test/inspection helpers — read internal state (not published to atomics
    // until processBlock returns).
    float getPeakL()      const noexcept { return peakL_; }
    float getPeakR()      const noexcept { return peakR_; }
    float getPeakHoldL()  const noexcept { return peakHoldL_; }
    float getPeakHoldR()  const noexcept { return peakHoldR_; }
    int   getClipHoldSamples() const noexcept { return clipHoldSamples_; }
    int   getRmsWindowSamples() const noexcept { return rmsWindowSamples_; }

private:
    ChannelMeterSlots* slot_ { nullptr };

    double sampleRate_                = 44100.0;
    int    rmsWindowSamples_          = 1;
    int    clipHoldSamples_           = 1;
    float  peakDecayCoeff_            = 0.0f;
    float  peakHoldDecayPerSample_    = 0.0f;

    std::vector<float> rmsBufL_;
    std::vector<float> rmsBufR_;
    double rmsSumL_                   = 0.0;
    double rmsSumR_                   = 0.0;
    int    rmsIdx_                    = 0;

    float  peakL_                     = 0.0f;
    float  peakR_                     = 0.0f;
    float  peakHoldL_                 = 0.0f;
    float  peakHoldR_                 = 0.0f;

    int    samplesSinceClip_          = 1;
};
