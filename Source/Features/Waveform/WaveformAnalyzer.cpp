#include "WaveformAnalyzer.h"
#include <juce_events/juce_events.h>
#include <cmath>

// ============================================================================
// AnalysisJob — ThreadPoolJob that computes waveform data
// ============================================================================

class WaveformAnalyzer::AnalysisJob : public juce::ThreadPoolJob
{
public:
    AnalysisJob (WaveformAnalyzer& owner,
                 const juce::String& hash,
                 std::vector<AudioBufferHolder::Ptr> bufs,
                 Callback cb)
        : juce::ThreadPoolJob ("WaveformAnalysis_" + hash),
          analyzer (owner),
          contentHash (hash),
          buffers (std::move (bufs)),
          callback (std::move (cb))
    {}

    JobStatus runJob() override
    {
        // 1. Check SQLite cache first
        juce::MemoryBlock cachedData;
        if (analyzer.db.loadWaveformData (contentHash, cachedData))
        {
            auto data = WaveformData::deserialize (cachedData, contentHash);
            if (data != nullptr)
            {
                deliverResult (std::move (data));
                return jobHasFinished;
            }
        }

        if (shouldExit())
            return jobHasFinished;

        // 2. Resolve the working buffer: a single source is used directly; several
        //    sources are summed here on the background thread (instrumental stem).
        AudioBufferHolder::Ptr buffer = (buffers.size() == 1)
                                            ? buffers.front()
                                            : sumBuffers (buffers);
        if (buffer == nullptr || shouldExit())
            return jobHasFinished;

        // 3. Analyze from PCM buffer
        auto data = analyzeBuffer (*buffer);
        if (data == nullptr || shouldExit())
            return jobHasFinished;

        // 4. Cache to SQLite
        auto serialized = data->serialize();
        analyzer.db.storeWaveformData (contentHash, serialized);

        // 5. Deliver result on message thread
        deliverResult (std::move (data));
        return jobHasFinished;
    }

private:
    // Mix several stem buffers into one (instrumental = drums + bass + other).
    // Runs on the background thread. Skips null contributors; the output spans the
    // shortest contributor so no read runs past a buffer's end.
    static AudioBufferHolder::Ptr sumBuffers (const std::vector<AudioBufferHolder::Ptr>& bufs)
    {
        int     numChannels = 0;
        int64_t numFrames   = 0;
        double  sampleRate  = 0.0;

        for (const auto& b : bufs)
        {
            if (b == nullptr)
                continue;
            numChannels = juce::jmax (numChannels, b->getBuffer().getNumChannels());
            numFrames   = (numFrames == 0) ? b->getNumFrames()
                                           : juce::jmin (numFrames, b->getNumFrames());
            sampleRate  = b->getSampleRate();
        }

        if (numChannels < 1 || numFrames <= 0 || sampleRate <= 0.0)
            return nullptr;

        const int frames = static_cast<int> (numFrames);
        juce::AudioBuffer<float> out (numChannels, frames);
        out.clear();

        for (const auto& b : bufs)
        {
            if (b == nullptr)
                continue;
            const auto& src    = b->getBuffer();
            const int   srcCh  = src.getNumChannels();
            const int   n      = static_cast<int> (juce::jmin<int64_t> (numFrames, b->getNumFrames()));
            for (int c = 0; c < numChannels; ++c)
            {
                // Fan a mono contributor across both output channels.
                const int sc = juce::jmin (c, srcCh - 1);
                out.addFrom (c, 0, src, sc, 0, n);
            }
        }

        return AudioBufferHolder::Ptr (
            new AudioBufferHolder (std::move (out), sampleRate, numFrames));
    }

    WaveformData::Ptr analyzeBuffer (const AudioBufferHolder& source)
    {
        const auto& audioBuffer = source.getBuffer();
        const auto  sr          = source.getSampleRate();
        const auto  numFrames   = source.getNumFrames();
        const int   numChannels = audioBuffer.getNumChannels();

        if (numFrames <= 0 || numChannels < 1 || sr <= 0.0)
            return nullptr;

        const float* channelL = audioBuffer.getReadPointer (0);
        const float* channelR = numChannels >= 2 ? audioBuffer.getReadPointer (1) : channelL;

        const int samplesPerPoint = WaveformData::baseSamplesPerPoint;
        const int numPoints = static_cast<int> ((numFrames + samplesPerPoint - 1) / samplesPerPoint);

        auto result = WaveformData::Ptr (new WaveformData());
        result->sampleRate   = sr;
        result->totalSamples = numFrames;
        result->contentHash  = contentHash;
        result->levels.resize (WaveformData::numMipmapLevels);
        result->levels[0].resize (static_cast<size_t> (numPoints));

        // IIR filter coefficients for 3-band split
        auto lowCoeffs  = juce::IIRCoefficients::makeLowPass (sr, 250.0);
        auto highCoeffs = juce::IIRCoefficients::makeHighPass (sr, 4000.0);
        // Band-pass: geometric mean of 250 and 4000
        auto midCoeffs  = juce::IIRCoefficients::makeBandPass (sr, std::sqrt (250.0 * 4000.0));

        // Create filters for L and R channels
        juce::IIRFilter lowFilterL, lowFilterR;
        juce::IIRFilter midFilterL, midFilterR;
        juce::IIRFilter highFilterL, highFilterR;

        lowFilterL.setCoefficients (lowCoeffs);
        lowFilterR.setCoefficients (lowCoeffs);
        midFilterL.setCoefficients (midCoeffs);
        midFilterR.setCoefficients (midCoeffs);
        highFilterL.setCoefficients (highCoeffs);
        highFilterR.setCoefficients (highCoeffs);

        // Process blocks
        for (int pointIdx = 0; pointIdx < numPoints; ++pointIdx)
        {
            if (shouldExit())
                return nullptr;

            int64_t startSample = static_cast<int64_t> (pointIdx) * samplesPerPoint;
            int64_t endSample   = juce::jmin (startSample + samplesPerPoint, numFrames);
            int     blockSize   = static_cast<int> (endSample - startSample);

            float peakL = 0.0f, peakR = 0.0f;
            float sumSqL = 0.0f, sumSqR = 0.0f;
            float sumLowEnergy = 0.0f, sumMidEnergy = 0.0f, sumHighEnergy = 0.0f;

            for (int i = 0; i < blockSize; ++i)
            {
                float sL = channelL[startSample + i];
                float sR = channelR[startSample + i];

                // Peak
                float absL = std::fabs (sL);
                float absR = std::fabs (sR);
                if (absL > peakL) peakL = absL;
                if (absR > peakR) peakR = absR;

                // RMS
                sumSqL += sL * sL;
                sumSqR += sR * sR;

                // Frequency splitting (mono sum for energy)
                float mono = (sL + sR) * 0.5f;

                float lowSample  = lowFilterL.processSingleSampleRaw (mono);
                float midSample  = midFilterL.processSingleSampleRaw (mono);
                float highSample = highFilterL.processSingleSampleRaw (mono);

                sumLowEnergy  += lowSample * lowSample;
                sumMidEnergy  += midSample * midSample;
                sumHighEnergy += highSample * highSample;
            }

            float invBlockSize = 1.0f / static_cast<float> (blockSize);

            auto& point    = result->levels[0][static_cast<size_t> (pointIdx)];
            point.peakL     = peakL;
            point.peakR     = peakR;
            point.rmsL      = std::sqrt (sumSqL * invBlockSize);
            point.rmsR      = std::sqrt (sumSqR * invBlockSize);
            point.energyLow  = std::sqrt (sumLowEnergy * invBlockSize);
            point.energyMid  = std::sqrt (sumMidEnergy * invBlockSize);
            point.energyHigh = std::sqrt (sumHighEnergy * invBlockSize);
        }

        // Build mipmap levels (each level halves the previous)
        for (int level = 1; level < WaveformData::numMipmapLevels; ++level)
        {
            if (shouldExit())
                return nullptr;

            const auto& prev = result->levels[static_cast<size_t> (level - 1)];
            auto prevSize = static_cast<int> (prev.size());
            int newSize = (prevSize + 1) / 2;

            result->levels[static_cast<size_t> (level)].resize (static_cast<size_t> (newSize));

            for (int i = 0; i < newSize; ++i)
            {
                int idx0 = i * 2;
                int idx1 = juce::jmin (idx0 + 1, prevSize - 1);

                auto& dst = result->levels[static_cast<size_t> (level)][static_cast<size_t> (i)];
                const auto& a = prev[static_cast<size_t> (idx0)];
                const auto& b = prev[static_cast<size_t> (idx1)];

                dst.peakL     = juce::jmax (a.peakL, b.peakL);
                dst.peakR     = juce::jmax (a.peakR, b.peakR);
                dst.rmsL      = std::sqrt ((a.rmsL * a.rmsL + b.rmsL * b.rmsL) * 0.5f);
                dst.rmsR      = std::sqrt ((a.rmsR * a.rmsR + b.rmsR * b.rmsR) * 0.5f);
                dst.energyLow  = juce::jmax (a.energyLow, b.energyLow);
                dst.energyMid  = juce::jmax (a.energyMid, b.energyMid);
                dst.energyHigh = juce::jmax (a.energyHigh, b.energyHigh);
            }
        }

        return result;
    }

    void deliverResult (WaveformData::Ptr data)
    {
        auto cb   = callback;
        auto hash = contentHash;
        juce::MessageManager::callAsync ([cb, hash, data]()
        {
            if (cb)
                cb (hash, data);
        });
    }

    WaveformAnalyzer&                   analyzer;
    juce::String                        contentHash;
    std::vector<AudioBufferHolder::Ptr> buffers;
    Callback                            callback;
};

// ============================================================================
// WaveformAnalyzer implementation
// ============================================================================

WaveformAnalyzer::WaveformAnalyzer (TrackDatabase& database)
    : db (database)
{
}

WaveformAnalyzer::~WaveformAnalyzer()
{
    threadPool.removeAllJobs (true, 5000);
}

void WaveformAnalyzer::analyze (const juce::String& contentHash,
                                AudioBufferHolder::Ptr buffer,
                                Callback callback)
{
    std::vector<AudioBufferHolder::Ptr> bufs { std::move (buffer) };
    auto* job = new AnalysisJob (*this, contentHash, std::move (bufs), std::move (callback));
    threadPool.addJob (job, true);
}

void WaveformAnalyzer::analyzeSum (const juce::String& cacheKey,
                                   std::vector<AudioBufferHolder::Ptr> buffers,
                                   Callback callback)
{
    auto* job = new AnalysisJob (*this, cacheKey, std::move (buffers), std::move (callback));
    threadPool.addJob (job, true);
}
