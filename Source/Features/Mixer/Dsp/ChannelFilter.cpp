#include "ChannelFilter.h"
#include <cmath>
#include <algorithm>

//==============================================================================
// PRD-0056: ChannelFilter implementation.
//==============================================================================

void ChannelFilter::prepare (double sampleRate)
{
    sampleRate_     = sampleRate;
    nyquistClampHz_ = 0.45f * static_cast<float> (sampleRate);   // PRD §1.5.4 Nyquist clamp

    // Per-sample one-pole smoother coefficient in log-Hz space (PRD §1.5.3):
    //   alpha = 1 - exp(-1 / (sampleRate * tau_seconds))
    const float tauSec = kCutoffSmoothingTauMs * 1.0e-3f;
    cutoffAlpha_ = 1.0f - std::exp (-1.0f / (static_cast<float> (sampleRate) * tauSec));

    // Engage / disengage / side-cross ramp length in samples (PRD §1.5.5).
    const int engageRampSamples = std::max (1,
        static_cast<int> (std::round (sampleRate * (kEngageRampMs * 1.0e-3))));
    wetIncrement_ = 1.0f / static_cast<float> (engageRampSamples);

    // Pre-allocate SVF channel-state vectors. The PRD requires 2 channels
    // (stereo). No allocation happens after this point.
    juce::dsp::ProcessSpec spec {};
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = 512;                                  // unused by SVF state sizing
    spec.numChannels      = static_cast<juce::uint32> (kMaxChannels);
    svf_.prepare (spec);
    svf_.setResonance (kFilterQ);
    svf_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);  // default; mode set on engage
    svf_.setCutoffFrequency (kFMaxHz);
    svf_.reset();

    currentSide_  = Side::Bypass;
    targetLogHz_  = std::log (kFMaxHz);
    currentLogHz_ = targetLogHz_;
    wet_          = 0.0f;
    wetTarget_    = 0.0f;
}

void ChannelFilter::reset() noexcept
{
    svf_.reset();
    currentSide_  = Side::Bypass;
    targetLogHz_  = std::log (kFMaxHz);
    currentLogHz_ = targetLogHz_;
    wet_          = 0.0f;
    wetTarget_    = 0.0f;
}

float ChannelFilter::mapMagnitudeToCutoffHz (Side side, float magnitude) noexcept
{
    // PRD §1.5.4 exponential cutoff map.
    //   HPF: Hz = fMin * (fMax/fMin)^t, t in (0, 1].
    //   LPF: Hz = fMax * (fMin/fMax)^t = fMax / (fMax/fMin)^t.
    const float t     = juce::jlimit (0.0f, 1.0f, magnitude);
    const float ratio = kFMaxHz / kFMinHz;                  // 1000
    if (side == Side::HighPass)
        return kFMinHz * std::pow (ratio, t);
    return kFMaxHz / std::pow (ratio, t);                   // Side::LowPass
}

void ChannelFilter::setTarget (float bipolarFilter) noexcept
{
    // Defensive clamp — the state setter is supposed to snap inside the
    // detent (PRD §1.5.6), but the audio thread re-clamps so a bad write
    // can never destabilise the DSP.
    const float bipolar = juce::jlimit (-1.0f, 1.0f, bipolarFilter);

    Side  targetSide = Side::Bypass;
    float magnitude  = 0.0f;
    if (std::abs (bipolar) < kDetentEpsilon)
    {
        targetSide = Side::Bypass;
    }
    else if (bipolar > 0.0f)
    {
        targetSide = Side::HighPass;
        magnitude  = (bipolar - kDetentEpsilon) / (1.0f - kDetentEpsilon);
    }
    else
    {
        targetSide = Side::LowPass;
        magnitude  = (-bipolar - kDetentEpsilon) / (1.0f - kDetentEpsilon);
    }

    if (targetSide == Side::Bypass)
    {
        // Park: ramp wet → 0 over kEngageRampSamples. The current SVF state
        // keeps filtering during the tail so the ramp itself is click-free.
        // Once wet reaches 0 the processSample() short-circuits.
        wetTarget_ = 0.0f;
        return;
    }

    // Target is engaged (HPF or LPF).
    const float rawCutoff   = mapMagnitudeToCutoffHz (targetSide, magnitude);
    const float clampedHz   = juce::jlimit (1.0f, nyquistClampHz_, rawCutoff);
    const float newTargetLog = std::log (clampedHz);

    if (currentSide_ != targetSide)
    {
        // Engage from bypass, or side-cross (HPF ↔ LPF) without dwelling in
        // the detent. PRD §1.5.5: switch type, reset SVF state, snap the
        // log-Hz smoother to the new target, restart the wet ramp at 0.
        svf_.setType (targetSide == Side::HighPass
                          ? juce::dsp::StateVariableTPTFilterType::highpass
                          : juce::dsp::StateVariableTPTFilterType::lowpass);
        svf_.reset();
        currentLogHz_ = newTargetLog;
        wet_          = 0.0f;
        wetTarget_    = 1.0f;
        currentSide_  = targetSide;
    }
    else
    {
        // Same engaged side — let the per-sample smoother glide toward the
        // new target, and (re-)pull the wet ramp toward 1.0 in case we were
        // mid-disengage when the user re-engaged on the same side.
        wetTarget_ = 1.0f;
    }

    targetLogHz_ = newTargetLog;
}

void ChannelFilter::processSample (float& left, float& right) noexcept
{
    // PRD §1.4 / §1.3.8: when parked in the detent and the wet ramp has
    // fully reached zero, the audio-thread cost is exactly one branch.
    if (wet_ <= 0.0f && wetTarget_ <= 0.0f)
        return;

    // Advance the wet/dry ramp toward its target. Linear, click-free, never
    // overshoots.
    if (wet_ < wetTarget_)
        wet_ = std::min (wet_ + wetIncrement_, wetTarget_);
    else if (wet_ > wetTarget_)
        wet_ = std::max (wet_ - wetIncrement_, wetTarget_);

    // Advance the log-Hz smoother and push the new cutoff to the SVF.
    currentLogHz_ += cutoffAlpha_ * (targetLogHz_ - currentLogHz_);
    float cutoffHz = std::exp (currentLogHz_);
    if (cutoffHz > nyquistClampHz_) cutoffHz = nyquistClampHz_;
    if (cutoffHz < 1.0f)            cutoffHz = 1.0f;
    svf_.setCutoffFrequency (cutoffHz);

    const float wetL = svf_.processSample (0, left);
    const float wetR = svf_.processSample (1, right);

    const float w = wet_;
    const float d = 1.0f - w;
    left  = wetL * w + left  * d;
    right = wetR * w + right * d;

    // Finalise bypass: once the disengage ramp lands at zero, drop back to
    // the pure-bypass fast path on subsequent samples.
    if (wet_ <= 0.0f && wetTarget_ <= 0.0f)
        currentSide_ = Side::Bypass;
}
