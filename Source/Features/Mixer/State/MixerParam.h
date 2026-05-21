#pragma once
//==============================================================================
// PRD-0052: MixerParam — normalised ↔ native value conversions.
//
// Provides the ONLY authoritative conversion between:
//   - The dB values stored in the mixer ValueTree (human-readable form)
//   - The normalised [0,1] form used by the MIDI inbound router (PRD-0044)
//   - The linear values stored in MixerAtomicSnapshot (audio-thread form)
//
// All functions are pure, constexpr-friendly, and free of allocation / I/O.
//==============================================================================

#include <cmath>
#include <algorithm>

namespace MixerParam
{
    //==========================================================================
    // dB floor constant.  Any dB value ≤ kMinDb is treated as -inf (silence).
    //==========================================================================
    inline constexpr float kMinDb     = -60.0f;
    inline constexpr float kMaxGainDb =  12.0f;   // channel gain and master gain ceiling
    inline constexpr float kMaxEqDb   =  12.0f;   // EQ band ceiling (per band)

    //==========================================================================
    // PRD-0056: filter knob detent epsilon. Any write to
    // `mixer.channel.{A,B,C,D}.filter` whose magnitude is strictly below this
    // value is snapped to exactly 0.0f at the state-setter boundary
    // (MixerStateBridge), so the audio thread observes either
    //   filter == 0.0f   (full bypass)
    //   |filter| >= kFilterDetentEpsilon  (engaged on one side)
    // and never an in-between value.
    //==========================================================================
    inline constexpr float kFilterDetentEpsilon = 0.02f;

    inline float snapFilterDetent (float bipolar) noexcept
    {
        const float clamped = std::max (-1.0f, std::min (1.0f, bipolar));
        return std::abs (clamped) < kFilterDetentEpsilon ? 0.0f : clamped;
    }

    //==========================================================================
    // Basic dB ↔ linear conversions.
    // Matches PRD-0010 GainKnobComponent exactly for binary compatibility.
    //==========================================================================
    inline float dbToLinear (float dB) noexcept
    {
        if (dB <= kMinDb)
            return 0.0f;
        return std::pow (10.0f, dB / 20.0f);
    }

    inline float linearToDb (float linear) noexcept
    {
        if (linear <= 0.0f)
            return kMinDb;
        float db = 20.0f * std::log10 (linear);
        return std::max (kMinDb, db);
    }

    //==========================================================================
    // PRD-0054 clamping helpers — applied on the message thread (in
    // MixerStateBridge) before values reach the audio-thread atomics, so the
    // audio thread never sees an out-of-range gain or fader.
    //==========================================================================
    inline float clampGainDb (float dB) noexcept
    {
        return std::max (kMinDb, std::min (kMaxGainDb, dB));
    }

    inline float clampFader (float value) noexcept
    {
        return std::max (0.0f, std::min (1.0f, value));
    }

    //==========================================================================
    // Channel/master gain normalised mapping.
    //   Range dB: [kMinDb, kMaxGainDb] = [-60, +12]
    //   norm 0.0  ↔  kMinDb  (-60 dB, treated as -inf)
    //   norm 0.5  ↔  0.0 dB  (12 o'clock, unity gain)
    //   norm 1.0  ↔  +12 dB
    //
    // This is a piecewise-linear mapping in dB space, identical to
    // GainKnobComponent in PRD-0010.
    //==========================================================================
    inline float gainDbToNormalised (float dB) noexcept
    {
        float clamped = std::max (kMinDb, std::min (kMaxGainDb, dB));
        if (clamped <= 0.0f)
            // map [kMinDb, 0] → [0, 0.5]
            return ((clamped - kMinDb) / (0.0f - kMinDb)) * 0.5f;
        else
            // map [0, kMaxGainDb] → [0.5, 1]
            return 0.5f + (clamped / kMaxGainDb) * 0.5f;
    }

    inline float normalisedToGainDb (float norm) noexcept
    {
        float n = std::max (0.0f, std::min (1.0f, norm));
        if (n <= 0.5f)
            return kMinDb + (n / 0.5f) * (0.0f - kMinDb);  // -60 + n*120
        else
            return ((n - 0.5f) / 0.5f) * kMaxGainDb;       // (n-0.5)*24
    }

    //==========================================================================
    // EQ band normalised mapping.
    //   Range dB: [kMinDb, kMaxEqDb] = [-60, +12]
    //   norm 0.0  ↔  kMinDb  (-60 dB, full cut)
    //   norm 0.5  ↔  0.0 dB  (12 o'clock, flat)
    //   norm 1.0  ↔  +12 dB
    //==========================================================================
    inline float eqDbToNormalised (float dB) noexcept
    {
        float clamped = std::max (kMinDb, std::min (kMaxEqDb, dB));
        if (clamped <= 0.0f)
            return ((clamped - kMinDb) / (0.0f - kMinDb)) * 0.5f;
        else
            return 0.5f + (clamped / kMaxEqDb) * 0.5f;
    }

    inline float normalisedToEqDb (float norm) noexcept
    {
        float n = std::max (0.0f, std::min (1.0f, norm));
        if (n <= 0.5f)
            return kMinDb + (n / 0.5f) * (0.0f - kMinDb);
        else
            return ((n - 0.5f) / 0.5f) * kMaxEqDb;
    }

    //==========================================================================
    // Filter normalised mapping.
    //   Stored value: bipolar float [-1, +1] where:
    //     0.0  = bypass (center detent, 12 o'clock)
    //    +1.0  = full HPF (clockwise, 5 o'clock)
    //    -1.0  = full LPF (counter-clockwise, 7 o'clock)
    //
    //   Normalised form [0, 1]:
    //     0.0  ↔ bipolar -1.0
    //     0.5  ↔ bipolar  0.0  (bypass)
    //     1.0  ↔ bipolar +1.0
    //
    //   Center-detent snap: ±0.02 around bipolar 0.0 snaps to 0.0 at the
    //   UI layer (PRD-0059); the ValueTree and atomics store the exact value.
    //==========================================================================
    inline float filterBipolarToNormalised (float bipolar) noexcept
    {
        float clamped = std::max (-1.0f, std::min (1.0f, bipolar));
        return (clamped + 1.0f) * 0.5f;
    }

    inline float normalisedToFilterBipolar (float norm) noexcept
    {
        float n = std::max (0.0f, std::min (1.0f, norm));
        return n * 2.0f - 1.0f;
    }

    //==========================================================================
    // Channel fader normalised mapping — identity: stored as [0,1] directly.
    //==========================================================================
    inline float faderToNormalised (float value) noexcept
    {
        return std::max (0.0f, std::min (1.0f, value));
    }

    inline float normalisedToFader (float norm) noexcept
    {
        return std::max (0.0f, std::min (1.0f, norm));
    }

    //==========================================================================
    // Crossfader normalised mapping — identity: stored as [0,1] directly.
    //==========================================================================
    inline float crossfaderToNormalised (float value) noexcept
    {
        return std::max (0.0f, std::min (1.0f, value));
    }

    inline float normalisedToCrossfader (float norm) noexcept
    {
        return std::max (0.0f, std::min (1.0f, norm));
    }

    //==========================================================================
    // Bool normalised form: false → 0.0, true → 1.0.
    //==========================================================================
    inline float boolToNormalised (bool value) noexcept  { return value ? 1.0f : 0.0f; }
    inline bool  normalisedToBool (float norm)  noexcept { return norm >= 0.5f; }

} // namespace MixerParam
