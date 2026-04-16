#include "BeatGridAnalyzer.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>

// ============================================================================
// Simple radix-2 FFT (avoids needing juce_dsp module)
// ============================================================================

namespace
{

void fftRadix2 (std::vector<std::complex<float>>& data)
{
    auto n = static_cast<int> (data.size());
    if (n <= 1)
        return;

    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
            std::swap (data[static_cast<size_t> (i)],
                       data[static_cast<size_t> (j)]);
    }

    // Cooley-Tukey iterative FFT
    for (int len = 2; len <= n; len <<= 1)
    {
        float angle = -2.0f * juce::MathConstants<float>::pi / static_cast<float> (len);
        std::complex<float> wLen (std::cos (angle), std::sin (angle));

        for (int i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j)
            {
                auto u = data[static_cast<size_t> (i + j)];
                auto v = data[static_cast<size_t> (i + j + len / 2)] * w;
                data[static_cast<size_t> (i + j)]             = u + v;
                data[static_cast<size_t> (i + j + len / 2)]   = u - v;
                w *= wLen;
            }
        }
    }
}

int nextPowerOfTwo (int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
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

        std::vector<float> mono (static_cast<size_t> (numFrames));
        for (int64_t i = 0; i < numFrames; ++i)
            mono[static_cast<size_t> (i)] = (channelL[i] + channelR[i]) * 0.5f;

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 2: Spectral flux onset detection
        // 2048-sample FFT, 512-sample hop, Hann window (per PRD-0008)
        // ================================================================
        constexpr int fftSize = 2048;
        constexpr int hopSize = 512;

        std::vector<float> window (fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[static_cast<size_t> (i)] = 0.5f * (1.0f - std::cos (
                2.0f * juce::MathConstants<float>::pi * static_cast<float> (i)
                    / static_cast<float> (fftSize)));

        int numHops = static_cast<int> ((numFrames - fftSize) / hopSize) + 1;
        if (numHops <= 0)
            return nullptr;

        const int fftN   = nextPowerOfTwo (fftSize);
        const int numBins = fftN / 2 + 1;

        std::vector<float> onsetFunction (static_cast<size_t> (numHops), 0.0f);
        std::vector<float> prevMagnitude (static_cast<size_t> (numBins), 0.0f);

        for (int hop = 0; hop < numHops; ++hop)
        {
            if (hop % 1000 == 0 && shouldExit())
                return nullptr;

            int64_t startSample = static_cast<int64_t> (hop) * hopSize;

            std::vector<std::complex<float>> fftBuf (static_cast<size_t> (fftN), { 0.0f, 0.0f });
            for (int i = 0; i < fftSize; ++i)
            {
                int64_t idx = startSample + i;
                float sample = (idx < numFrames) ? mono[static_cast<size_t> (idx)] : 0.0f;
                fftBuf[static_cast<size_t> (i)] = { sample * window[static_cast<size_t> (i)], 0.0f };
            }

            fftRadix2 (fftBuf);

            float flux = 0.0f;
            for (int bin = 0; bin < numBins; ++bin)
            {
                float mag  = std::abs (fftBuf[static_cast<size_t> (bin)]);
                float diff = mag - prevMagnitude[static_cast<size_t> (bin)];
                if (diff > 0.0f)
                    flux += diff;
                prevMagnitude[static_cast<size_t> (bin)] = mag;
            }

            onsetFunction[static_cast<size_t> (hop)] = flux;
        }

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 3: Autocorrelation tempo estimation with Rayleigh prior
        //
        // Rayleigh prior resolves octave ambiguity by favouring lags
        // near 120 BPM. Autocorrelation is run over the raw (un-normalised)
        // onset function for maximum signal.
        // ================================================================
        double odfRate = sr / static_cast<double> (hopSize);

        int minLag = juce::jmax (1, static_cast<int> (60.0 * odfRate / 220.0));
        int maxLag = juce::jmin (static_cast<int> (60.0 * odfRate / 40.0),
                                 numHops / 2);

        if (minLag >= maxLag)
            return nullptr;

        int acLen = maxLag - minLag + 1;
        std::vector<float> autocorr (static_cast<size_t> (acLen), 0.0f);

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            float sum   = 0.0f;
            int   count = 0;
            for (int i = 0; i < numHops - lag; ++i)
            {
                sum += onsetFunction[static_cast<size_t> (i)]
                     * onsetFunction[static_cast<size_t> (i + lag)];
                ++count;
            }
            if (count > 0)
                autocorr[static_cast<size_t> (lag - minLag)] = sum / static_cast<float> (count);
        }

        // Rayleigh-weighted tempo prior centred at ~120 BPM
        double priorLag = 60.0 * odfRate / 120.0;
        double sigma    = priorLag * 0.7;

        std::vector<float> weightedAc (static_cast<size_t> (acLen));
        for (int i = 0; i < acLen; ++i)
        {
            double lag      = static_cast<double> (i + minLag);
            double rayleigh = (lag / (sigma * sigma))
                            * std::exp (-(lag * lag) / (2.0 * sigma * sigma));
            weightedAc[static_cast<size_t> (i)] =
                autocorr[static_cast<size_t> (i)] * static_cast<float> (rayleigh);
        }

        // Find peak in weighted autocorrelation
        int   bestLagOff = 0;
        float bestWtVal  = 0.0f;
        for (int i = 0; i < acLen; ++i)
        {
            if (weightedAc[static_cast<size_t> (i)] > bestWtVal)
            {
                bestWtVal  = weightedAc[static_cast<size_t> (i)];
                bestLagOff = i;
            }
        }

        // Parabolic interpolation on UNWEIGHTED autocorrelation
        double refinedLag = static_cast<double> (bestLagOff + minLag);
        if (bestLagOff > 0 && bestLagOff < acLen - 1)
        {
            float a = autocorr[static_cast<size_t> (bestLagOff - 1)];
            float b = autocorr[static_cast<size_t> (bestLagOff)];
            float c = autocorr[static_cast<size_t> (bestLagOff + 1)];
            float denom = a - 2.0f * b + c;
            if (std::abs (denom) > 1e-10f)
                refinedLag += static_cast<double> (0.5f * (a - c) / denom);
        }

        double acBpm = 60.0 * odfRate / refinedLag;

        // Resolve to 80-160 BPM range
        while (acBpm < 80.0 && acBpm > 0.0) acBpm *= 2.0;
        while (acBpm > 160.0)               acBpm /= 2.0;

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 4: Fine BPM sweep via comb-filter scoring
        //
        // For each candidate BPM in a ±3 BPM window around the
        // autocorrelation estimate, evaluate how well a periodic comb
        // aligns with the onset function. The candidate with the highest
        // total onset energy at grid positions wins.
        //
        // This simultaneously finds the optimal period AND phase.
        // Resolution: 0.01 BPM steps → ~600 candidates × ~40 phases.
        // ================================================================
        double bestCombBpm   = acBpm;
        double bestCombPhase = 0.0;
        double bestCombScore = -1.0;

        double bpmLo   = juce::jmax (40.0, acBpm - 3.0);
        double bpmHi   = juce::jmin (220.0, acBpm + 3.0);
        double bpmStep = 0.01;

        for (double candBpm = bpmLo; candBpm <= bpmHi; candBpm += bpmStep)
        {
            double periodFrames = 60.0 * odfRate / candBpm;
            int    iPeriod      = static_cast<int> (std::round (periodFrames));
            if (iPeriod < 2)
                continue;

            // Test all phase offsets within one beat period
            int numPhases = iPeriod;

            for (int phase = 0; phase < numPhases; ++phase)
            {
                double score = 0.0;
                int    count = 0;

                // Walk the grid and sum onset energy at each grid position.
                // Use fractional accumulation with the exact period.
                double pos = static_cast<double> (phase);
                while (pos < static_cast<double> (numHops))
                {
                    int idx = static_cast<int> (std::round (pos));
                    if (idx >= 0 && idx < numHops)
                    {
                        score += static_cast<double> (onsetFunction[static_cast<size_t> (idx)]);
                        ++count;
                    }
                    pos += periodFrames;
                }

                if (count > 0 && score > bestCombScore)
                {
                    bestCombScore = score;
                    bestCombBpm   = candBpm;
                    bestCombPhase = static_cast<double> (phase);
                }
            }
        }

        if (shouldExit())
            return nullptr;

        // ================================================================
        // Step 5: Sub-frame phase refinement
        //
        // Sweep the phase in ±1 frame steps at 0.1-frame resolution
        // around the winning phase for maximum precision.
        // ================================================================
        {
            double periodFrames = 60.0 * odfRate / bestCombBpm;
            double bestFinePhase = bestCombPhase;
            double bestFineScore = bestCombScore;

            for (double dp = -1.0; dp <= 1.0; dp += 0.1)
            {
                double testPhase = bestCombPhase + dp;
                if (testPhase < 0.0)
                    testPhase += periodFrames;

                double score = 0.0;
                double pos   = testPhase;
                while (pos < static_cast<double> (numHops))
                {
                    int idx = static_cast<int> (std::round (pos));
                    if (idx >= 0 && idx < numHops)
                        score += static_cast<double> (onsetFunction[static_cast<size_t> (idx)]);
                    pos += periodFrames;
                }

                if (score > bestFineScore)
                {
                    bestFineScore = score;
                    bestFinePhase = testPhase;
                }
            }

            bestCombPhase = bestFinePhase;
            bestCombScore = bestFineScore;
        }

        // ================================================================
        // Step 6: Confidence
        //
        // Compare the best comb-filter score to the mean score across a
        // range of candidate BPMs. A sharp peak = clear tempo. Also
        // consider the ratio of the best score to a "random phase" baseline.
        // ================================================================
        double periodFrames = 60.0 * odfRate / bestCombBpm;
        int numBeats = static_cast<int> (static_cast<double> (numHops) / periodFrames);

        // Compute mean onset energy per frame as baseline
        double totalOnset = 0.0;
        for (int i = 0; i < numHops; ++i)
            totalOnset += static_cast<double> (onsetFunction[static_cast<size_t> (i)]);
        double meanOnsetPerFrame = totalOnset / static_cast<double> (juce::jmax (1, numHops));

        // Confidence: how much stronger are onsets at beat positions vs average?
        float conf = 0.0f;
        if (meanOnsetPerFrame > 0.0 && numBeats > 0)
        {
            double avgOnsetAtBeats = bestCombScore / static_cast<double> (numBeats);
            double ratio = avgOnsetAtBeats / meanOnsetPerFrame;
            // ratio > 1.0 means beats land on stronger-than-average onsets.
            // Map [1.0, 3.0] → [0.0, 1.0]
            conf = juce::jlimit (0.0f, 1.0f, static_cast<float> ((ratio - 1.0) / 2.0));
        }

        double finalBpm    = bestCombBpm;
        double finalPeriod = 60.0 * sr / finalBpm;
        double finalAnchor = bestCombPhase * static_cast<double> (hopSize);

        // Normalise anchor to [0, period)
        while (finalAnchor < 0.0)
            finalAnchor += finalPeriod;
        while (finalAnchor >= finalPeriod)
            finalAnchor -= finalPeriod;

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
        result->anchorSample        = static_cast<int64_t> (std::round (finalAnchor));
        result->beatIntervalSamples = finalPeriod;
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
