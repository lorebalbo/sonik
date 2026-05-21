#pragma once
//==============================================================================
// PRD-0056: ChannelFilter — per-channel HPF/LPF filter knob DSP stage.
//
// One instance per mixer channel. Drives a JUCE TPT state-variable filter
// (`juce::dsp::StateVariableTPTFilter<float>`) from the bipolar
// `mixer.channel.{A,B,C,D}.filter` parameter (range [-1, +1], detent at 0):
//
//   - |filter| < kDetentEpsilon : full bypass (output == input, single
//                                 compare-and-jump on the hot path).
//   - filter > +kDetentEpsilon  : high-pass, cutoff = 20 Hz .. 20 kHz
//                                 (exponential, magnitude-driven).
//   - filter < -kDetentEpsilon  : low-pass,  cutoff = 20 kHz .. 20 Hz
//                                 (exponential, mirror of HPF side).
//
// Fixed Butterworth Q (PRD §1.5.1, `kFilterQ = 0.7071`). Cutoff is smoothed
// per-sample in log-Hz space with a 10 ms one-pole exponential filter
// (PRD §1.5.3). Engage / disengage / side-cross transitions use a 5 ms
// linear wet/dry ramp combined with an SVF integrator reset (PRD §1.5.5) so
// the user never hears a click when entering, leaving, or crossing the
// detent.
//
// Audio-thread contract (AGENTS.md):
//   - prepare() is the only path that may allocate (it pre-sizes the SVF
//     state vectors); the audio thread never allocates.
//   - processSample() takes no locks, performs no I/O, and only touches
//     pre-allocated state.
//==============================================================================

#include "../State/MixerParam.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

class ChannelFilter
{
public:
    //==========================================================================
    // PRD-0056 fixed constants (§1.5.1 / §1.5.2 / §1.5.3 / §1.5.4 / §1.5.5).
    //==========================================================================
    static constexpr float kDetentEpsilon        = MixerParam::kFilterDetentEpsilon;
    static constexpr float kFilterQ              = 0.7071f;
    static constexpr float kFMinHz               = 20.0f;
    static constexpr float kFMaxHz              = 20000.0f;
    static constexpr float kCutoffSmoothingTauMs = 10.0f;
    static constexpr float kEngageRampMs         = 5.0f;

    enum class Side : int { Bypass = 0, HighPass = +1, LowPass = -1 };

    ChannelFilter() = default;

    /// Message-thread setup. Pre-allocates the SVF channel-state vectors and
    /// resets the filter to bypass. Safe to call repeatedly across device
    /// reconfigurations.
    void prepare (double sampleRate);

    /// Force the filter into a clean bypass state. Safe to call from the
    /// audio thread (no allocation).
    void reset() noexcept;

    /// Push the per-block bipolar filter parameter. Classifies the target
    /// side, computes the target cutoff in log-Hz, and seeds the wet/dry
    /// ramp / SVF state when the side changes.
    void setTarget (float bipolarFilter) noexcept;

    /// Process one stereo sample in place. When parked in the detent (and
    /// the wet ramp has reached zero) this is a single compare-and-return:
    /// no math, no SVF call, no smoothing advance.
    void processSample (float& left, float& right) noexcept;

    // ---- introspection (test-only; not used on the audio thread) -----------
    Side  currentSide() const noexcept { return currentSide_; }
    float currentCutoffHz() const noexcept { return std::exp (currentLogHz_); }
    float currentWet() const noexcept { return wet_; }

private:
    static constexpr int   kMaxChannels = 2;

    static float mapMagnitudeToCutoffHz (Side side, float magnitude) noexcept;

    double sampleRate_        = 44100.0;
    float  nyquistClampHz_    = 22050.0f * 0.45f / 0.5f;   // = 0.45 * sampleRate
    float  cutoffAlpha_       = 0.0f;     // per-sample one-pole alpha (log-Hz)
    float  wetIncrement_      = 0.0f;     // 1.0 / engageRampSamples

    Side   currentSide_       = Side::Bypass;
    float  targetLogHz_       = std::log (kFMaxHz);
    float  currentLogHz_      = std::log (kFMaxHz);

    float  wet_               = 0.0f;
    float  wetTarget_         = 0.0f;

    juce::dsp::StateVariableTPTFilter<float> svf_;
};
