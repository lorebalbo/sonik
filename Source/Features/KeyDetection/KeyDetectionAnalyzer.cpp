#include "KeyDetectionAnalyzer.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <vector>
#include <complex>
#include <algorithm>
#include <numeric>

// ============================================================================
// Simple radix-2 FFT (same as BeatGridAnalyzer — avoids juce_dsp dependency)
// ============================================================================

namespace
{

void fftRadix2 (std::vector<std::complex<float>>& data)
{
    auto n = static_cast<int> (data.size());
    if (n <= 1)
        return;

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
// Krumhansl-Schmuckler key profiles
// ============================================================================

namespace KeyProfiles
{

// Krumhansl key profiles (1990)
// Correlation weights for each pitch class relative to the tonic

static constexpr double major[] = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
    2.52, 5.19, 2.39, 3.66, 2.29, 2.88
};

static constexpr double minor[] = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
    2.54, 4.75, 3.98, 2.69, 3.34, 3.17
};

} // namespace KeyProfiles

// ============================================================================
// AnalysisJob
// ============================================================================

class KeyDetectionAnalyzer::AnalysisJob : public juce::ThreadPoolJob
{
public:
    AnalysisJob (KeyDetectionAnalyzer& owner,
                 const juce::String& hash,
                 const juce::String& path,
                 AudioBufferHolder::Ptr buf,
                 Callback cb)
        : juce::ThreadPoolJob ("KeyAnalysis_" + hash),
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
        if (cached.has_value() && cached->keyIndex >= 0)
        {
            if (cached->keyManuallyAdjusted)
            {
                deliverResult (cached->keyIndex, cached->keyConfidence);
                return jobHasFinished;
            }

            deliverResult (cached->keyIndex, cached->keyConfidence);
            return jobHasFinished;
        }

        if (shouldExit())
            return jobHasFinished;

        // 2. Run analysis
        int   detectedKey  = -1;
        float detectedConf = 0.0f;
        analyzeBuffer (detectedKey, detectedConf);

        if (shouldExit())
        {
            deliverResult (-1, 0.0f);
            return jobHasFinished;
        }

        // 3. Cache to database
        {
            auto existing = analyzer.db.loadTrackData (filePath, contentHash);

            juce::String cuePointsJson;
            juce::String beatgridJson;

            if (existing.has_value())
            {
                // Don't overwrite manually adjusted key
                if (existing->keyManuallyAdjusted)
                {
                    deliverResult (existing->keyIndex, existing->keyConfidence);
                    return jobHasFinished;
                }

                cuePointsJson = existing->cuePointsJson;
                beatgridJson  = existing->beatgridJson;
            }

            if (detectedKey >= 0)
            {
                analyzer.db.saveTrackData (filePath, contentHash,
                                           cuePointsJson, beatgridJson,
                                           detectedKey, detectedConf, false);
            }
        }

        // 4. Deliver
        deliverResult (detectedKey, detectedConf);
        return jobHasFinished;
    }

private:
    void analyzeBuffer (int& outKey, float& outConf)
    {
        const auto& audioBuffer = buffer->getBuffer();
        const auto  sr          = buffer->getSampleRate();
        const auto  numFrames   = buffer->getNumFrames();
        const int   numChannels = audioBuffer.getNumChannels();

        if (numFrames <= 0 || numChannels < 1 || sr <= 0.0)
            return;

        // ================================================================
        // Step 1: Downmix to mono
        // ================================================================
        const float* channelL = audioBuffer.getReadPointer (0);
        const float* channelR = numChannels >= 2 ? audioBuffer.getReadPointer (1) : channelL;

        std::vector<float> mono (static_cast<size_t> (numFrames));
        for (int64_t i = 0; i < numFrames; ++i)
            mono[static_cast<size_t> (i)] = (channelL[i] + channelR[i]) * 0.5f;

        if (shouldExit())
            return;

        // ================================================================
        // Step 2: Select representative segment
        //
        // Analyze 60 seconds starting from 15% into the track.
        // If track < 90 seconds, analyze the entire track.
        // ================================================================
        int64_t segStart = 0;
        int64_t segEnd   = numFrames;

        double trackDuration = static_cast<double> (numFrames) / sr;
        if (trackDuration > 90.0)
        {
            segStart = static_cast<int64_t> (0.15 * static_cast<double> (numFrames));
            segEnd   = juce::jmin (numFrames,
                                   segStart + static_cast<int64_t> (60.0 * sr));
        }

        int64_t segLen = segEnd - segStart;
        if (segLen <= 0)
            return;

        // ================================================================
        // Step 3: Chromagram extraction
        //
        // STFT with 4096-sample window, 2048-sample hop, Hann window.
        // Map each FFT bin to one of 12 pitch classes using log-frequency
        // binning aligned to A440.
        // ================================================================
        constexpr int fftSize = 4096;
        constexpr int hopSize = 2048;

        std::vector<float> window (fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[static_cast<size_t> (i)] = 0.5f * (1.0f - std::cos (
                2.0f * juce::MathConstants<float>::pi * static_cast<float> (i)
                    / static_cast<float> (fftSize)));

        int numHops = static_cast<int> ((segLen - fftSize) / hopSize) + 1;
        if (numHops <= 0)
            return;

        const int fftN   = nextPowerOfTwo (fftSize);
        const int numBins = fftN / 2 + 1;

        // Precompute bin-to-chroma and bin-to-octave mapping
        // freq = bin * sr / fftN
        // MIDI note = 69 + 12 * log2(freq / 440)
        // Pitch class = MIDI note % 12
        // Octave = MIDI note / 12
        //
        // Restrict to 130 Hz (C3) – 4200 Hz to avoid bass bias
        // and extreme highs where harmonics are sparse.
        std::vector<int> binToChroma (static_cast<size_t> (numBins), -1);
        std::vector<int> binToOctave (static_cast<size_t> (numBins), -1);
        int minOctave = 100, maxOctave = -1;

        for (int bin = 1; bin < numBins; ++bin)
        {
            double freq = static_cast<double> (bin) * sr / static_cast<double> (fftN);
            if (freq < 130.0 || freq > 4200.0)
                continue;

            double midiNote = 69.0 + 12.0 * std::log2 (freq / 440.0);
            int roundedMidi = static_cast<int> (std::round (midiNote));
            int pitchClass = roundedMidi % 12;
            if (pitchClass < 0)
                pitchClass += 12;
            int octave = roundedMidi / 12;

            binToChroma[static_cast<size_t> (bin)] = pitchClass;
            binToOctave[static_cast<size_t> (bin)] = octave;

            if (octave < minOctave) minOctave = octave;
            if (octave > maxOctave) maxOctave = octave;
        }

        int numOctaves = (maxOctave >= minOctave) ? (maxOctave - minOctave + 1) : 0;
        if (numOctaves <= 0)
            return;

        // Per-octave chroma accumulation, then normalize each octave
        // before summing. This prevents bass-heavy tracks from biasing
        // the chroma toward low-pitched notes.
        // Layout: octaveChroma[octave][pitchClass]
        std::vector<std::vector<double>> octaveChroma (
            static_cast<size_t> (numOctaves),
            std::vector<double> (12, 0.0));

        for (int hop = 0; hop < numHops; ++hop)
        {
            if (hop % 500 == 0 && shouldExit())
                return;

            int64_t startSample = segStart + static_cast<int64_t> (hop) * hopSize;

            std::vector<std::complex<float>> fftBuf (static_cast<size_t> (fftN), { 0.0f, 0.0f });
            for (int i = 0; i < fftSize; ++i)
            {
                int64_t idx = startSample + i;
                float sample = (idx < numFrames) ? mono[static_cast<size_t> (idx)] : 0.0f;
                fftBuf[static_cast<size_t> (i)] = { sample * window[static_cast<size_t> (i)], 0.0f };
            }

            fftRadix2 (fftBuf);

            // Map magnitude to per-octave chroma bins (using magnitude, not power)
            for (int bin = 1; bin < numBins; ++bin)
            {
                int pc  = binToChroma[static_cast<size_t> (bin)];
                int oct = binToOctave[static_cast<size_t> (bin)];
                if (pc >= 0 && oct >= 0)
                {
                    float mag = std::abs (fftBuf[static_cast<size_t> (bin)]);
                    int octIdx = oct - minOctave;
                    octaveChroma[static_cast<size_t> (octIdx)][static_cast<size_t> (pc)]
                        += static_cast<double> (mag);
                }
            }
        }

        if (shouldExit())
            return;

        // ================================================================
        // Step 4: Per-octave normalization, then sum into final chroma
        // ================================================================
        std::vector<double> chromaSum (12, 0.0);

        for (int o = 0; o < numOctaves; ++o)
        {
            auto& oc = octaveChroma[static_cast<size_t> (o)];
            double octMax = *std::max_element (oc.begin(), oc.end());
            if (octMax <= 0.0)
                continue;

            for (int pc = 0; pc < 12; ++pc)
                chromaSum[static_cast<size_t> (pc)] += oc[static_cast<size_t> (pc)] / octMax;
        }

        double chromaMax = *std::max_element (chromaSum.begin(), chromaSum.end());
        if (chromaMax <= 0.0)
            return;

        for (auto& v : chromaSum)
            v /= chromaMax;

        // ================================================================
        // Step 5: Krumhansl-Schmuckler key profile correlation
        //
        // Correlate the chroma profile against all 24 key templates
        // (12 roots x 2 modes) using Pearson correlation.
        // ================================================================
        double bestCorr       = -2.0;
        double secondBestCorr = -2.0;
        int    bestKey        = -1;

        for (int root = 0; root < 12; ++root)
        {
            for (int mode = 0; mode < 2; ++mode) // 0 = major, 1 = minor
            {
                const double* profile = (mode == 0) ? KeyProfiles::major : KeyProfiles::minor;

                // Rotate profile to start at 'root'
                double rotated[12];
                for (int i = 0; i < 12; ++i)
                    rotated[i] = profile[((i - root) + 12) % 12];

                // Pearson correlation
                double meanX = 0.0, meanY = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    meanX += chromaSum[static_cast<size_t> (i)];
                    meanY += rotated[i];
                }
                meanX /= 12.0;
                meanY /= 12.0;

                double sumXY = 0.0, sumXX = 0.0, sumYY = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    double dx = chromaSum[static_cast<size_t> (i)] - meanX;
                    double dy = rotated[i] - meanY;
                    sumXY += dx * dy;
                    sumXX += dx * dx;
                    sumYY += dy * dy;
                }

                double denom = std::sqrt (sumXX * sumYY);
                double corr  = (denom > 1e-12) ? sumXY / denom : 0.0;

                int canonicalKey = root * 2 + mode;

                if (corr > bestCorr)
                {
                    secondBestCorr = bestCorr;
                    bestCorr       = corr;
                    bestKey        = canonicalKey;
                }
                else if (corr > secondBestCorr)
                {
                    secondBestCorr = corr;
                }
            }
        }

        // ================================================================
        // Step 6: Confidence
        //
        // conf = 1.0 - (secondBest / best), clamped to [0, 1]
        // Low threshold because relative major/minor always correlate closely.
        // ================================================================
        float conf = 0.0f;
        if (bestCorr > 0.0)
        {
            double ratio = (secondBestCorr > 0.0)
                         ? (1.0 - secondBestCorr / bestCorr)
                         : 1.0;
            conf = juce::jlimit (0.0f, 1.0f, static_cast<float> (ratio));
        }

        if (conf < 0.02f || bestKey < 0 || bestCorr < 0.3)
        {
            outKey  = -1;
            outConf = conf;
        }
        else
        {
            outKey  = bestKey;
            outConf = conf;
        }
    }

    void deliverResult (int keyIndex, float confidence)
    {
        auto cb   = callback;
        auto hash = contentHash;
        juce::MessageManager::callAsync ([cb, hash, keyIndex, confidence]()
        {
            if (cb)
                cb (hash, keyIndex, confidence);
        });
    }

    KeyDetectionAnalyzer& analyzer;
    juce::String          contentHash;
    juce::String          filePath;
    AudioBufferHolder::Ptr buffer;
    Callback              callback;
};

// ============================================================================
// KeyDetectionAnalyzer implementation
// ============================================================================

KeyDetectionAnalyzer::KeyDetectionAnalyzer (TrackDatabase& database)
    : db (database)
{
}

KeyDetectionAnalyzer::~KeyDetectionAnalyzer()
{
    threadPool.removeAllJobs (true, 5000);
}

void KeyDetectionAnalyzer::analyze (const juce::String& contentHash,
                                     const juce::String& filePath,
                                     AudioBufferHolder::Ptr buffer,
                                     Callback callback)
{
    threadPool.addJob (new AnalysisJob (*this, contentHash, filePath,
                                        std::move (buffer), std::move (callback)),
                       true);
}
