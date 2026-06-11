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
    if (stretcher == nullptr || input == nullptr || output == nullptr
        || inputSamples <= 0 || maxOutputSamples <= 0)
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

void TimeStretcher::setPitchScale (double scale)
{
    if (stretcher == nullptr)
        return;

    // Clamp to a sensible range (one octave each direction is more than
    // we need for a ±12-semitone key stepper; the upper bound is generous).
    if (scale < 0.5)  scale = 0.5;
    if (scale > 2.0)  scale = 2.0;

    if (std::abs (scale - lastPitchScale) > 1.0e-9)
    {
        stretcher->setPitchScale (scale);
        lastPitchScale = scale;
    }
}

void TimeStretcher::reset()
{
    stretcher->reset();
    lastTimeRatio = 1.0;
    lastPitchScale = 1.0;
}

int TimeStretcher::getLatency() const
{
    return static_cast<int> (stretcher->getLatency());
}

int TimeStretcher::getStartPad() const
{
    return stretcher != nullptr ? static_cast<int> (stretcher->getPreferredStartPad()) : 0;
}

int TimeStretcher::getBufferedOutputSamples() const
{
    if (stretcher == nullptr)
        return 0;

    return std::max (0, static_cast<int> (stretcher->available()));
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

int TimeStretcher::primeWithAudio (const float* channelL, const float* channelR,
                                   int numFramesAvailable)
{
    // Feed real track audio for pad + latency samples so the stretcher's
    // internal pipeline is filled with valid data.
    //
    // The R3 engine does NOT produce all (pad + latency) output samples
    // immediately after being fed — it only makes a small fraction available
    // at once.  We discard that fraction and compute the EFFECTIVE pipeline
    // depth as:
    //
    //   effectiveLatency = (pad + latency) - discardedSamples
    //
    // The caller (AudioEngine) stores this as stretcherLatency and uses it
    // as the read-ahead offset:
    //
    //   readPos = playheadAccumulator + stretcherLatency
    //
    // This ensures the stretched output aligns exactly with the vinyl path
    // (zero phase offset at Key Lock toggle).  See measurement in
    // check_r3.cpp: readAhead=3840 gives output[0]=file pos 0 for R3.
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

    // Discard all output produced during priming and measure how much
    // was ready.  The rest is still buffered in the pipeline.
    int discarded = stretcher->available();
    if (discarded > 0)
    {
        float discardA[chunkSize];
        float discardB[chunkSize];
        float* discardPtrs[2] = { discardA, discardB };
        size_t discardRemaining = static_cast<size_t> (discarded);
        while (discardRemaining > 0)
        {
            size_t dn = std::min (discardRemaining, chunkSize);
            stretcher->retrieve (discardPtrs, dn);
            discardRemaining -= dn;
        }
    }

    // The effective read-ahead offset needed for zero phase error.
    return static_cast<int> (pad + latency) - discarded;
}
