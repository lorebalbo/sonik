#pragma once
//==============================================================================
// PRD-0054: ChannelStripProcessor — per-channel DSP pipeline stage.
//
// This is a plain, real-time-safe C++ class (NOT a juce::AudioProcessor, NOT
// a juce::dsp::ProcessorBase subclass). Each instance owns its own DSP state
// so the AudioEngine can hold one instance per channel.
//
// PRD-0054 adds functional gain and fader stages with 7 ms per-sample
// parameter smoothing via juce::SmoothedValue to eliminate zipper noise.
// The DSP chain order is: input → gain → (EQ/kills/filter pass-through) → fader → output.
//
// Audio-thread contract (enforced by CLAUDE.md):
//   - process() allocates NO memory.
//   - process() takes NO locks.
//   - process() performs NO I/O.
//==============================================================================

#include "ChannelStripSnapshot.h"
#include "ChannelMeter.h"
#include "../Dsp/ThreeBandEq.h"
#include "../Dsp/ChannelFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>

struct ChannelMeterSlots; // forward decl (defined in MixerMeterSnapshot.h)

class ChannelStripProcessor
{
public:
    ChannelStripProcessor() = default;

    /// Call on the message thread before the audio callback starts (or after a
    /// device-format change). Resets all internal DSP state so the first audio
    /// block is deterministic. Pre-allocates any internal scratch state that
    /// sibling PRDs will need (currently a no-op because the strip is a
    /// pass-through, but the contract is established here).
    ///
    /// @param sampleRate   Device sample rate in Hz.
    /// @param blockSize    Maximum block size in samples.
    /// @param numChannels  Number of audio channels (≥ 2 for stereo).
    void prepareToPlay (double sampleRate, int blockSize, int numChannels);

    /// Release any resources allocated during prepareToPlay. Safe to call on
    /// the message thread after the audio callback has stopped.
    void releaseResources();

    /// Process one block for this channel strip.
    ///
    /// PRD-0053 identity transform: copies inputL/R into outputL/R unchanged.
    /// The snapshot values are read but intentionally ignored in this PRD;
    /// they are wired in here so sibling PRDs can apply them without changing
    /// the call sites.
    ///
    /// @param inputL   Pointer to block-size floats — deck-output left channel.
    /// @param inputR   Pointer to block-size floats — deck-output right channel.
    /// @param outputL  Pointer to block-size floats — channel-strip output L.
    /// @param outputR  Pointer to block-size floats — channel-strip output R.
    /// @param numSamples  Number of samples in this block.
    /// @param snapshot    Parameter snapshot for this block (currently ignored).
    void process (const float* inputL,
                  const float* inputR,
                  float*       outputL,
                  float*       outputR,
                  int          numSamples,
                  const ChannelStripSnapshot& snapshot) noexcept;

    /// PRD-0058: wire this channel strip's meter slot. Pass nullptr to
    /// disable metering (e.g. SignalFlowTests).
    void setMeterSlot (ChannelMeterSlots* slot) noexcept;

    // Test hook.
    const ChannelMeter& getMeterForTest() const noexcept { return meter_; }

private:
    // Per-sample smoothers — live on the audio thread only, touched exclusively
    // inside prepareToPlay() (message thread, pre-callback) and process() (audio thread).
    // Default-initialised to 1.0 (unity gain / full fader) so the first block is silent-startup safe.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmoother_;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> faderSmoother_;

    // PRD-0055: 3-band EQ + per-band kill, slotted between gain and fader.
    ThreeBandEq eq_;

    // PRD-0056: per-channel HPF/LPF filter, slotted between EQ and fader.
    ChannelFilter filter_;

    // PRD-0058: per-channel post-fader metering.
    ChannelMeter meter_;

    // DSP state reserved for sibling PRDs (filter history, etc.).
    double currentSampleRate_ = 44100.0;
    int    currentBlockSize_  = 0;
    int    numChannels_       = 2;
};
