#pragma once
//==============================================================================
// PRD-0058: MasterStage — applies smoothed master gain and master metering
// to the crossfader bus, then writes to the output bus.
//
// DSP chain (per sample):
//   output = masterScratch * smoothed(masterGain)
// The metering block then observes outputL/outputR and publishes
// peak/peakHold/RMS/clip to MixerMeterSnapshot::master.
//
// The PRD-0002 hard-clip safety net continues to run on the output bus
// AFTER this stage in AudioEngine, so the master meter taps the
// pre-hard-clip signal (PRD-0058 §1.5.3).
//
// Audio-thread contract (CLAUDE.md): no allocation, no locks, no I/O.
//==============================================================================

#include "ChannelMeter.h"
#include "MasterSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>

struct ChannelMeterSlots; // defined in MixerMeterSnapshot.h

class MasterStage
{
public:
    /// Call on the message thread before the audio callback. Pre-allocates
    /// the meter ring buffers and snaps the gain smoother to unity.
    void prepareToPlay (double sampleRate, int blockSize, int numChannels) noexcept;

    /// Release resources. The smoother and meter ring buffers continue to
    /// own their storage; this is a no-op aside from documenting the
    /// lifecycle contract.
    void releaseResources() noexcept {}

    /// PRD-0058: wire the master meter snapshot slot. May be nullptr in
    /// tests that do not exercise metering.
    void setMeterSlot (ChannelMeterSlots* slot) noexcept;

    /// Apply smoothed master gain to `masterL`/`masterR` into `outputL`/
    /// `outputR`, then update master metering.
    void process (const float*          masterL,
                  const float*          masterR,
                  float*                outputL,
                  float*                outputR,
                  int                   numSamples,
                  const MasterSnapshot& snapshot) noexcept;

    // Test hooks (no audio-thread synchronisation needed; called from
    // single-threaded unit tests after process() returns).
    const ChannelMeter& getMeterForTest() const noexcept { return meter_; }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmoother_;
    ChannelMeter meter_;
};
