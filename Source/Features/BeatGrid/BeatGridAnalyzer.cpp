#include "BeatGridAnalyzer.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>

#include <essentia/algorithmfactory.h>
#include <essentia/essentiamath.h>

// ============================================================================
// Essentia one-time initialisation (thread-safe via std::call_once)
// ============================================================================

namespace
{

std::once_flag essentiaInitFlag;

void ensureEssentiaInitialised()
{
    std::call_once (essentiaInitFlag, []() { essentia::init(); });
}

} // anonymous namespace

// ============================================================================
// AnalysisJob
// ============================================================================

class BeatGridAnalyzer::AnalysisJob : public juce::ThreadPoolJob
{
public:
    AnalysisJob (BeatGridAnalyzer& owner,
                 const juce::String& hash,
                 const juce::String& path,
                 AudioBufferHolder::Ptr buf,
                 Callback cb)
        : juce::ThreadPoolJob ("BeatGridAnalysis_" + hash),
          analyzer (owner),
          contentHash (hash),
          filePath (path),
          buffer (std::move (buf)),
          callback (std::move (cb))
    {}

    JobStatus runJob() override
    {
        // 1. Check SQLite cache
        auto cached = analyzer.db.loadTrackData (filePath, contentHash);
        if (cached.has_value() && cached->beatgridJson.isNotEmpty())
        {
            auto data = BeatGridData::fromJson (cached->beatgridJson);
            if (data != nullptr && data->bpm > 0.0)
            {
                deliverResult (std::move (data));
                return jobHasFinished;
            }
        }

        if (shouldExit())
            return jobHasFinished;

        // 2. Run DSP analysis
        auto data = analyzeBuffer();
        if (data == nullptr || shouldExit())
        {
            deliverResult (nullptr);
            return jobHasFinished;
        }

        // 3. Cache to database
        {
            // Load existing track data to preserve other fields
            auto existing = analyzer.db.loadTrackData (filePath, contentHash);

            juce::String cuePointsJson;
            int keyIndex = -1;
            float keyConfidence = 0.0f;
            bool keyManual = false;

            if (existing.has_value())
            {
                // If manually adjusted, don't overwrite
                auto existingBg = BeatGridData::fromJson (existing->beatgridJson);
                if (existingBg != nullptr && existingBg->manuallyAdjusted)
                {
                    deliverResult (std::move (existingBg));
                    return jobHasFinished;
                }

                cuePointsJson = existing->cuePointsJson;
                keyIndex       = existing->keyIndex;
                keyConfidence  = existing->keyConfidence;
                keyManual      = existing->keyManuallyAdjusted;
            }

            // Only cache results with valid BPM
            if (data->bpm > 0.0)
            {
                analyzer.db.saveTrackData (filePath, contentHash,
                                           cuePointsJson, data->toJson(),
                                           keyIndex, keyConfidence, keyManual);
            }
        }

        // 4. Deliver
        deliverResult (std::move (data));
        return jobHasFinished;
    }

private:
    BeatGridData::Ptr analyzeBuffer()
    {
        const auto& audioBuffer = buffer->getBuffer();
        const auto  sr          = buffer->getSampleRate();
        const auto  numFrames   = buffer->getNumFrames();
        const int   numChannels = audioBuffer.getNumChannels();

        if (numFrames <= 0 || numChannels < 1 || sr <= 0.0)
            return nullptr;

        // ================================================================
        // Step 1: Downmix to mono
        // ================================================================
        const float* channelL = audioBuffer.getReadPointer (0);
        const float* channelR = numChannels >= 2 ? audioBuffer.getReadPointer (1) : channelL;

        std::vector<essentia::Real> mono (static_cast<size_t> (numFrames));
        for (int64_t i = 0; i < numFrames; ++i)
            mono[static_cast<size_t> (i)] = (channelL[i] + channelR[i]) * 0.5f;

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 2: Resample to 44100 Hz if needed
        //         (RhythmExtractor2013 requires 44100 Hz)
        // ================================================================
        constexpr double essentiaRate = 44100.0;
        std::vector<essentia::Real> signal;

        if (std::abs (sr - essentiaRate) > 1.0)
        {
            ensureEssentiaInitialised();
            auto& factory = essentia::standard::AlgorithmFactory::instance();
            std::unique_ptr<essentia::standard::Algorithm> resample (
                factory.create ("Resample",
                                "inputSampleRate", static_cast<essentia::Real> (sr),
                                "outputSampleRate", static_cast<essentia::Real> (essentiaRate),
                                "quality", 4));

            resample->input ("signal").set (mono);
            resample->output ("signal").set (signal);
            resample->compute();
        }
        else
        {
            signal = std::move (mono);
        }

        if (signal.empty() || shouldExit())
            return nullptr;

        // ================================================================
        // Step 3: Run Essentia RhythmExtractor2013
        //
        // Uses the "multifeature" method (5 onset detection functions +
        // multi-agent HMM beat tracker) for best accuracy.
        //
        // Outputs:
        //   bpm         — estimated BPM (Real)
        //   ticks       — beat positions in seconds (vector<Real>)
        //   confidence  — detection confidence 0..5.32 (Real)
        //   estimates   — BPM candidates (vector<Real>)
        //   bpmIntervals— inter-beat intervals in BPM (vector<Real>)
        // ================================================================
        ensureEssentiaInitialised();

        essentia::Real estBpm = 0.0f;
        std::vector<essentia::Real> ticks;
        essentia::Real ticksConfidence = 0.0f;
        std::vector<essentia::Real> estimates;
        std::vector<essentia::Real> bpmIntervals;

        {
            auto& factory = essentia::standard::AlgorithmFactory::instance();
            std::unique_ptr<essentia::standard::Algorithm> rhythmExtractor (
                factory.create ("RhythmExtractor2013",
                                "method", "multifeature",
                                "minTempo", 40,
                                "maxTempo", 208));

            rhythmExtractor->input ("signal").set (signal);
            rhythmExtractor->output ("bpm").set (estBpm);
            rhythmExtractor->output ("ticks").set (ticks);
            rhythmExtractor->output ("confidence").set (ticksConfidence);
            rhythmExtractor->output ("estimates").set (estimates);
            rhythmExtractor->output ("bpmIntervals").set (bpmIntervals);
            rhythmExtractor->compute();
        }

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 4: Resolve BPM to preferred 80–160 range
        // ================================================================
        double finalBpm = static_cast<double> (estBpm);
        while (finalBpm > 0.0 && finalBpm < 80.0)  finalBpm *= 2.0;
        while (finalBpm > 160.0)                    finalBpm /= 2.0;

        // ================================================================
        // Step 5: Compute fixed grid from beat ticks
        //
        // The beat ticks from Essentia are individual positions (in seconds).
        // We compute the optimal fixed grid (anchor + constant interval)
        // by computing the median inter-beat interval, then finding the
        // phase that best aligns with the detected ticks.
        // ================================================================
        if (ticks.size() < 2)
        {
            // Not enough beats detected
            auto result = BeatGridData::Ptr (new BeatGridData());
            result->bpm                = 0.0;
            result->confidence         = 0.0f;
            result->analysisSampleRate = sr;
            return result;
        }

        // Beat interval from finalBpm (in source sample rate samples)
        double beatIntervalSamples = 60.0 * sr / finalBpm;

        // Convert ticks (in seconds at 44100) to source sample positions
        double rateRatio = sr / essentiaRate;
        std::vector<double> tickSamples;
        tickSamples.reserve (ticks.size());
        for (auto t : ticks)
            tickSamples.push_back (static_cast<double> (t) * sr);

        // ================================================================
        // Step 5: Compute fixed grid anchor via circular statistics.
        //
        // Each detected tick is mapped to a phase angle on the unit circle
        // (angle = (tick mod beatInterval) / beatInterval * 2π).  The
        // circular mean of those angles gives the exact optimal anchor —
        // no discrete search, no quantisation error.
        // ================================================================
        double bestAnchor = 0.0;
        {
            const double twoPi = juce::MathConstants<double>::twoPi;
            double sinSum = 0.0, cosSum = 0.0;

            for (auto tick : tickSamples)
            {
                double phase = std::fmod (tick, beatIntervalSamples);
                if (phase < 0.0)
                    phase += beatIntervalSamples;
                double angle = phase / beatIntervalSamples * twoPi;
                sinSum += std::sin (angle);
                cosSum += std::cos (angle);
            }

            double meanAngle = std::atan2 (sinSum, cosSum);
            if (meanAngle < 0.0)
                meanAngle += twoPi;
            bestAnchor = meanAngle / twoPi * beatIntervalSamples;
        }

        // ================================================================
        // Step 6: Confidence mapping
        //
        // Compute RMS grid alignment error to measure quality.
        // Essentia's multifeature confidence is in [0, 5.32].
        // Map to [0, 1] with a saturation at ~4.0 (strong detection).
        // Also incorporate grid alignment quality.
        // ================================================================
        double sumSqErr = 0.0;
        for (auto tick : tickSamples)
        {
            double offset   = tick - bestAnchor;
            double beatFrac = offset / beatIntervalSamples;
            double nearest  = std::round (beatFrac) * beatIntervalSamples + bestAnchor;
            double err      = tick - nearest;
            sumSqErr += err * err;
        }
        double rmsGridError = std::sqrt (sumSqErr / static_cast<double> (tickSamples.size()));
        double gridQuality  = juce::jlimit (0.0, 1.0, 1.0 - rmsGridError / beatIntervalSamples);

        float conf = juce::jlimit (0.0f, 1.0f,
            static_cast<float> (static_cast<double> (ticksConfidence) / 4.0 * 0.7
                              + gridQuality * 0.3));

        if (conf < 0.15f)
        {
            auto result = BeatGridData::Ptr (new BeatGridData());
            result->bpm                = 0.0;
            result->confidence         = conf;
            result->analysisSampleRate = sr;
            return result;
        }

        // ================================================================
        // Build result
        // ================================================================
        auto result = BeatGridData::Ptr (new BeatGridData());
        result->bpm                 = finalBpm;
        result->anchorSample        = static_cast<int64_t> (std::round (bestAnchor));
        result->beatIntervalSamples = beatIntervalSamples;
        result->confidence          = conf;
        result->manuallyAdjusted    = false;
        result->analysisSampleRate  = sr;

        return result;
    }

    void deliverResult (BeatGridData::Ptr data)
    {
        auto cb   = callback;
        auto hash = contentHash;
        juce::MessageManager::callAsync ([cb, hash, data]()
        {
            if (cb)
                cb (hash, data);
        });
    }

    BeatGridAnalyzer&      analyzer;
    juce::String           contentHash;
    juce::String           filePath;
    AudioBufferHolder::Ptr buffer;
    Callback               callback;
};

// ============================================================================
// BeatGridAnalyzer implementation
// ============================================================================

BeatGridAnalyzer::BeatGridAnalyzer (TrackDatabase& database)
    : db (database)
{
}

BeatGridAnalyzer::~BeatGridAnalyzer()
{
    threadPool.removeAllJobs (true, 5000);
}

void BeatGridAnalyzer::analyze (const juce::String& contentHash,
                                const juce::String& filePath,
                                AudioBufferHolder::Ptr buffer,
                                Callback callback)
{
    auto* job = new AnalysisJob (*this, contentHash, filePath, std::move (buffer), std::move (callback));
    threadPool.addJob (job, true);
}
