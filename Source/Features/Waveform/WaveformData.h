#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <cstdint>
#include <cmath>

struct WaveformPoint
{
    float peakL     = 0.0f;
    float peakR     = 0.0f;
    float rmsL      = 0.0f;
    float rmsR      = 0.0f;
    float energyLow  = 0.0f;
    float energyMid  = 0.0f;
    float energyHigh = 0.0f;
};

class WaveformData : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<WaveformData>;

    WaveformData() = default;

    static constexpr int baseSamplesPerPoint = 256;
    static constexpr int numMipmapLevels     = 6; // 1x, 2x, 4x, 8x, 16x, 32x

    // Level 0 = base resolution (256 spp), level 1 = 2x reduction, etc.
    std::vector<std::vector<WaveformPoint>> levels;

    double  sampleRate   = 0.0;
    int64_t totalSamples = 0;
    juce::String contentHash;

    int getBestLevel (double samplesPerPixel) const
    {
        if (levels.empty())
            return 0;

        for (int i = 0; i < static_cast<int> (levels.size()) - 1; ++i)
        {
            double levelSpp = static_cast<double> (baseSamplesPerPoint) * std::pow (2.0, i);
            if (levelSpp >= samplesPerPixel)
                return i;
        }

        return static_cast<int> (levels.size()) - 1;
    }

    float samplePositionToPixelX (int64_t samplePos, int level, float componentWidth) const
    {
        if (level < 0 || level >= static_cast<int> (levels.size()) || levels[static_cast<size_t> (level)].empty())
            return 0.0f;

        auto numPoints = static_cast<float> (levels[static_cast<size_t> (level)].size());
        double levelSpp = static_cast<double> (baseSamplesPerPoint) * std::pow (2.0, level);
        float pointIndex = static_cast<float> (static_cast<double> (samplePos) / levelSpp);

        return (pointIndex / numPoints) * componentWidth;
    }

    int64_t pixelXToSamplePosition (float pixelX, int level, float componentWidth) const
    {
        if (level < 0 || level >= static_cast<int> (levels.size()) || levels[static_cast<size_t> (level)].empty()
            || componentWidth <= 0.0f)
            return 0;

        auto numPoints = static_cast<float> (levels[static_cast<size_t> (level)].size());
        float pointIndex = (pixelX / componentWidth) * numPoints;
        double levelSpp = static_cast<double> (baseSamplesPerPoint) * std::pow (2.0, level);

        return static_cast<int64_t> (static_cast<double> (pointIndex) * levelSpp);
    }

    // Serialization
    juce::MemoryBlock serialize() const
    {
        juce::MemoryOutputStream out;

        out.writeDouble (sampleRate);
        out.writeInt64 (totalSamples);
        out.writeInt (static_cast<int> (levels.size()));

        for (const auto& level : levels)
        {
            auto numPoints = static_cast<int> (level.size());
            out.writeInt (numPoints);
            out.write (level.data(), static_cast<size_t> (numPoints) * sizeof (WaveformPoint));
        }

        return out.getMemoryBlock();
    }

    static Ptr deserialize (const juce::MemoryBlock& data, const juce::String& hash)
    {
        juce::MemoryInputStream in (data, false);

        auto result    = Ptr (new WaveformData());
        result->sampleRate   = in.readDouble();
        result->totalSamples = in.readInt64();
        result->contentHash  = hash;

        int numLevels = in.readInt();
        if (numLevels < 0 || numLevels > 16)
            return nullptr;

        result->levels.resize (static_cast<size_t> (numLevels));

        for (int i = 0; i < numLevels; ++i)
        {
            int numPoints = in.readInt();
            if (numPoints < 0 || numPoints > 10000000)
                return nullptr;

            result->levels[static_cast<size_t> (i)].resize (static_cast<size_t> (numPoints));
            in.read (result->levels[static_cast<size_t> (i)].data(),
                     static_cast<int> (static_cast<size_t> (numPoints) * sizeof (WaveformPoint)));
        }

        return result;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformData)
};
