#pragma once
//==============================================================================
// PRD-0080: SummingAudioFormatReader — presents N source AudioFormatReaders as
// a single stereo reader whose samples are the per-sample sum of the inputs.
//
// Used to materialise the DAW "Instrumental" lane, which is the sum of the
// drums + bass + other stem files (EPIC-0002 stem cache). All inputs are
// assumed to share the same sample rate (they come from one separation pass).
//
// MESSAGE / BACKGROUND THREAD ONLY — constructed and read off the audio thread
// by ClipStreamer's reader loop. Never touched by the audio thread.
//==============================================================================

#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace Daw
{

class SummingAudioFormatReader final : public juce::AudioFormatReader
{
public:
    explicit SummingAudioFormatReader (std::vector<std::unique_ptr<juce::AudioFormatReader>> readers)
        : juce::AudioFormatReader (nullptr, "Summing")
        , readers_ (std::move (readers))
    {
        // Derive metadata from the first valid reader; present as stereo float.
        double  rate   = 44100.0;
        int64_t length = 0;
        for (auto& r : readers_)
        {
            if (r == nullptr)
                continue;
            if (r->sampleRate > 0.0)
                rate = r->sampleRate;
            length = juce::jmax (length, (int64_t) r->lengthInSamples);
        }

        sampleRate            = rate;
        bitsPerSample         = 32;
        usesFloatingPointData = true;
        numChannels           = 2;
        lengthInSamples       = length;
    }

    bool readSamples (int* const* destChannels,
                      int          numDestChannels,
                      int          startOffsetInDestBuffer,
                      juce::int64  startSampleInFile,
                      int          numSamples) override
    {
        if (numSamples <= 0)
            return true;

        ensureScratch (numSamples);

        accum_.clear (0, numSamples);

        for (auto& r : readers_)
        {
            if (r == nullptr)
                continue;

            temp_.clear (0, numSamples);
            r->read (&temp_, 0, numSamples, startSampleInFile, true, true);

            // Mono stem → duplicate to both channels before summing.
            if (r->numChannels == 1)
                temp_.copyFrom (1, 0, temp_, 0, 0, numSamples);

            accum_.addFrom (0, 0, temp_, 0, 0, numSamples);
            accum_.addFrom (1, 0, temp_, 1, 0, numSamples);
        }

        // Write the summed float data into the destination channels. Because
        // usesFloatingPointData is true, the int* destinations alias float data.
        for (int ch = 0; ch < numDestChannels; ++ch)
        {
            if (destChannels[ch] == nullptr)
                continue;

            auto* dest = reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer;
            const int srcCh = juce::jmin (ch, 1);
            const float* src = accum_.getReadPointer (srcCh);
            std::memcpy (dest, src, (size_t) numSamples * sizeof (float));
        }

        return true;
    }

private:
    void ensureScratch (int numSamples)
    {
        if (temp_.getNumSamples() < numSamples)
        {
            temp_.setSize (2, numSamples, false, false, true);
            accum_.setSize (2, numSamples, false, false, true);
        }
    }

    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers_;
    juce::AudioBuffer<float> temp_  { 2, 0 };
    juce::AudioBuffer<float> accum_ { 2, 0 };
};

} // namespace Daw
