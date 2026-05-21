#pragma once
//==============================================================================
// PRD-0053 + PRD-0057: CrossfaderStage — mixes bus A and bus B into the
// stereo master scratch buffer with a click-free per-sample curve law.
//
// Audio-thread contract: zero allocation, zero locks, zero I/O.
//
// PRD-0057 §1.4 / §1.5.6 implementation notes:
//
//   1. Position smoothing (zipper suppression).
//      The raw `snapshot.crossfader` value (range [0, 1], updated on the
//      message thread via std::atomic) is fed sample-by-sample into a
//      one-pole IIR smoother:
//          smoothed[n] = smoothed[n-1] + α * (target - smoothed[n-1])
//      with α chosen so the smoother reaches 90% of a step input in 7 ms:
//          α = 1 - exp(-ln(10) / (0.007 * sampleRate))
//      α is recomputed only when prepareToPlay() observes a sample-rate
//      change. The first process() call after prepareToPlay snaps
//      `smoothed` to the current target, so the smoother does not need a
//      warm-up block to reach the correct steady-state value at startup.
//
//   2. Curve law (applied per sample to the smoothed position p):
//
//        Smooth (equal-power, PRD-0057 §1.4 AC):
//           gainA = cos(p · π / 2)
//           gainB = sin(p · π / 2)
//           Property: gainA² + gainB² == 1 for all p ∈ [0,1].
//
//        Sharp (piecewise-linear hard cut, §1.4 AC):
//           Centred at p = 0.5 with transition half-width w = 0.02.
//             p ≤ 0.5 - w           : gainA = 1, gainB = 0
//             0.5 - w < p < 0.5 + w : linear cross-fade across 2w
//             p ≥ 0.5 + w           : gainA = 0, gainB = 1
//
//        Linear (internal/test only — never exposed via the ValueTree):
//           gainA = 1 - p
//           gainB = p
//           Used by SignalFlowTests to keep the pre-PRD-0057 routing-
//           identity invariant: when assignA == assignB == true on every
//           contributing channel, busA == busB == channelSum, so master ==
//           (1-p)·channelSum + p·channelSum == channelSum.
//
//   3. Output: masterL[n] = gainA(p[n]) * busAL[n] + gainB(p[n]) * busBL[n]
//             masterR[n] = gainA(p[n]) * busAR[n] + gainB(p[n]) * busBR[n]
//      Switching curves at runtime affects only the gain function applied
//      to `smoothed`; `smoothed` itself is preserved across curve changes,
//      so the curve-toggle gesture is click-free (§1.4 AC).
//==============================================================================

#include "CrossfaderSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

class CrossfaderStage
{
public:
    /// Called on the message thread before the audio callback. Recomputes
    /// the smoother coefficient for the new sample rate and arms the
    /// "snap on first sample" flag so the smoother does not glide from a
    /// stale start value.
    void prepareToPlay (double sampleRate, int /*blockSize*/, int /*numChannels*/) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        // α = 1 - exp(-ln(10) / (timeTo90 * sampleRate)), timeTo90 = 7 ms.
        constexpr double kTimeTo90Seconds = 0.007;
        const double exponent = -2.302585092994046 /* ln(10) */
                                 / (kTimeTo90Seconds * sr);
        alpha    = static_cast<float> (1.0 - std::exp (exponent));
        snapNext = true;
    }

    void releaseResources() noexcept
    {
        snapNext = true;
    }

    /// Mix bus A and bus B into the master scratch buffer.
    void process (const float*              busAL,
                  const float*              busAR,
                  const float*              busBL,
                  const float*              busBR,
                  float*                    masterL,
                  float*                    masterR,
                  int                       numSamples,
                  const CrossfaderSnapshot& snapshot) noexcept
    {
        // Clamp once per block — the snapshot value is produced by the
        // message thread which is supposed to clamp, but the audio thread
        // never trusts the producer.
        float target = snapshot.crossfader;
        if (target < 0.0f)      target = 0.0f;
        else if (target > 1.0f) target = 1.0f;

        if (snapNext)
        {
            smoothedPos = target;
            snapNext    = false;
        }

        const CrossfaderCurve curve = snapshot.curve;
        const float a = alpha;

        float p = smoothedPos;
        for (int n = 0; n < numSamples; ++n)
        {
            // One-pole smoother.
            p += a * (target - p);

            float gA;
            float gB;
            computeGains (curve, p, gA, gB);

            masterL[n] = gA * busAL[n] + gB * busBL[n];
            masterR[n] = gA * busAR[n] + gB * busBR[n];
        }
        smoothedPos = p;
    }

private:
    static void computeGains (CrossfaderCurve curve,
                              float           p,
                              float&          gainA,
                              float&          gainB) noexcept
    {
        switch (curve)
        {
            case CrossfaderCurve::Sharp:
            {
                // Piecewise-linear hard cut around 0.5, half-width 0.02.
                constexpr float w = 0.02f;
                constexpr float lo = 0.5f - w;
                constexpr float hi = 0.5f + w;
                if (p <= lo)        { gainA = 1.0f; gainB = 0.0f; }
                else if (p >= hi)   { gainA = 0.0f; gainB = 1.0f; }
                else
                {
                    constexpr float invSpan = 1.0f / (2.0f * w);
                    gainB = (p - lo) * invSpan;
                    gainA = (hi - p) * invSpan;
                }
                return;
            }
            case CrossfaderCurve::Linear:
            {
                gainA = 1.0f - p;
                gainB = p;
                return;
            }
            case CrossfaderCurve::Smooth:
            default:
            {
                constexpr float kHalfPi = 1.57079632679489661923f;
                const float theta = p * kHalfPi;
                gainA = std::cos (theta);
                gainB = std::sin (theta);
                return;
            }
        }
    }

    float alpha       { 0.0f };   ///< Smoother coefficient; set in prepareToPlay.
    float smoothedPos { 0.5f };   ///< Current smoothed position.
    bool  snapNext    { true };   ///< Snap smoother to target on next sample.
};
