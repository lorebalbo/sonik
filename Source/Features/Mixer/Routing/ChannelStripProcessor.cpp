#include "ChannelStripProcessor.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

void ChannelStripProcessor::prepareToPlay (double sampleRate, int blockSize, int numChannels)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = blockSize;
    numChannels_       = numChannels;

    // PRD-0054: initialise per-sample smoothers for gain and fader.
    // Ramp length = 7 ms = round(sampleRate * 0.007) samples.
    // setCurrentAndTargetValue(1.0) snaps to unity gain with no ramp at startup,
    // preventing an audible fade-in when the device first starts.
    gainSmoother_.reset  (sampleRate, 0.007);
    faderSmoother_.reset (sampleRate, 0.007);
    gainSmoother_.setCurrentAndTargetValue  (1.0f);
    faderSmoother_.setCurrentAndTargetValue (1.0f);

    // PRD-0055: prepare the 3-band EQ at the same sample rate. The EQ owns
    // its own smoothers + biquad state and resets them here.
    eq_.prepare (sampleRate);

    // PRD-0056: prepare the per-channel HPF/LPF filter. Pre-allocates the
    // SVF channel-state vectors so the audio thread never allocates.
    filter_.prepare (sampleRate);

    // PRD-0058: pre-allocate per-channel meter ring buffers.
    meter_.prepare (sampleRate);
}

void ChannelStripProcessor::setMeterSlot (ChannelMeterSlots* slot) noexcept
{
    meter_.setSlot (slot);
}

void ChannelStripProcessor::releaseResources()
{
    // Nothing to release; smoothers do not own heap memory.
}

void ChannelStripProcessor::process (const float*              inputL,
                                     const float*              inputR,
                                     float*                    outputL,
                                     float*                    outputR,
                                     int                       numSamples,
                                     const ChannelStripSnapshot& snapshot) noexcept
{
    // PRD-0054: set per-block smoother targets from the snapshot.
    // The snapshot is read from atomics by AudioEngine before calling process(),
    // so this is always called on the audio thread — no cross-thread access here.
    gainSmoother_.setTargetValue  (snapshot.gain);
    faderSmoother_.setTargetValue (snapshot.fader);

    // PRD-0055: push EQ targets (linear amplitudes + kill latch booleans).
    eq_.setTargets (snapshot.eqLow,  snapshot.killLow,
                    snapshot.eqMid,  snapshot.killMid,
                    snapshot.eqHigh, snapshot.killHigh);

    // PRD-0056: push the bipolar filter parameter; the filter classifies
    // bypass / HPF / LPF and seeds wet-ramp + SVF state internally.
    filter_.setTarget (snapshot.filter);

    // Per-sample processing chain (EPIC-0007 §1.2.1):
    //   input → [gain stage] → [EQ + per-band kills (PRD-0055)]
    //         → [filter (PRD-0056)] → [fader stage] → output
    for (int i = 0; i < numSamples; ++i)
    {
        const float gainMul  = gainSmoother_.getNextValue();
        const float faderMul = faderSmoother_.getNextValue();

        float sL = inputL[i] * gainMul;
        float sR = inputR[i] * gainMul;

        // EQ stage (low shelf → mid peak → high shelf, with per-band kill).
        eq_.processSample (sL, sR);

        // Filter stage (HPF / LPF / bypass, click-free).
        filter_.processSample (sL, sR);

        outputL[i] = sL * faderMul;
        outputR[i] = sR * faderMul;
    }

    // PRD-0058 §1.3.3: per-channel meter taps the post-fader, pre-A/B-bus
    // signal — i.e. the channel's contribution to the crossfader, ignoring
    // A/B assignment. Slot may be null in unit tests, which makes this a
    // no-op.
    meter_.processBlock (outputL, outputR, numSamples);
}
