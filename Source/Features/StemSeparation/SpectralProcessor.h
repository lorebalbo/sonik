#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

/// STFT / iSTFT processor matching htdemucs training configuration.
/// FFT size: 4096, hop size: 1024, Hann window.
/// All processing is done on a background thread — never on the audio thread.
class SpectralProcessor
{
public:
    static constexpr int kFFTSize    = 4096;
    static constexpr int kHopSize    = 1024;
    static constexpr int kWindowSize = 4096;

    SpectralProcessor();
    ~SpectralProcessor() = default;

    SpectralProcessor (const SpectralProcessor&) = delete;
    SpectralProcessor& operator= (const SpectralProcessor&) = delete;

    /// Number of frequency bins in the output spectrogram (N/2 + 1).
    static constexpr int numBins() { return kFFTSize / 2 + 1; }

    /// Compute the number of STFT frames for a given signal length.
    static int numFrames (int signalLength);

    /// Perform forward STFT on a mono signal.
    /// Output: interleaved [real, imag] for each bin, for each frame.
    /// Output size: numFrames * numBins * 2.
    void forward (const float* signal, int signalLength,
                  std::vector<float>& output) const;

    /// Perform inverse STFT (overlap-add) from complex spectrogram to time-domain signal.
    /// Input: interleaved [real, imag] per bin, per frame. Same layout as forward() output.
    /// Output signal length is returned.
    int inverse (const float* spectrogramData, int nFrames,
                 std::vector<float>& output) const;

private:
    void initWindow();

    /// JUCE FFT engine (log2 of 4096 = 12).
    juce::dsp::FFT fft;

    /// Hann window, pre-computed.
    std::vector<float> window;
};
