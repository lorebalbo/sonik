#include "OverviewWaveform.h"
#include <cmath>

OverviewWaveform::OverviewWaveform()
{
    setOpaque (true);
}

OverviewWaveform::~OverviewWaveform()
{
    stopTimer();
}

void OverviewWaveform::setWaveformData (WaveformData::Ptr data)
{
    waveformData = std::move (data);

    if (waveformData != nullptr)
    {
        totalSamples = waveformData->totalSamples;
        startTimerHz (timerHz);
    }
    else
    {
        totalSamples = 0;
        stopTimer();
    }

    cachedWidth = 0; // force rebuild
    repaint();
}

void OverviewWaveform::setAudioState (DeckAudioState* state)
{
    audioState = state;
}

void OverviewWaveform::setTotalSamples (int64_t total)
{
    totalSamples = total;
}

void OverviewWaveform::setVisibleRange (int64_t startSample, int64_t endSample)
{
    visibleStart = startSample;
    visibleEnd   = endSample;
}

void OverviewWaveform::setHotCues (const std::array<HotCueInfo, 8>& cues)
{
    hotCues = cues;
    repaint();
}

void OverviewWaveform::timerCallback()
{
    repaint();
}

void OverviewWaveform::rebuildImage()
{
    auto w = getWidth();
    auto h = getHeight();

    if (w <= 0 || h <= 0 || waveformData == nullptr)
        return;

    cachedImage = juce::Image (juce::Image::ARGB, w, h, true);
    cachedWidth  = w;
    cachedHeight = h;

    juce::Graphics ig (cachedImage);

    // Choose mipmap level based on how many samples per pixel
    double samplesPerPixel = static_cast<double> (totalSamples) / static_cast<double> (w);
    int level = waveformData->getBestLevel (samplesPerPixel);

    if (level < 0 || level >= static_cast<int> (waveformData->levels.size()))
        return;

    const auto& points = waveformData->levels[static_cast<size_t> (level)];
    if (points.empty())
        return;

    auto numPoints = static_cast<int> (points.size());
    float halfH = static_cast<float> (h) * 0.5f;

    // Three-band colors
    juce::Colour lowColor  (0xFFFF4444);
    juce::Colour midColor  (0xFF44FF44);
    juce::Colour highColor (0xFF44CCFF);

    for (int x = 0; x < w; ++x)
    {
        float frac = static_cast<float> (x) / static_cast<float> (w);
        int pointIdx = juce::jlimit (0, numPoints - 1,
                                     static_cast<int> (frac * static_cast<float> (numPoints)));
        const auto& pt = points[static_cast<size_t> (pointIdx)];

        float peak = juce::jmax (pt.peakL, pt.peakR);
        float rms  = juce::jmax (pt.rmsL, pt.rmsR);

        // Normalize energies for color weighting
        float totalEnergy = pt.energyLow + pt.energyMid + pt.energyHigh;

        juce::Colour barColor;
        if (totalEnergy > 0.0001f)
        {
            float wLow  = pt.energyLow / totalEnergy;
            float wMid  = pt.energyMid / totalEnergy;
            float wHigh = pt.energyHigh / totalEnergy;

            barColor = juce::Colour (
                static_cast<uint8_t> (juce::jlimit (0.0f, 255.0f, wLow * 255.0f + wHigh * 68.0f)),
                static_cast<uint8_t> (juce::jlimit (0.0f, 255.0f, wMid * 255.0f + wHigh * 204.0f)),
                static_cast<uint8_t> (juce::jlimit (0.0f, 255.0f, wLow * 68.0f + wHigh * 255.0f))
            );
        }
        else
        {
            barColor = juce::Colour (0xFF333333);
        }

        // Peak envelope
        float peakHeight = peak * halfH;
        ig.setColour (barColor.withAlpha (0.6f));
        ig.drawVerticalLine (x,
                             halfH - peakHeight,
                             halfH + peakHeight);

        // RMS region (brighter)
        float rmsHeight = rms * halfH;
        ig.setColour (barColor.withAlpha (0.9f));
        ig.drawVerticalLine (x,
                             halfH - rmsHeight,
                             halfH + rmsHeight);
    }
}

void OverviewWaveform::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.setColour (juce::Colour (0xFF0A0A0A));
    g.fillRect (bounds);

    if (waveformData == nullptr)
        return;

    // Rebuild image if size changed
    if (cachedWidth != getWidth() || cachedHeight != getHeight())
        rebuildImage();

    if (cachedImage.isValid())
        g.drawImageAt (cachedImage, 0, 0);

    // Viewport rectangle
    if (totalSamples > 0 && visibleEnd > visibleStart)
    {
        float xStart = static_cast<float> (visibleStart) / static_cast<float> (totalSamples)
                        * static_cast<float> (getWidth());
        float xEnd   = static_cast<float> (visibleEnd) / static_cast<float> (totalSamples)
                        * static_cast<float> (getWidth());

        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.fillRect (xStart, 0.0f, xEnd - xStart, static_cast<float> (getHeight()));

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.drawRect (xStart, 0.0f, xEnd - xStart, static_cast<float> (getHeight()), 1.0f);
    }

    // Playhead marker
    if (audioState != nullptr && totalSamples > 0)
    {
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        float xPos = static_cast<float> (pos) / static_cast<float> (totalSamples)
                     * static_cast<float> (getWidth());

        g.setColour (juce::Colours::white);
        g.drawVerticalLine (static_cast<int> (xPos), 0.0f, static_cast<float> (getHeight()));
    }

    // Hot cue markers (PRD-0012)
    if (totalSamples > 0)
    {
        float w = static_cast<float> (getWidth());

        for (const auto& cue : hotCues)
        {
            if (! cue.active || cue.positionSamples < 0)
                continue;

            float pixelX = static_cast<float> (cue.positionSamples)
                         / static_cast<float> (totalSamples) * w;

            if (pixelX < -4.0f || pixelX > w + 4.0f)
                continue;

            auto cueColour = HotCueColors::getColour (cue.colorIndex);

            // Triangle only (no vertical line) on overview
            juce::Path triangle;
            triangle.addTriangle (pixelX - 4.0f, 0.0f,
                                  pixelX + 4.0f, 0.0f,
                                  pixelX,        8.0f);
            g.setColour (cueColour);
            g.fillPath (triangle);
        }
    }
}

void OverviewWaveform::mouseDown (const juce::MouseEvent& e)
{
    if (waveformData == nullptr || totalSamples <= 0)
        return;

    float fraction = static_cast<float> (e.x) / static_cast<float> (getWidth());
    fraction = juce::jlimit (0.0f, 1.0f, fraction);
    int64_t samplePos = static_cast<int64_t> (fraction * static_cast<float> (totalSamples));

    if (onSeek)
        onSeek (samplePos);
}
