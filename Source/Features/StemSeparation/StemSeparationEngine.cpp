#include "StemSeparationEngine.h"
#include "ModelManager.h"
#include <cmath>

StemSeparationEngine::StemSeparationEngine (const juce::String& deckId_,
                                             const juce::String& contentHash_,
                                             AudioBufferHolder::Ptr sourceBuffer_,
                                             OnnxInference& inference,
                                             StemCache& cache,
                                             juce::ValueTree stemsNode_,
                                             double deviceRate,
                                             CompletionCallback callback)
    : juce::ThreadPoolJob ("StemSep_" + deckId_),
      deckId (deckId_),
      contentHash (contentHash_),
      sourceBuffer (std::move (sourceBuffer_)),
      onnxInference (inference),
      stemCache (cache),
      stemsNode (std::move (stemsNode_)),
      deviceSampleRate (deviceRate),
      completionCallback (std::move (callback))
{
}

// ============================================================================
// Main pipeline
// ============================================================================

StemSeparationEngine::JobStatus StemSeparationEngine::runJob()
{
    lastProgressTime = juce::Time::getMillisecondCounterHiRes();

    // Update status to "separating"
    reportProgress (0.0f);

    // 1. Insert pending DB record before any file writes
    stemCache.insertPendingRecord (contentHash,
                                    juce::String (ModelManager::getModelVersion()));

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    // 2. Resample source to model rate (44100 Hz) if needed
    juce::AudioBuffer<float> modelRateBuffer;
    if (! resampleToModelRate (modelRateBuffer))
        return jobHasFinished;

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.05f);

    // 3. STFT — left and right channels
    std::vector<float> specL, specR;
    int nFrames = 0;

    if (! performSTFT (modelRateBuffer, specL, specR, nFrames))
        return jobHasFinished;

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.15f);

    // 4. ONNX inference — single stereo call produces 4 stem spectrograms (L+R)
    std::vector<std::vector<float>> stemSpecsL, stemSpecsR;

    if (! performInference (specL, specR, nFrames, stemSpecsL, stemSpecsR))
        return jobHasFinished;

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.70f);

    // 5. iSTFT — convert each stem spectrogram back to time domain
    const int originalLength = modelRateBuffer.getNumSamples();

    std::array<std::vector<float>, StemData::NumStems> stemSignalsL;
    if (! performISTFT (stemSpecsL, nFrames, originalLength, stemSignalsL))
        return jobHasFinished;

    std::array<std::vector<float>, StemData::NumStems> stemSignalsR;
    if (! performISTFT (stemSpecsR, nFrames, originalLength, stemSignalsR))
        return jobHasFinished;

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.80f);

    // 6. Assemble stereo AudioBufferHolder for each stem
    auto stemResult = new StemData();
    if (! assembleStemBuffers (stemSignalsL, stemSignalsR, originalLength, *stemResult))
        return jobHasFinished;

    StemData::Ptr stemPtr (stemResult);

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.85f);

    // 7. Write stems to disk cache
    // Determine write sample rate: if device rate != model rate, we write at device rate
    // so that cached files match the device rate directly.
    double writeSampleRate = deviceSampleRate;

    // Resample stems back to device rate if needed, then write
    if (std::abs (deviceSampleRate - kModelSampleRate) > 0.01)
    {
        // Stems are currently at model rate — resample to device rate
        for (int s = 0; s < StemData::NumStems; ++s)
        {
            auto stemHolder = stemPtr->stems[static_cast<size_t> (s)];
            const auto& buf = stemHolder->getBuffer();

            double ratio = kModelSampleRate / deviceSampleRate;
            int64_t outFrames = static_cast<int64_t> (
                std::ceil (static_cast<double> (buf.getNumSamples()) / ratio));

            juce::AudioBuffer<float> resampled (buf.getNumChannels(),
                                                  static_cast<int> (outFrames));
            resampled.clear();

            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                juce::LagrangeInterpolator interpolator;
                interpolator.process (ratio,
                                      buf.getReadPointer (ch),
                                      resampled.getWritePointer (ch),
                                      static_cast<int> (outFrames));
            }

            stemPtr->stems[static_cast<size_t> (s)] =
                new AudioBufferHolder (std::move (resampled), deviceSampleRate, outFrames);
        }
    }

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.90f);

    if (! stemCache.writeStemsToDisk (contentHash,
                                       juce::String (ModelManager::getModelVersion()),
                                       *stemPtr, writeSampleRate))
    {
        reportError ("Failed to write stem files to disk (disk full or permissions error)");
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    if (shouldExit())
        return jobHasFinished;

    reportProgress (1.0f);

    // 8. Deliver result via callback
    auto cb = completionCallback;
    auto dk = deckId;
    auto sp = stemPtr;
    juce::MessageManager::callAsync ([cb, dk, sp]()
    {
        cb (dk, sp, {});
    });

    return jobHasFinished;
}

// ============================================================================
// Pipeline stages
// ============================================================================

bool StemSeparationEngine::resampleToModelRate (juce::AudioBuffer<float>& output)
{
    const auto& srcBuf = sourceBuffer->getBuffer();
    const int numChannels = srcBuf.getNumChannels();
    const int numSamples  = srcBuf.getNumSamples();
    const double srcRate  = sourceBuffer->getSampleRate();

    if (std::abs (srcRate - kModelSampleRate) < 0.01)
    {
        // No resampling needed — copy
        output = srcBuf;
        return true;
    }

    double ratio = srcRate / kModelSampleRate;
    int64_t outFrames = static_cast<int64_t> (
        std::ceil (static_cast<double> (numSamples) / ratio));

    output.setSize (numChannels, static_cast<int> (outFrames));
    output.clear();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio,
                              srcBuf.getReadPointer (ch),
                              output.getWritePointer (ch),
                              static_cast<int> (outFrames));
    }

    return true;
}

bool StemSeparationEngine::performSTFT (const juce::AudioBuffer<float>& input,
                                          std::vector<float>& specL,
                                          std::vector<float>& specR,
                                          int& nFrames)
{
    const int numSamples = input.getNumSamples();
    nFrames = SpectralProcessor::numFrames (numSamples);

    if (nFrames <= 0)
    {
        reportError ("Track too short for STFT processing");
        return false;
    }

    // Left channel STFT
    spectralProcessor.forward (input.getReadPointer (0), numSamples, specL);

    if (shouldExit())
    {
        stemCache.deletePartialFiles (contentHash);
        return false;
    }

    // Right channel STFT (or duplicate if mono)
    if (input.getNumChannels() >= 2)
        spectralProcessor.forward (input.getReadPointer (1), numSamples, specR);
    else
        specR = specL;

    return true;
}

bool StemSeparationEngine::performInference (const std::vector<float>& specL,
                                               const std::vector<float>& specR,
                                               int nFrames,
                                               std::vector<std::vector<float>>& stemSpecsL,
                                               std::vector<std::vector<float>>& stemSpecsR)
{
    // htdemucs model expects input shape: [batch=1, channels=2, freq_bins, time_frames]
    // with interleaved real+imaginary -> channels = 2 * 2 = 4 (re_L, im_L, re_R, im_R)
    // Simplified: we pack as [1, 2, numBins*2, nFrames] where channel dim has L and R
    // Each "channel" has interleaved re/im per bin.
    //
    // Actual htdemucs: input is waveform or spectrogram depending on model export.
    // For an ONNX spectrogram model: [batch, 2, freq_bins, frames] with complex as separate channels.
    //
    // We use a practical layout: [1, 4, numBins, nFrames]
    // channels: [re_L, im_L, re_R, im_R]

    const int nBins = SpectralProcessor::numBins();

    // Reshape from interleaved [frame * bins * 2] to [channel, bins, frames]
    // specL layout: for each frame f, for each bin b: [real, imag]
    std::vector<float> inputTensor (static_cast<size_t> (4 * nBins * nFrames), 0.0f);

    auto fillChannel = [&] (const std::vector<float>& spec, int reChannelIdx, int imChannelIdx)
    {
        for (int f = 0; f < nFrames; ++f)
        {
            if (shouldExit()) return;

            for (int b = 0; b < nBins; ++b)
            {
                size_t srcIdx = static_cast<size_t> (f * nBins * 2 + b * 2);
                size_t reIdx  = static_cast<size_t> (reChannelIdx * nBins * nFrames + b * nFrames + f);
                size_t imIdx  = static_cast<size_t> (imChannelIdx * nBins * nFrames + b * nFrames + f);

                inputTensor[reIdx] = spec[srcIdx];      // real
                inputTensor[imIdx] = spec[srcIdx + 1];  // imag
            }
        }
    };

    fillChannel (specL, 0, 1); // re_L, im_L
    if (shouldExit()) return false;
    fillChannel (specR, 2, 3); // re_R, im_R
    if (shouldExit()) return false;

    // Input shape: [1, 4, numBins, nFrames]
    std::vector<int64_t> inputShape = { 1, 4, static_cast<int64_t> (nBins),
                                         static_cast<int64_t> (nFrames) };

    // Run inference
    auto result = onnxInference.run (inputTensor, inputShape, "mix", "stems");

    if (! result.success)
    {
        reportError (juce::String (result.errorMessage));
        return false;
    }

    // Expected output shape: [1, stems=4, channels=2, numBins, nFrames]
    // Total elements = 4 * 2 * numBins * nFrames
    // Per stem: 2 * numBins * nFrames (L channel then R channel)

    const size_t totalOutput = result.outputData.size();
    const size_t perStem = totalOutput / StemData::NumStems;
    const size_t perChannel = perStem / 2; // L and R per stem

    stemSpecsL.resize (StemData::NumStems);
    stemSpecsR.resize (StemData::NumStems);

    for (int s = 0; s < StemData::NumStems; ++s)
    {
        if (shouldExit()) return false;

        auto& specOutL = stemSpecsL[static_cast<size_t> (s)];
        auto& specOutR = stemSpecsR[static_cast<size_t> (s)];

        const float* stemData = result.outputData.data()
                                + static_cast<size_t> (s) * perStem;

        if (perChannel == static_cast<size_t> (nBins * nFrames))
        {
            // Output layout: [bins, frames] per channel — reshape to [frame, bin, re/im]
            // Channel 0 = L, Channel 1 = R
            specOutL.resize (static_cast<size_t> (nFrames * nBins * 2));
            specOutR.resize (static_cast<size_t> (nFrames * nBins * 2));

            const float* chL = stemData;                     // channel 0
            const float* chR = stemData + perChannel;        // channel 1

            for (int f = 0; f < nFrames; ++f)
            {
                for (int b = 0; b < nBins; ++b)
                {
                    size_t dstIdx = static_cast<size_t> (f * nBins * 2 + b * 2);
                    size_t srcIdx = static_cast<size_t> (b * nFrames + f);

                    specOutL[dstIdx]     = chL[srcIdx]; // real
                    specOutL[dstIdx + 1] = 0.0f;        // imag (separate pass below)
                    specOutR[dstIdx]     = chR[srcIdx];
                    specOutR[dstIdx + 1] = 0.0f;
                }
            }
        }
        else if (perChannel == static_cast<size_t> (nBins * nFrames * 2))
        {
            // Output has re+im per channel: [re_ch, im_ch] each of size [bins, frames]
            specOutL.resize (static_cast<size_t> (nFrames * nBins * 2));
            specOutR.resize (static_cast<size_t> (nFrames * nBins * 2));

            const float* chL = stemData;
            const float* chR = stemData + perChannel;
            const size_t halfCh = static_cast<size_t> (nBins * nFrames);

            for (int f = 0; f < nFrames; ++f)
            {
                for (int b = 0; b < nBins; ++b)
                {
                    size_t dstIdx = static_cast<size_t> (f * nBins * 2 + b * 2);
                    size_t srcIdx = static_cast<size_t> (b * nFrames + f);

                    specOutL[dstIdx]     = chL[srcIdx];             // real L
                    specOutL[dstIdx + 1] = chL[halfCh + srcIdx];    // imag L
                    specOutR[dstIdx]     = chR[srcIdx];             // real R
                    specOutR[dstIdx + 1] = chR[halfCh + srcIdx];    // imag R
                }
            }
        }
        else
        {
            // Fallback: split raw data evenly between L and R
            specOutL.assign (stemData, stemData + static_cast<ptrdiff_t> (perChannel));
            specOutR.assign (stemData + static_cast<ptrdiff_t> (perChannel),
                              stemData + static_cast<ptrdiff_t> (perStem));
        }
    }

    return true;
}

bool StemSeparationEngine::performISTFT (const std::vector<std::vector<float>>& stemSpecs,
                                           int nFrames, int originalLength,
                                           std::array<std::vector<float>, StemData::NumStems>& stemSignals)
{
    for (int s = 0; s < StemData::NumStems; ++s)
    {
        if (shouldExit())
        {
            stemCache.deletePartialFiles (contentHash);
            return false;
        }

        int outputLen = spectralProcessor.inverse (stemSpecs[static_cast<size_t> (s)].data(),
                                                     nFrames,
                                                     stemSignals[static_cast<size_t> (s)]);

        // Trim to original length
        if (outputLen > originalLength)
            stemSignals[static_cast<size_t> (s)].resize (static_cast<size_t> (originalLength));
        else if (outputLen < originalLength)
            stemSignals[static_cast<size_t> (s)].resize (static_cast<size_t> (originalLength), 0.0f);
    }

    return true;
}

bool StemSeparationEngine::assembleStemBuffers (
    const std::array<std::vector<float>, StemData::NumStems>& stemSignalsL,
    const std::array<std::vector<float>, StemData::NumStems>& stemSignalsR,
    int originalLength,
    StemData& output)
{
    for (int s = 0; s < StemData::NumStems; ++s)
    {
        if (shouldExit())
        {
            stemCache.deletePartialFiles (contentHash);
            return false;
        }

        juce::AudioBuffer<float> buf (2, originalLength);
        buf.clear();

        // Left channel
        const auto& sigL = stemSignalsL[static_cast<size_t> (s)];
        int lenL = juce::jmin (static_cast<int> (sigL.size()), originalLength);
        if (lenL > 0)
            std::memcpy (buf.getWritePointer (0), sigL.data(),
                          static_cast<size_t> (lenL) * sizeof (float));

        // Right channel
        const auto& sigR = stemSignalsR[static_cast<size_t> (s)];
        int lenR = juce::jmin (static_cast<int> (sigR.size()), originalLength);
        if (lenR > 0)
            std::memcpy (buf.getWritePointer (1), sigR.data(),
                          static_cast<size_t> (lenR) * sizeof (float));

        output.stems[static_cast<size_t> (s)] =
            new AudioBufferHolder (std::move (buf), kModelSampleRate, originalLength);
    }

    return true;
}

// ============================================================================
// Progress & error reporting
// ============================================================================

void StemSeparationEngine::reportProgress (float prog)
{
    // Throttle to at most once per second
    double now = juce::Time::getMillisecondCounterHiRes();
    if (prog < 1.0f && (now - lastProgressTime) < 1000.0)
        return;

    lastProgressTime = now;

    auto node = stemsNode;
    juce::MessageManager::callAsync ([node, prog]() mutable
    {
        if (node.isValid())
        {
            node.setProperty (IDs::progress, prog, nullptr);
            if (prog > 0.0f && prog < 1.0f)
                node.setProperty (IDs::status, "separating", nullptr);
        }
    });
}

void StemSeparationEngine::reportError (const juce::String& message)
{
    stemCache.deletePartialFiles (contentHash);

    auto node = stemsNode;
    auto dk   = deckId;
    auto cb   = completionCallback;
    juce::MessageManager::callAsync ([node, message, dk, cb]() mutable
    {
        if (node.isValid())
        {
            node.setProperty (IDs::status,    "error",  nullptr);
            node.setProperty (IDs::stemError, message,  nullptr);
            node.setProperty (IDs::progress,  0.0f,     nullptr);
        }
        cb (dk, nullptr, message);
    });
}
