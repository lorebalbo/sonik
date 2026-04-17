#include "TimeStretcher.h"
#include <cstring>
#include <algorithm>

TimeStretcher::TimeStretcher (double sampleRate, int channels, int maxBlockSize)
{
    using RBS = RubberBand::RubberBandStretcher;

    // RealTime mode pre-allocates all buffers at construction.
    // OptionEngineFiner (R3) provides highest quality with good latency.
    // R3 engine handles transient preservation internally — no need for
    // OptionWindowShort or OptionTransientsCrisp which degrade quality.
    auto options = RBS::OptionProcessRealTime
                 | RBS::OptionEngineFiner;

    stretcher = std::make_unique<RBS> (
        static_cast<size_t> (sampleRate),
        static_cast<size_t> (channels),
        options,
        1.0,   // initial time ratio
        1.0    // initial pitch scale (1.0 = no shift)
    );

    // Tell the stretcher the maximum block size so it can pre-allocate.
    stretcher->setMaxProcessSize (static_cast<size_t> (maxBlockSize));
}

TimeStretcher::~TimeStretcher() = default;

int TimeStretcher::process (const float* const* input, int inputSamples,
                            float* const* output, int maxOutputSamples,
                            double timeRatio)
{
    if (inputSamples <= 0)
        return 0;

    // Update time ratio if changed (audio-thread safe in RealTime mode).
    if (std::abs (timeRatio - lastTimeRatio) > 1.0e-9)
    {
        stretcher->setTimeRatio (timeRatio);
        lastTimeRatio = timeRatio;
    }

    // Feed input to the stretcher.
    stretcher->process (input, static_cast<size_t> (inputSamples), false);

    // Retrieve available output.
    int avail = stretcher->available();
    if (avail <= 0)
        return 0;

    int toRetrieve = std::min (avail, maxOutputSamples);
    stretcher->retrieve (output, static_cast<size_t> (toRetrieve));

    return toRetrieve;
}

void TimeStretcher::reset()
{
    stretcher->reset();
    lastTimeRatio = 1.0;
}

int TimeStretcher::getLatency() const
{
    return static_cast<int> (stretcher->getLatency());
}

void TimeStretcher::prime()
{
    // Feed silence for the preferred start pad so the stretcher is primed
    // and ready to produce output immediately.
    size_t pad = stretcher->getPreferredStartPad();
    if (pad == 0)
        return;

    // Use stack-allocated small chunks to avoid heap allocation.
    static constexpr size_t chunkSize = 512;
    float silenceA[chunkSize] = {};
    float silenceB[chunkSize] = {};
    const float* silencePtrs[2] = { silenceA, silenceB };

    size_t remaining = pad;
    while (remaining > 0)
    {
        size_t n = std::min (remaining, chunkSize);
        stretcher->process (silencePtrs, n, false);
        remaining -= n;
    }

    // Discard any output produced during priming.
    int avail = stretcher->available();
    if (avail > 0)
    {
        float discardA[chunkSize];
        float discardB[chunkSize];
        float* discardPtrs[2] = { discardA, discardB };
        size_t discardRemaining = static_cast<size_t> (avail);
        while (discardRemaining > 0)
        {
            size_t n = std::min (discardRemaining, chunkSize);
            stretcher->retrieve (discardPtrs, n);
            discardRemaining -= n;
        }
    }
}

void TimeStretcher::primeWithAudio (const float* channelL, const float* channelR,
                                    int numFramesAvailable)
{
    // Feed real track audio for pad + latency samples so the stretcher's
    // internal pipeline is filled with valid data.  After this, the
    // stretcher output is immediately aligned with the playhead when
    // processBlock reads from playheadAccumulator + latency.
    size_t pad     = stretcher->getPreferredStartPad();
    size_t latency = stretcher->getLatency();
    size_t total   = pad + latency;
    size_t avail   = static_cast<size_t> (std::max (0, numFramesAvailable));

    static constexpr size_t chunkSize = 512;

    size_t fed = 0;
    while (fed < total)
    {
        size_t n = std::min (chunkSize, total - fed);

        if (fed < avail)
        {
            // Feed real track audio
            size_t realSamples = std::min (n, avail - fed);
            const float* ptrs[2] = { channelL + fed, channelR + fed };
            stretcher->process (ptrs, realSamples, false);
            fed += realSamples;

            // If we consumed less than n, pad the rest with silence
            if (realSamples < n)
            {
                size_t silCount = n - realSamples;
                float silA[chunkSize] = {};
                float silB[chunkSize] = {};
                const float* silPtrs[2] = { silA, silB };
                stretcher->process (silPtrs, silCount, false);
                fed += silCount;
            }
        }
        else
        {
            // Beyond available audio — feed silence
            float silA[chunkSize] = {};
            float silB[chunkSize] = {};
            const float* silPtrs[2] = { silA, silB };
            stretcher->process (silPtrs, n, false);
            fed += n;
        }
    }

    // Discard all output produced during priming.
    int outputAvail = stretcher->available();
    if (outputAvail > 0)
    {
        float discardA[chunkSize];
        float discardB[chunkSize];
        float* discardPtrs[2] = { discardA, discardB };
        size_t discardRemaining = static_cast<size_t> (outputAvail);
        while (discardRemaining > 0)
        {
            size_t dn = std::min (discardRemaining, chunkSize);
            stretcher->retrieve (discardPtrs, dn);
            discardRemaining -= dn;
        }
    }
}
