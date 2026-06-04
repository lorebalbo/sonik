#pragma once
//==============================================================================
// PRD-0055: ThreeBandEq — per-channel 3-band EQ (low shelf, mid peak, high
// shelf) with per-band "true kill" support.
//
// Topology (PRD §1.5.1 / §1.5.2 — DDM4000-style):
//   Low band  : RBJ low-shelf  centred at 100 Hz,   Q = 0.7
//   Mid band  : RBJ peak       centred at 1 kHz,    Q = 0.7
//   High band : RBJ high-shelf centred at 10 kHz,   Q = 0.7
//
// Series order (PRD §1.5.3): low → mid → high. Direct-Form I biquad per band,
// stereo state pair (L, R). At the design unity-gain operating point
// (linearGain == 1.0 for every band) all three biquads collapse to a
// mathematical identity (numerator polynomial == denominator polynomial in
// z⁻¹), so the EQ stage is bit-transparent when all knobs are at 0 dB.
//
// Smoothing (PRD §1.5.4):
//   - Per-band "knob" smoother (7 ms linear ramp) tracks the band's linear
//     amplitude target from the snapshot.
//   - Per-band "kill" smoother (8 ms linear ramp) tracks 0 (engaged) or 1
//     (released).
//   - Effective per-band linear gain = knobSmoother × killSmoother.
//   - The kill smoother does NOT write to the underlying knob value (PRD
//     §1.5.6); the snapshot's eq{Low,Mid,High} field is read independently.
//
// Coefficient update (PRD §1.5.5):
//   - Biquad coefficients are recomputed in chunks of 16 samples (≈0.36 ms at
//     44.1 kHz) using the smoother's current value. Each chunk is small
//     enough that the per-chunk gain delta during the 8 ms kill ramp stays
//     well below the audible-click threshold; large enough that the
//     coefficient-recompute cost is negligible.
//   - The smoothers themselves advance every sample so their internal target
//     is always tracked exactly.
//
// Audio-thread contract (CLAUDE.md):
//   - prepare() is the only path that may allocate (it does not, but is the
//     designated message-thread setup point).
//   - processSample() performs zero allocations, takes no locks, does no I/O.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

class ThreeBandEq
{
public:
    //==========================================================================
    // Fixed topology constants (PRD-0055 §1.5.1).
    //==========================================================================
    static constexpr float kLowFreqHz  = 100.0f;
    static constexpr float kMidFreqHz  = 1000.0f;
    static constexpr float kHighFreqHz = 10000.0f;
    static constexpr float kBandQ      = 0.7f;

    static constexpr float kKnobRampSeconds  = 0.007f;  // PRD §1.5: 7 ms
    static constexpr float kKillRampSeconds  = 0.008f;  // PRD §1.5.4: 8 ms

    // Recompute biquad coefficients every N samples. 16 = ~0.36 ms at 44.1 kHz;
    // empirically click-free across the 8 ms kill ramp.
    static constexpr int   kCoeffUpdateChunk = 16;

    // Floor for the linear gain fed into the RBJ formulas. The peak filter
    // (mid band) has a pole that approaches the unit circle as A → 0, which
    // creates extremely slow transients (hundreds of ms) for very small A.
    // The shelf filters do not exhibit this: their poles stay well inside
    // the unit circle even at A = 1e-3.
    //
    // We therefore use two band-specific floors:
    //   - Mid (peak):   1e-3 → A = sqrt(1e-3) ≈ 0.0316 → centre |H| = A² = -60 dB.
    //                   Pole ≈ 0.997, time-constant ≈ 7.5 ms — fast settle.
    //   - Shelves:      1e-6 → A = 1e-3 → deep-stopband asymptote |H| = A = -60 dB.
    //                   Shelf poles stay well inside the unit circle.
    // Both kept above float denormals.
    static constexpr float kMinLinearForMidCoeff   = 1.0e-3f;
    static constexpr float kMinLinearForShelfCoeff = 1.0e-6f;

    ThreeBandEq() = default;

    /// Message-thread setup. Computes frequency-domain constants for the
    /// fixed band centres at the given sample rate and primes the smoothers
    /// to the unity (flat) operating point.
    void prepare (double sampleRate);

    /// Re-seed smoothers and filter state so the next processed sample sees
    /// no residual history. Safe to call on the audio thread (no allocation).
    void reset() noexcept;

    /// Push the per-block snapshot targets for all three bands. Linear gain
    /// values come straight from MixerAtomicSnapshot (PRD-0052) which stores
    /// them in linear form already (dB→linear conversion happens on the
    /// message thread in MixerStateBridge).
    void setTargets (float lowLin,  bool killLow,
                     float midLin,  bool killMid,
                     float highLin, bool killHigh) noexcept;

    /// Apply the three biquads in series (low → mid → high) to one stereo
    /// sample in place. Advances internal smoothers and updates coefficients
    /// every kCoeffUpdateChunk samples.
    void processSample (float& left, float& right) noexcept;

private:
    enum BandIndex : int { Low = 0, Mid = 1, High = 2, NumBands = 3 };

    struct Coeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
    struct State  { float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f; };

    static float biquadProcess (const Coeffs& c, State& s, float x) noexcept;
    void recomputeBand (int band, float linearGain) noexcept;

    double sampleRate_ = 44100.0;

    // Per-band frequency-domain constants (fixed for a given band + sample rate).
    float cos_w0_[NumBands] {};
    float alpha_  [NumBands] {};

    // Per-band biquad coefficients (already normalised by a0) and stereo state.
    Coeffs coeffs_[NumBands] {};
    State  stateL_[NumBands] {};
    State  stateR_[NumBands] {};

    // Linear gain currently baked into coeffs_ — used to skip recompute when
    // unchanged at chunk boundaries.
    float  coeffLinear_[NumBands] { 1.0f, 1.0f, 1.0f };

    // Per-band smoothers, composed multiplicatively (PRD §1.5.4).
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> knobSmoother_[NumBands];
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> killSmoother_[NumBands];

    int chunkCounter_ = 0;
};
