#include "SpectralProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <algorithm>

SpectralProcessor::SpectralProcessor()
    : fft (12) // log2(4096) = 12
{
    initWindow();
}

void SpectralProcessor::initWindow()
{
    window.resize (kWindowSize);
    for (int i = 0; i < kWindowSize; ++i)
        window[static_cast<size_t> (i)] =
            0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                     * static_cast<float> (i)
                                     / static_cast<float> (kWindowSize)));
}

int SpectralProcessor::numFrames (int signalLength)
{
    if (signalLength <= 0) return 0;
    return (signalLength - kWindowSize) / kHopSize + 1;
}

void SpectralProcessor::forward (const float* signal, int signalLength,
                                 std::vector<float>& output) const
{
    const int nFrames = numFrames (signalLength);
    if (nFrames <= 0)
    {
        output.clear();
        return;
    }

    const int nBins = numBins();
    output.resize (static_cast<size_t> (nFrames * nBins * 2));

    // Temporary buffer for FFT: needs 2 * fftSize floats (interleaved complex)
    std::vector<float> fftBuffer (static_cast<size_t> (kFFTSize * 2), 0.0f);

    for (int f = 0; f < nFrames; ++f)
    {
        const int offset = f * kHopSize;

        // Zero the buffer
        std::memset (fftBuffer.data(), 0, fftBuffer.size() * sizeof (float));

        // Apply window and copy into FFT buffer (real part only)
        for (int i = 0; i < kWindowSize; ++i)
            fftBuffer[static_cast<size_t> (i)] =
                signal[offset + i] * window[static_cast<size_t> (i)];

        // Perform FFT (JUCE performRealOnlyForwardTransform expects 2*N buffer)
        fft.performRealOnlyForwardTransform (fftBuffer.data(), true);

        // Extract complex bins: JUCE output is [re0, im0, re1, im1, ...] for N/2+1 bins
        auto* dst = output.data() + static_cast<size_t> (f * nBins * 2);
        for (int b = 0; b < nBins; ++b)
        {
            dst[b * 2]     = fftBuffer[static_cast<size_t> (b * 2)];
            dst[b * 2 + 1] = fftBuffer[static_cast<size_t> (b * 2 + 1)];
        }
    }
}

int SpectralProcessor::inverse (const float* spectrogramData, int nFrames,
                                std::vector<float>& output) const
{
    if (nFrames <= 0)
    {
        output.clear();
        return 0;
    }

    const int nBins = numBins();
    const int outputLength = (nFrames - 1) * kHopSize + kWindowSize;

    output.assign (static_cast<size_t> (outputLength), 0.0f);

    // Normalization accumulator for overlap-add
    std::vector<float> windowSum (static_cast<size_t> (outputLength), 0.0f);

    // Temporary buffer for iFFT
    std::vector<float> fftBuffer (static_cast<size_t> (kFFTSize * 2), 0.0f);

    for (int f = 0; f < nFrames; ++f)
    {
        const auto* src = spectrogramData + static_cast<size_t> (f * nBins * 2);

        // Fill the FFT buffer with the complex spectrum
        std::memset (fftBuffer.data(), 0, fftBuffer.size() * sizeof (float));
        for (int b = 0; b < nBins; ++b)
        {
            fftBuffer[static_cast<size_t> (b * 2)]     = src[b * 2];
            fftBuffer[static_cast<size_t> (b * 2 + 1)] = src[b * 2 + 1];
        }

        // Perform inverse FFT
        fft.performRealOnlyInverseTransform (fftBuffer.data());

        // Window and overlap-add
        const int offset = f * kHopSize;
        for (int i = 0; i < kWindowSize; ++i)
        {
            float w = window[static_cast<size_t> (i)];
            output[static_cast<size_t> (offset + i)] += fftBuffer[static_cast<size_t> (i)] * w;
            windowSum[static_cast<size_t> (offset + i)] += w * w;
        }
    }

    // Normalize by window sum (COLA condition)
    for (int i = 0; i < outputLength; ++i)
    {
        if (windowSum[static_cast<size_t> (i)] > 1e-8f)
            output[static_cast<size_t> (i)] /= windowSum[static_cast<size_t> (i)];
    }

    return outputLength;
}
