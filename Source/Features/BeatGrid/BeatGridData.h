#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <cmath>

struct BeatGridData : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<BeatGridData>;

    double  bpm                 = 0.0;
    int64_t anchorSample        = 0;
    double  beatIntervalSamples = 0.0;
    float   confidence          = 0.0f;
    bool    manuallyAdjusted    = false;
    double  analysisSampleRate  = 44100.0;

    int64_t getNearestBeat (int64_t samplePos) const
    {
        if (beatIntervalSamples <= 0.0)
            return samplePos;

        double offset = static_cast<double> (samplePos - anchorSample);
        double beatIndex = std::round (offset / beatIntervalSamples);
        return anchorSample + static_cast<int64_t> (beatIndex * beatIntervalSamples);
    }

    int getBeatIndex (int64_t samplePos) const
    {
        if (beatIntervalSamples <= 0.0)
            return 0;

        double offset = static_cast<double> (samplePos - anchorSample);
        return static_cast<int> (std::round (offset / beatIntervalSamples));
    }

    float getBeatPhase (int64_t samplePos) const
    {
        if (beatIntervalSamples <= 0.0)
            return 0.0f;

        double offset = static_cast<double> (samplePos - anchorSample);
        double phase = std::fmod (offset / beatIntervalSamples, 1.0);
        if (phase < 0.0)
            phase += 1.0;
        return static_cast<float> (phase);
    }

    void getBeatsInRange (int64_t start, int64_t end,
                          juce::Array<int64_t>& beats,
                          juce::Array<bool>& isDownbeat) const
    {
        beats.clearQuick();
        isDownbeat.clearQuick();

        if (beatIntervalSamples <= 0.0 || end <= start)
            return;

        double offset = static_cast<double> (start - anchorSample);
        int firstBeatIdx = static_cast<int> (std::ceil (offset / beatIntervalSamples));

        for (int i = firstBeatIdx; ; ++i)
        {
            int64_t beatPos = anchorSample + static_cast<int64_t> (static_cast<double> (i) * beatIntervalSamples);

            if (beatPos >= end)
                break;

            if (beatPos >= start)
            {
                beats.add (beatPos);

                // Compute a stable modulo so that beat index 0 at anchor is a downbeat
                int mod = ((i % 4) + 4) % 4;
                isDownbeat.add (mod == 0);
            }
        }
    }

    juce::String toJson() const
    {
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("bpm",                 bpm);
        obj->setProperty ("anchorSample",         static_cast<double> (anchorSample));
        obj->setProperty ("beatIntervalSamples",  beatIntervalSamples);
        obj->setProperty ("confidence",           static_cast<double> (confidence));
        obj->setProperty ("manuallyAdjusted",     manuallyAdjusted);
        obj->setProperty ("sampleRate",           analysisSampleRate);

        return juce::JSON::toString (juce::var (obj.release()));
    }

    static Ptr fromJson (const juce::String& json)
    {
        auto parsed = juce::JSON::parse (json);
        if (! parsed.isObject())
            return nullptr;

        auto result = Ptr (new BeatGridData());
        if (auto* obj = parsed.getDynamicObject())
        {
            result->bpm                 = obj->getProperty ("bpm");
            result->anchorSample        = static_cast<int64_t> (static_cast<double> (obj->getProperty ("anchorSample")));
            result->beatIntervalSamples = obj->getProperty ("beatIntervalSamples");
            result->confidence          = static_cast<float> (static_cast<double> (obj->getProperty ("confidence")));
            result->manuallyAdjusted    = obj->getProperty ("manuallyAdjusted");
            result->analysisSampleRate  = obj->getProperty ("sampleRate");
        }
        return result;
    }
};
