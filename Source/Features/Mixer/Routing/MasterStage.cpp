#include "MasterStage.h"

void MasterStage::prepareToPlay (double sampleRate, int /*blockSize*/, int /*numChannels*/) noexcept
{
    // PRD-0058 §1.3.1: 7 ms ramp matches channel gain/fader smoothers and is
    // short enough to be inaudible while still suppressing zipper noise on
    // continuous fader motion.
    gainSmoother_.reset (sampleRate, 0.007);
    gainSmoother_.setCurrentAndTargetValue (1.0f);

    meter_.prepare (sampleRate);
}

void MasterStage::setMeterSlot (ChannelMeterSlots* slot) noexcept
{
    meter_.setSlot (slot);
}

void MasterStage::process (const float*          masterL,
                            const float*          masterR,
                            float*                outputL,
                            float*                outputR,
                            int                   numSamples,
                            const MasterSnapshot& snapshot) noexcept
{
    // Push the latest target. The smoother snapshots the change once per
    // block; per-sample ramping happens below.
    gainSmoother_.setTargetValue (snapshot.masterGain);

    if (gainSmoother_.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gainSmoother_.getNextValue();
            outputL[i] = masterL[i] * g;
            outputR[i] = masterR[i] * g;
        }
    }
    else
    {
        // Fast path: not smoothing → gain is constant for this block. When
        // gain == 1.0 (default master), this branch produces a bit-exact
        // identity copy, preserving the PRD-0053 SignalFlowTests invariant.
        const float g = gainSmoother_.getCurrentValue();
        if (g == 1.0f)
        {
            juce::FloatVectorOperations::copy (outputL, masterL, numSamples);
            juce::FloatVectorOperations::copy (outputR, masterR, numSamples);
        }
        else
        {
            juce::FloatVectorOperations::multiply (outputL, masterL, g, numSamples);
            juce::FloatVectorOperations::multiply (outputR, masterR, g, numSamples);
        }
    }

    // Master metering taps the post-gain, pre-hard-clip signal (PRD-0058
    // §1.5.3). The PRD-0002 hard-clip safety net runs in AudioEngine AFTER
    // this stage returns.
    meter_.processBlock (outputL, outputR, numSamples);
}
