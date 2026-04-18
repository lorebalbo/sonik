#include "DetailWaveform.h"
#include <cmath>

DetailWaveform::DetailWaveform()
{
    setOpaque (true);
}

DetailWaveform::~DetailWaveform()
{
    stopTimer();
}

void DetailWaveform::setWaveformData (WaveformData::Ptr data)
{
    waveformData = std::move (data);

    if (waveformData != nullptr)
    {
        totalSamples = waveformData->totalSamples;
        sampleRate   = waveformData->sampleRate;
        startTimerHz (timerHz);
    }
    else
    {
        totalSamples = 0;
        stopTimer();
    }

    cachedWidth = 0;
    cachedZoomIndex = -1;
    repaint();
}

void DetailWaveform::setAudioState (DeckAudioState* state)
{
    audioState = state;
}

void DetailWaveform::setTotalSamples (int64_t total)
{
    totalSamples = total;
}

void DetailWaveform::setBeatGridData (BeatGridData::Ptr data)
{
    beatGridData = std::move (data);
    cachedZoomIndex = -1;
    repaint();
}

void DetailWaveform::setHotCues (const std::array<HotCueInfo, 8>& cues)
{
    hotCues = cues;
    repaint();
}

void DetailWaveform::getVisibleRange (int64_t& startSample, int64_t& endSample) const
{
    if (audioState == nullptr || totalSamples <= 0 || sampleRate <= 0.0)
    {
        startSample = 0;
        endSample = 0;
        return;
    }

    int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
    int64_t halfVisible = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);

    startSample = juce::jmax (static_cast<int64_t> (0), pos - halfVisible);
    endSample   = juce::jmin (totalSamples, pos + halfVisible);
}

void DetailWaveform::timerCallback()
{
    repaint();
}

void DetailWaveform::rebuildImage()
{
    auto w = getWidth();
    auto h = getHeight();

    if (w <= 0 || h <= 0 || waveformData == nullptr || audioState == nullptr)
        return;

    int64_t centerSample = audioState->playheadPosition.load (std::memory_order_relaxed);

    // Only rebuild if something changed
    if (cachedWidth == w && cachedHeight == h
        && cachedZoomIndex == zoomLevelIndex
        && std::abs (centerSample - cachedCenterSample) < 128)
        return;

    cachedImage = juce::Image (juce::Image::ARGB, w, h, true);
    cachedWidth  = w;
    cachedHeight = h;
    cachedZoomIndex = zoomLevelIndex;
    cachedCenterSample = centerSample;

    juce::Graphics ig (cachedImage);

    int64_t halfVisibleSamples = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);
    int64_t viewStart = centerSample - halfVisibleSamples;
    int64_t viewEnd   = centerSample + halfVisibleSamples;
    int64_t viewSpan  = viewEnd - viewStart;

    if (viewSpan <= 0)
        return;

    double samplesPerPixel = static_cast<double> (viewSpan) / static_cast<double> (w);
    int level = waveformData->getBestLevel (samplesPerPixel);

    if (level < 0 || level >= static_cast<int> (waveformData->levels.size()))
        return;

    const auto& points = waveformData->levels[static_cast<size_t> (level)];
    if (points.empty())
        return;

    auto numPoints = static_cast<int> (points.size());
    double levelSpp = static_cast<double> (WaveformData::baseSamplesPerPoint) * std::pow (2.0, level);
    float halfH = static_cast<float> (h) * 0.5f;

    juce::Colour lowColor  (0xFFFF4444);
    juce::Colour midColor  (0xFF44FF44);
    juce::Colour highColor (0xFF44CCFF);

    for (int x = 0; x < w; ++x)
    {
        // Map pixel to sample position
        double samplePos = static_cast<double> (viewStart)
                           + static_cast<double> (x) / static_cast<double> (w) * static_cast<double> (viewSpan);

        // Check bounds
        if (samplePos < 0.0 || samplePos >= static_cast<double> (totalSamples))
            continue;

        // Map to point index
        int pointIdx = static_cast<int> (samplePos / levelSpp);
        pointIdx = juce::jlimit (0, numPoints - 1, pointIdx);

        const auto& pt = points[static_cast<size_t> (pointIdx)];

        float peak = juce::jmax (pt.peakL, pt.peakR);
        float rms  = juce::jmax (pt.rmsL, pt.rmsR);

        // 3-band coloring: draw overlapping bars
        float totalEnergy = pt.energyLow + pt.energyMid + pt.energyHigh;

        if (totalEnergy < 0.0001f)
            continue;

        // Low band
        {
            float energy = pt.energyLow / totalEnergy;
            float height = peak * halfH * energy;
            float rmsH   = rms * halfH * energy;

            ig.setColour (lowColor.withAlpha (0.5f));
            ig.drawVerticalLine (x, halfH - height, halfH + height);

            ig.setColour (lowColor.withAlpha (0.85f));
            ig.drawVerticalLine (x, halfH - rmsH, halfH + rmsH);
        }

        // Mid band
        {
            float energy = pt.energyMid / totalEnergy;
            float height = peak * halfH * energy;
            float rmsH   = rms * halfH * energy;

            ig.setColour (midColor.withAlpha (0.5f));
            ig.drawVerticalLine (x, halfH - height, halfH + height);

            ig.setColour (midColor.withAlpha (0.85f));
            ig.drawVerticalLine (x, halfH - rmsH, halfH + rmsH);
        }

        // High band
        {
            float energy = pt.energyHigh / totalEnergy;
            float height = peak * halfH * energy;
            float rmsH   = rms * halfH * energy;

            ig.setColour (highColor.withAlpha (0.5f));
            ig.drawVerticalLine (x, halfH - height, halfH + height);

            ig.setColour (highColor.withAlpha (0.85f));
            ig.drawVerticalLine (x, halfH - rmsH, halfH + rmsH);
        }
    }
}

void DetailWaveform::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.setColour (juce::Colour (0xFF0A0A0A));
    g.fillRect (bounds);

    if (waveformData == nullptr || audioState == nullptr || totalSamples <= 0)
        return;

    rebuildImage();

    if (cachedImage.isValid())
        g.drawImageAt (cachedImage, 0, 0);

    // Beat grid overlay
    if (beatGridData != nullptr && beatGridData->bpm > 0.0 && audioState != nullptr)
    {
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        int64_t halfVisible = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);
        int64_t viewStart = pos - halfVisible;
        int64_t viewEnd   = pos + halfVisible;
        int64_t viewSpan  = viewEnd - viewStart;

        if (viewSpan > 0)
        {
            juce::Array<int64_t> beats;
            juce::Array<bool> isDownbeat;
            beatGridData->getBeatsInRange (viewStart, viewEnd, beats, isDownbeat);

            float w = static_cast<float> (getWidth());
            float h = static_cast<float> (getHeight());

            for (int i = 0; i < beats.size(); ++i)
            {
                float pixelX = static_cast<float> (beats[i] - viewStart)
                             / static_cast<float> (viewSpan) * w;

                if (isDownbeat[i])
                    g.setColour (juce::Colour (0xAAf9f9f9));
                else
                    g.setColour (juce::Colour (0x44f9f9f9));

                g.drawVerticalLine (static_cast<int> (pixelX), 0.0f, h);
            }
        }
    }

    // Fixed playhead marker at horizontal center
    int centerX = getWidth() / 2;
    g.setColour (juce::Colours::white);
    g.drawVerticalLine (centerX, 0.0f, static_cast<float> (getHeight()));

    // Hot cue markers (PRD-0012)
    if (audioState != nullptr && totalSamples > 0)
    {
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        int64_t halfVisible = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);
        int64_t viewStart = pos - halfVisible;
        int64_t viewEnd   = pos + halfVisible;
        int64_t viewSpan  = viewEnd - viewStart;

        if (viewSpan > 0)
        {
            float w = static_cast<float> (getWidth());
            float h = static_cast<float> (getHeight());

            // Loop overlay (PRD-0014)
            {
                int64_t lpIn  = audioState->loopInSamples.load (std::memory_order_relaxed);
                int64_t lpOut = audioState->loopOutSamples.load (std::memory_order_relaxed);
                bool lpActive = audioState->loopActive.load (std::memory_order_relaxed);

                if (lpIn >= 0 && lpOut > lpIn)
                {
                    float xIn  = static_cast<float> (lpIn - viewStart)
                               / static_cast<float> (viewSpan) * w;
                    float xOut = static_cast<float> (lpOut - viewStart)
                               / static_cast<float> (viewSpan) * w;

                    float visXIn  = juce::jlimit (-2.0f, w + 2.0f, xIn);
                    float visXOut = juce::jlimit (-2.0f, w + 2.0f, xOut);

                    if (visXOut > visXIn)
                    {
                        float alpha = lpActive ? 0.25f : 0.10f;
                        g.setColour (deckAccentColour.withAlpha (alpha));
                        g.fillRect (visXIn, 0.0f, visXOut - visXIn, h);

                        float markerAlpha = lpActive ? 1.0f : 0.5f;
                        g.setColour (deckAccentColour.withAlpha (markerAlpha));

                        // Loop-in marker: 2px line + right-pointing triangle
                        if (xIn >= -2.0f && xIn <= w + 2.0f)
                        {
                            g.fillRect (xIn - 1.0f, 0.0f, 2.0f, h);
                            juce::Path tri;
                            tri.addTriangle (xIn, 0.0f, xIn, 8.0f, xIn + 8.0f, 4.0f);
                            g.fillPath (tri);
                        }

                        // Loop-out marker: 2px line + left-pointing triangle
                        if (xOut >= -2.0f && xOut <= w + 2.0f)
                        {
                            g.fillRect (xOut - 1.0f, 0.0f, 2.0f, h);
                            juce::Path tri;
                            tri.addTriangle (xOut, 0.0f, xOut, 8.0f, xOut - 8.0f, 4.0f);
                            g.fillPath (tri);
                        }
                    }
                }
            }

            for (const auto& cue : hotCues)
            {
                if (! cue.active || cue.positionSamples < 0)
                    continue;

                float pixelX = static_cast<float> (cue.positionSamples - viewStart)
                             / static_cast<float> (viewSpan) * w;

                // Skip if off-screen
                if (pixelX < -8.0f || pixelX > w + 8.0f)
                    continue;

                auto cueColour = HotCueColors::getColour (cue.colorIndex);

                // Vertical line at 50% opacity
                g.setColour (cueColour.withAlpha (0.5f));
                g.drawVerticalLine (static_cast<int> (pixelX), 8.0f, h);

                // Filled triangle (8px wide, 8px tall) at top edge
                juce::Path triangle;
                triangle.addTriangle (pixelX - 4.0f, 0.0f,
                                      pixelX + 4.0f, 0.0f,
                                      pixelX,        8.0f);
                g.setColour (cueColour);
                g.fillPath (triangle);

                // Show label when playhead is within 2 seconds of marker
                if (cue.label.isNotEmpty())
                {
                    double distSeconds = std::abs (static_cast<double> (pos - cue.positionSamples)) / sampleRate;
                    if (distSeconds < 2.0)
                    {
                        g.setColour (cueColour);
                        g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
                        auto textWidth = g.getCurrentFont().getStringWidth (cue.label);
                        float textX = pixelX - static_cast<float> (textWidth) * 0.5f;
                        textX = juce::jlimit (0.0f, w - static_cast<float> (textWidth), textX);
                        g.drawText (cue.label,
                                    static_cast<int> (textX), 9,
                                    textWidth + 4, 12,
                                    juce::Justification::centred);
                    }
                }
            }
        }
    }
}

void DetailWaveform::mouseWheelMove (const juce::MouseEvent&,
                                      const juce::MouseWheelDetails& wheel)
{
    if (waveformData == nullptr)
        return;

    if (wheel.deltaY > 0.0f)
        zoomLevelIndex = juce::jmax (0, zoomLevelIndex - 1);
    else if (wheel.deltaY < 0.0f)
        zoomLevelIndex = juce::jmin (numZoomLevels - 1, zoomLevelIndex + 1);

    visibleSeconds = zoomLevels[zoomLevelIndex];

    // Force image rebuild
    cachedZoomIndex = -1;
    repaint();
}
