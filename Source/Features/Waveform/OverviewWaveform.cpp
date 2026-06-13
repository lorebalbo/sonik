#include "OverviewWaveform.h"
#include <cmath>

OverviewWaveform::OverviewWaveform()
{
    setOpaque (true);
    setRepaintsOnMouseActivity (true);
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

void OverviewWaveform::setHotCues (const std::array<HotCueInfo, 9>& cues)
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

    for (int x = 0; x < w; ++x)
    {
        float frac = static_cast<float> (x) / static_cast<float> (w);
        int pointIdx = juce::jlimit (0, numPoints - 1,
                                     static_cast<int> (frac * static_cast<float> (numPoints)));
        const auto& pt = points[static_cast<size_t> (pointIdx)];

        float peak = juce::jmax (pt.peakL, pt.peakR);
        float rms  = juce::jmax (pt.rmsL, pt.rmsR);

        float totalEnergy = pt.energyLow + pt.energyMid + pt.energyHigh;

        // 1-bit density: bass = solid black, treble = lighter gray (sparse)
        juce::Colour barColour;
        if (totalEnergy > 0.0001f)
        {
            float wMid  = pt.energyMid  / totalEnergy;
            float wHigh = pt.energyHigh / totalEnergy;
            auto grey = static_cast<uint8_t> (juce::jlimit (0.0f, 180.0f, wMid * 80.0f + wHigh * 160.0f));
            barColour = juce::Colour (grey, grey, grey);
        }
        else
        {
            barColour = juce::Colour (0xFFC8C8C8);
        }

        // Peak envelope (translucent outer shell)
        float peakHeight = peak * halfH;
        ig.setColour (barColour.withAlpha (0.35f));
        ig.drawVerticalLine (x, halfH - peakHeight, halfH + peakHeight);

        // RMS core (opaque inner body)
        float rmsHeight = rms * halfH;
        ig.setColour (barColour);
        ig.drawVerticalLine (x, halfH - rmsHeight, halfH + rmsHeight);
    }

    // Center divider
    ig.setColour (juce::Colour (0xFFD8D8D8));
    ig.drawHorizontalLine (static_cast<int> (halfH), 0.0f, static_cast<float> (w));
}

void OverviewWaveform::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background — surface-container-lowest per DESIGN.md (high-priority visual canvas)
    g.setColour (juce::Colour (0xFFFFFFFF));
    g.fillRect (bounds);

    if (waveformData == nullptr)
        return;

    // Rebuild image if size changed
    if (cachedWidth != getWidth() || cachedHeight != getHeight())
        rebuildImage();

    if (cachedImage.isValid())
        g.drawImageAt (cachedImage, 0, 0);

    // Viewport rectangle — subtle dark tint + 1px border
    if (totalSamples > 0 && visibleEnd > visibleStart)
    {
        float xStart = static_cast<float> (visibleStart) / static_cast<float> (totalSamples)
                        * static_cast<float> (getWidth());
        float xEnd   = static_cast<float> (visibleEnd) / static_cast<float> (totalSamples)
                        * static_cast<float> (getWidth());

        g.setColour (juce::Colour (0x20000000));
        g.fillRect (xStart, 0.0f, xEnd - xStart, static_cast<float> (getHeight()));

        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (xStart, 0.0f, xEnd - xStart, static_cast<float> (getHeight()), 1.0f);
    }

    // Playhead marker
    if (audioState != nullptr && totalSamples > 0)
    {
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        float xPos = static_cast<float> (pos) / static_cast<float> (totalSamples)
                     * static_cast<float> (getWidth());

        g.setColour (juce::Colours::black);
        g.drawVerticalLine (static_cast<int> (xPos), 0.0f, static_cast<float> (getHeight()));

        // PRD-0017: Slip ghost marker (shadow playhead)
        if (audioState->slipEnabled.load (std::memory_order_relaxed)
            && audioState->slipDisplaced.load (std::memory_order_relaxed))
        {
            double shadowPos = audioState->slipShadowPosition.load (std::memory_order_relaxed);
            float ghostX = static_cast<float> (shadowPos) / static_cast<float> (totalSamples)
                           * static_cast<float> (getWidth());

            g.setColour (juce::Colour (0x66000000));
            g.drawVerticalLine (static_cast<int> (ghostX), 0.0f,
                                static_cast<float> (getHeight()));
        }
    }

    // Hot cue markers (PRD-0012)
    if (totalSamples > 0)
    {
        float w = static_cast<float> (getWidth());

        // Loop overlay (PRD-0014)
        if (audioState != nullptr)
        {
            int64_t lpIn  = audioState->loopInSamples.load (std::memory_order_relaxed);
            int64_t lpOut = audioState->loopOutSamples.load (std::memory_order_relaxed);
            bool lpActive = audioState->loopActive.load (std::memory_order_relaxed);

            if (lpIn >= 0 && lpOut > lpIn)
            {
                float h = static_cast<float> (getHeight());
                float xIn  = static_cast<float> (lpIn)  / static_cast<float> (totalSamples) * w;
                float xOut = static_cast<float> (lpOut) / static_cast<float> (totalSamples) * w;

                float alpha = lpActive ? 0.20f : 0.10f;
                g.setColour (deckAccentColour.withAlpha (alpha));
                g.fillRect (xIn, 0.0f, xOut - xIn, h);

                g.setColour (deckAccentColour.withAlpha (lpActive ? 0.8f : 0.4f));
                g.drawVerticalLine (static_cast<int> (xIn),  0.0f, h);
                g.drawVerticalLine (static_cast<int> (xOut), 0.0f, h);
            }
        }

        for (const auto& cue : hotCues)
        {
            if (! cue.active || cue.positionSamples < 0)
                continue;

            float pixelX = static_cast<float> (cue.positionSamples)
                         / static_cast<float> (totalSamples) * w;

            if (pixelX < -4.0f || pixelX > w + 4.0f)
                continue;

            // Strict monochrome (DESIGN.md §2): cue markers are ink, not the
            // stored cue colour — that stays as metadata for controller LEDs.
            // Triangle only (no vertical line) on overview
            juce::Path triangle;
            triangle.addTriangle (pixelX - 4.0f, 0.0f,
                                  pixelX + 4.0f, 0.0f,
                                  pixelX,        8.0f);
            g.setColour (juce::Colour (0xFF2D2D2D));
            g.fillPath (triangle);
        }
    }

    // Tooltip (PRD-0016)
    paintTooltip (g);
}

void OverviewWaveform::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown())
        return;

    if (waveformData == nullptr || totalSamples <= 0)
        return;

    isDragging = true;
    handleSeekAt (e);
}

void OverviewWaveform::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging || waveformData == nullptr || totalSamples <= 0)
        return;

    handleSeekAt (e);
}

void OverviewWaveform::mouseMove (const juce::MouseEvent& e)
{
    if (waveformData == nullptr || totalSamples <= 0)
    {
        showTooltip = false;
        return;
    }

    showTooltip   = true;
    tooltipPixelX = static_cast<float> (e.x);
    tooltipSample = pixelXToSamplePosition (tooltipPixelX);

    // Apply quantize snap for tooltip when Alt/Option held
    if (e.mods.isAltDown() && audioState != nullptr)
    {
        int64_t anchor   = audioState->beatgridAnchor.load (std::memory_order_relaxed);
        double  interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
        bool    quantize = audioState->quantizeEnabled.load (std::memory_order_relaxed);

        if (quantize && interval > 0.0)
            tooltipSample = QuantizeService::snapToNearestBeat (tooltipSample, anchor, interval);
    }
}

void OverviewWaveform::mouseEnter (const juce::MouseEvent&)
{
    if (waveformData != nullptr && totalSamples > 0)
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void OverviewWaveform::mouseExit (const juce::MouseEvent&)
{
    showTooltip = false;
    isDragging  = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void OverviewWaveform::mouseWheelMove (const juce::MouseEvent&,
                                        const juce::MouseWheelDetails&)
{
    // Ignore scroll wheel on overview — overview always shows full track
}

void OverviewWaveform::resized()
{
    cachedWidth = 0; // force rebuild
}

int64_t OverviewWaveform::pixelXToSamplePosition (float pixelX) const
{
    if (totalSamples <= 0 || getWidth() <= 0)
        return 0;

    float fraction = juce::jlimit (0.0f, 1.0f,
                                   pixelX / static_cast<float> (getWidth()));
    return juce::jlimit (int64_t (0), totalSamples - 1,
                         static_cast<int64_t> (fraction * static_cast<float> (totalSamples)));
}

void OverviewWaveform::handleSeekAt (const juce::MouseEvent& e)
{
    int64_t samplePos = pixelXToSamplePosition (static_cast<float> (e.x));

    // Alt/Option: quantize snap to nearest beat
    if (e.mods.isAltDown() && audioState != nullptr)
    {
        int64_t anchor   = audioState->beatgridAnchor.load (std::memory_order_relaxed);
        double  interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
        bool    quantize = audioState->quantizeEnabled.load (std::memory_order_relaxed);

        if (quantize && interval > 0.0)
            samplePos = QuantizeService::snapToNearestBeat (samplePos, anchor, interval);
    }

    samplePos = juce::jlimit (int64_t (0), totalSamples - 1, samplePos);

    if (onSeek)
        onSeek (samplePos);
}

void OverviewWaveform::paintTooltip (juce::Graphics& g)
{
    if (! showTooltip || totalSamples <= 0)
        return;

    auto timeStr = formatTime (tooltipSample);
    auto font = juce::Font (juce::FontOptions (11.0f));
    g.setFont (font);

    int textWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, timeStr)) + 8;
    int textHeight = 16;

    float tipX = tooltipPixelX - static_cast<float> (textWidth) * 0.5f;
    float tipY = -16.0f; // 16px above the cursor (above component top is fine)
    tipY = juce::jmax (0.0f, tipY); // clamp to component bounds

    // Clamp horizontally
    tipX = juce::jlimit (0.0f,
                         static_cast<float> (getWidth() - textWidth),
                         tipX);

    g.setColour (juce::Colour (0xEE000000));
    g.fillRect (tipX, tipY, static_cast<float> (textWidth), static_cast<float> (textHeight));

    g.setColour (juce::Colour (0xFFf9f9f9));
    g.drawText (timeStr,
                static_cast<int> (tipX), static_cast<int> (tipY),
                textWidth, textHeight,
                juce::Justification::centred);
}

juce::String OverviewWaveform::formatTime (int64_t samplePos) const
{
    double sr = 44100.0;
    if (waveformData != nullptr && waveformData->sampleRate > 0.0)
        sr = waveformData->sampleRate;

    double totalSeconds = static_cast<double> (samplePos) / sr;
    int minutes = static_cast<int> (totalSeconds) / 60;
    double secs = totalSeconds - static_cast<double> (minutes * 60);
    int wholeSecs = static_cast<int> (secs);
    int centis = static_cast<int> ((secs - wholeSecs) * 100.0);

    return juce::String::formatted ("%d:%02d.%02d", minutes, wholeSecs, centis);
}
