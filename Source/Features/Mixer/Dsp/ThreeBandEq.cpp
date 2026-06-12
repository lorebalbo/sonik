#include "ThreeBandEq.h"
#include <cmath>

//==============================================================================
// PRD-0055: ThreeBandEq implementation.
//
// RBJ Audio EQ Cookbook formulas — peak (parametric), low shelf, high shelf.
// At linearGain == 1.0 each filter's numerator polynomial equals its
// denominator polynomial in z⁻¹, so y[n] == x[n] (verified mathematically and
// by the ThreeBandEqTests unity-passthrough test).
//==============================================================================

void ThreeBandEq::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    const float bandHz[NumBands] = { kLowFreqHz, kMidFreqHz, kHighFreqHz };
    for (int b = 0; b < NumBands; ++b)
    {
        const float w0     = juce::MathConstants<float>::twoPi
                             * bandHz[b] / static_cast<float> (sampleRate);
        cos_w0_[b]         = std::cos (w0);
        const float sin_w0 = std::sin (w0);
        alpha_[b]          = sin_w0 / (2.0f * kBandQ);

        knobSmoother_[b].reset (sampleRate, static_cast<double> (kKnobRampSeconds));
        killSmoother_[b].reset (sampleRate, static_cast<double> (kKillRampSeconds));
        knobSmoother_[b].setCurrentAndTargetValue (1.0f);
        killSmoother_[b].setCurrentAndTargetValue (1.0f);

        coeffLinear_[b] = 1.0f;
        recomputeBand (b, 1.0f);
        stateL_[b] = State{};
        stateR_[b] = State{};
    }
    chunkCounter_ = 0;
}

void ThreeBandEq::reset() noexcept
{
    for (int b = 0; b < NumBands; ++b)
    {
        stateL_[b] = State{};
        stateR_[b] = State{};
    }
    chunkCounter_ = 0;
}

void ThreeBandEq::setTargets (float lowLin,  bool killLow,
                              float midLin,  bool killMid,
                              float highLin, bool killHigh) noexcept
{
    knobSmoother_[Low ].setTargetValue (lowLin);
    knobSmoother_[Mid ].setTargetValue (midLin);
    knobSmoother_[High].setTargetValue (highLin);

    killSmoother_[Low ].setTargetValue (killLow  ? 0.0f : 1.0f);
    killSmoother_[Mid ].setTargetValue (killMid  ? 0.0f : 1.0f);
    killSmoother_[High].setTargetValue (killHigh ? 0.0f : 1.0f);
}

void ThreeBandEq::processSample (float& left, float& right) noexcept
{
    // Refresh coefficients at the start of each kCoeffUpdateChunk-sized chunk.
    // Reading getCurrentValue() does NOT advance the smoother — it reports the
    // value the smoother is currently sitting at, which is what the just-
    // -processed sample saw.
    if (chunkCounter_ == 0)
    {
        for (int b = 0; b < NumBands; ++b)
        {
            const float effective = knobSmoother_[b].getCurrentValue()
                                  * killSmoother_[b].getCurrentValue();
            if (! juce::exactlyEqual (effective, coeffLinear_[b]))
            {
                recomputeBand (b, effective);
                coeffLinear_[b] = effective;
            }
        }
    }
    if (++chunkCounter_ >= kCoeffUpdateChunk)
        chunkCounter_ = 0;

    // Advance both smoothers by one sample so their internal target tracking
    // stays accurate. We ignore the returned value because coefficient
    // updates are batched to chunk boundaries.
    for (int b = 0; b < NumBands; ++b)
    {
        knobSmoother_[b].getNextValue();
        killSmoother_[b].getNextValue();
    }

    // Apply biquads in series — PRD §1.5.3 fixed order: low → mid → high.
    for (int b = 0; b < NumBands; ++b)
    {
        left  = biquadProcess (coeffs_[b], stateL_[b], left);
        right = biquadProcess (coeffs_[b], stateR_[b], right);
    }
}

float ThreeBandEq::biquadProcess (const Coeffs& c, State& s, float x) noexcept
{
    // Direct Form I biquad. Coefficients are already normalised by a0 so a0
    // does not appear here.
    const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                  - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

void ThreeBandEq::recomputeBand (int band, float linearGain) noexcept
{
    // Clamp linear gain to the documented per-band floor so the RBJ formulas
    // stay numerically well-conditioned and biquad poles stay away from the
    // unit circle even at deep cuts (true kill).
    const float bandFloor = (band == Mid) ? kMinLinearForMidCoeff
                                          : kMinLinearForShelfCoeff;
    const float lin = linearGain < bandFloor ? bandFloor : linearGain;

    // Unity-gain shortcut: at linearGain == 1.0 every RBJ filter is
    // mathematically the identity (numerator polynomial == denominator
    // polynomial). The closed-form coefficients above evaluate to that
    // identity only modulo the round-trip `(1+α) * (1/(1+α))` precision
    // loss, which leaks ~1 ULP per sample into the downstream pipeline and
    // would break the bit-exact direct-sum equivalence asserted by
    // SignalFlowTests. Forcing the exact identity here avoids that drift
    // without adding any per-sample branches in the hot path.
    Coeffs& coef = coeffs_[band];
    if (lin == 1.0f)
    {
        coef.b0 = 1.0f;
        coef.b1 = 0.0f;
        coef.b2 = 0.0f;
        coef.a1 = 0.0f;
        coef.a2 = 0.0f;
        return;
    }

    // RBJ: A = sqrt(linear) for both peak and shelf filters.
    const float A   = std::sqrt (lin);
    const float c   = cos_w0_[band];
    const float al  = alpha_  [band];

    if (band == Mid)
    {
        // Peak (parametric) — RBJ cookbook.
        const float a0  = 1.0f + al / A;
        const float inv = 1.0f / a0;
        coef.b0 = (1.0f + al * A) * inv;
        coef.b1 = (-2.0f * c)     * inv;
        coef.b2 = (1.0f - al * A) * inv;
        coef.a1 = (-2.0f * c)     * inv;
        coef.a2 = (1.0f - al / A) * inv;
    }
    else if (band == Low)
    {
        // Low shelf — RBJ cookbook.
        const float sqrtA           = std::sqrt (A);
        const float twoSqrtA_alpha  = 2.0f * sqrtA * al;
        const float Ap1             = A + 1.0f;
        const float Am1             = A - 1.0f;
        const float a0              = Ap1 + Am1 * c + twoSqrtA_alpha;
        const float inv             = 1.0f / a0;
        coef.b0 =        A * (Ap1 - Am1 * c + twoSqrtA_alpha) * inv;
        coef.b1 = 2.0f * A * (Am1 - Ap1 * c)                  * inv;
        coef.b2 =        A * (Ap1 - Am1 * c - twoSqrtA_alpha) * inv;
        coef.a1 = -2.0f *   (Am1 + Ap1 * c)                   * inv;
        coef.a2 =          (Ap1 + Am1 * c - twoSqrtA_alpha)   * inv;
    }
    else // High shelf
    {
        const float sqrtA           = std::sqrt (A);
        const float twoSqrtA_alpha  = 2.0f * sqrtA * al;
        const float Ap1             = A + 1.0f;
        const float Am1             = A - 1.0f;
        const float a0              = Ap1 - Am1 * c + twoSqrtA_alpha;
        const float inv             = 1.0f / a0;
        coef.b0 =        A * (Ap1 + Am1 * c + twoSqrtA_alpha) * inv;
        coef.b1 = -2.0f * A * (Am1 + Ap1 * c)                 * inv;
        coef.b2 =        A * (Ap1 + Am1 * c - twoSqrtA_alpha) * inv;
        coef.a1 = 2.0f *    (Am1 - Ap1 * c)                   * inv;
        coef.a2 =          (Ap1 - Am1 * c - twoSqrtA_alpha)   * inv;
    }
}
