#include "DetailWaveform.h"
#include <cmath>

DetailWaveform::DetailWaveform()
{
    setOpaque (true);
    setRepaintsOnMouseActivity (true);
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

void DetailWaveform::setHotCues (const std::array<HotCueInfo, 9>& cues)
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

        // 1-bit density approach: bass = solid black, treble = lighter gray (sparse)
        float totalEnergy = pt.energyLow + pt.energyMid + pt.energyHigh;

        if (totalEnergy < 0.0001f)
            continue;

        float wMid  = pt.energyMid  / totalEnergy;
        float wHigh = pt.energyHigh / totalEnergy;

        // Bass-heavy columns render as pure black; treble-heavy as lighter gray
        auto grey = static_cast<uint8_t> (juce::jlimit (0.0f, 180.0f, wMid * 80.0f + wHigh * 160.0f));
        juce::Colour barColour (grey, grey, grey);

        // Peak envelope (translucent outer shell)
        ig.setColour (barColour.withAlpha (0.35f));
        ig.drawVerticalLine (x, halfH - peak * halfH, halfH + peak * halfH);

        // RMS core (opaque inner body)
        ig.setColour (barColour);
        ig.drawVerticalLine (x, halfH - rms * halfH, halfH + rms * halfH);
    }

    // Center divider — subtle axis reference line
    ig.setColour (juce::Colour (0xFFD8D8D8));
    ig.drawHorizontalLine (static_cast<int> (halfH), 0.0f, static_cast<float> (w));
}

void DetailWaveform::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background — surface-container-lowest per DESIGN.md (high-priority visual canvas)
    g.setColour (juce::Colour (0xFFFFFFFF));
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
                    g.setColour (juce::Colour (0x80000000));
                else
                    g.setColour (juce::Colour (0x30000000));

                g.drawVerticalLine (static_cast<int> (pixelX), 0.0f, h);
            }
        }
    }

    // Fixed playhead marker at horizontal center
    int centerX = getWidth() / 2;
    g.setColour (juce::Colours::black);
    g.drawVerticalLine (centerX, 0.0f, static_cast<float> (getHeight()));

    // PRD-0017: Slip ghost marker (shadow playhead)
    if (audioState != nullptr && totalSamples > 0
        && audioState->slipEnabled.load (std::memory_order_relaxed)
        && audioState->slipDisplaced.load (std::memory_order_relaxed))
    {
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        int64_t halfVisible = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);
        int64_t vStart = pos - halfVisible;
        int64_t vEnd   = pos + halfVisible;
        int64_t vSpan  = vEnd - vStart;

        if (vSpan > 0)
        {
            double shadowPos = audioState->slipShadowPosition.load (std::memory_order_relaxed);
            float ghostX = static_cast<float> (shadowPos - static_cast<double> (vStart))
                           / static_cast<float> (vSpan)
                           * static_cast<float> (getWidth());

            if (ghostX >= -2.0f && ghostX <= static_cast<float> (getWidth()) + 2.0f)
            {
                float h = static_cast<float> (getHeight());

                g.setColour (juce::Colour (0x66000000));
                g.drawVerticalLine (static_cast<int> (ghostX), 0.0f, h);

                // Small triangle at top
                juce::Path tri;
                tri.addTriangle (ghostX - 4.0f, 0.0f,
                                 ghostX + 4.0f, 0.0f,
                                 ghostX,        6.0f);
                g.fillPath (tri);
            }
        }
    }

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

    // Tooltip (PRD-0016)
    paintTooltip (g);
}

void DetailWaveform::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown())
        return;

    if (waveformData == nullptr || totalSamples <= 0)
        return;

    if (e.mods.isShiftDown())
    {
        // Shift+click/drag: precise seek (existing behavior).
        isDragging    = true;
        isScratchDrag = false;
        handleSeekAt (e);
        return;
    }

    // Unmodified press: vinyl-style touch/hold + relative-drag scratch
    // (PRD-0016). Press does NOT seek — it captures the current playhead as
    // the drag anchor so the track stays where it is until the user actually
    // drags. The host controls transport state on begin/end to emulate
    // platter touch semantics.
    isDragging        = true;
    isScratchDrag     = true;
    scratchActive     = true;
    dragAnchorX       = e.x;
    dragAnchorSample  = (audioState != nullptr)
                            ? audioState->playheadPosition.load (std::memory_order_relaxed)
                            : int64_t (0);
    lastDispatchedSample = dragAnchorSample;

    if (onScratchBegin)
        onScratchBegin();
}

void DetailWaveform::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging || waveformData == nullptr || totalSamples <= 0)
        return;

    if (isScratchDrag)
    {
        // Unmodified press/drag scratch: compute the target sample from the
        // anchor captured at mouseDown so total displacement equals total
        // mouse motion (1px ~ samplesPerPixel) independent of drag speed.
        // Dragging right pulls earlier samples under the playhead (vinyl
        // convention: grab the waveform and drag it the same way you would
        // drag a record). Each unique target dispatches a seek; the existing
        // 64-sample seek crossfade produces the audible scratch artefact.
        if (getWidth() <= 0 || totalSamples <= 0 || sampleRate <= 0.0)
            return;

        const double samplesPerPixel =
            (static_cast<double> (visibleSeconds) * sampleRate)
            / static_cast<double> (getWidth());

        const int dx = e.x - dragAnchorX;
        int64_t target = dragAnchorSample
                         - static_cast<int64_t> (static_cast<double> (dx) * samplesPerPixel);
        target = juce::jlimit (int64_t (0), totalSamples - 1, target);

        if (target != lastDispatchedSample)
        {
            lastDispatchedSample = target;
            if (onSeek)
                onSeek (target);
        }
        return;
    }

    // Shift+drag seek: stop scrubbing if Shift is released during drag.
    if (! e.mods.isShiftDown())
    {
        isDragging = false;
        return;
    }

    handleSeekAt (e);
}

void DetailWaveform::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;

    if (scratchActive)
    {
        scratchActive = false;
        isScratchDrag = false;
        if (onScratchEnd)
            onScratchEnd();
    }
}

void DetailWaveform::mouseMove (const juce::MouseEvent& e)
{
    updateCursor (e);

    if (waveformData == nullptr || totalSamples <= 0 || ! e.mods.isShiftDown())
    {
        showTooltip = false;
        return;
    }

    showTooltip   = true;
    tooltipPixelX = static_cast<float> (e.x);
    tooltipSample = pixelXToSamplePosition (tooltipPixelX);

    // Apply quantize snap for tooltip when Shift+Alt/Option held
    if (e.mods.isAltDown() && audioState != nullptr)
    {
        int64_t anchor   = audioState->beatgridAnchor.load (std::memory_order_relaxed);
        double  interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
        bool    quantize = audioState->quantizeEnabled.load (std::memory_order_relaxed);

        if (quantize && interval > 0.0)
            tooltipSample = QuantizeService::snapToNearestBeat (tooltipSample, anchor, interval);
    }
}

void DetailWaveform::mouseEnter (const juce::MouseEvent& e)
{
    updateCursor (e);
}

void DetailWaveform::mouseExit (const juce::MouseEvent&)
{
    showTooltip = false;
    // Note: we deliberately do NOT cancel an in-flight scratch drag here.
    // mouseExit fires when the cursor leaves the component bounds vertically
    // during a horizontal drag; JUCE continues to deliver mouseDrag events to
    // the captured component, and we want the scratch gesture to survive.
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void DetailWaveform::mouseWheelMove (const juce::MouseEvent&,
                                      const juce::MouseWheelDetails& wheel)
{
    if (waveformData == nullptr)
        return;

    // Scroll-wheel sensitivity (PRD-0006 follow-up): accumulate raw deltas so
    // trackpad inertial scrolls don’t blow through all 6 zoom levels in one
    // gesture. Step zoom only when the accumulator crosses the threshold.
    wheelAccum += wheel.deltaY;

    bool changed = false;
    while (wheelAccum >= wheelStepThreshold)
    {
        zoomLevelIndex = juce::jmax (0, zoomLevelIndex - 1);
        wheelAccum -= wheelStepThreshold;
        changed = true;
    }
    while (wheelAccum <= -wheelStepThreshold)
    {
        zoomLevelIndex = juce::jmin (numZoomLevels - 1, zoomLevelIndex + 1);
        wheelAccum += wheelStepThreshold;
        changed = true;
    }

    if (! changed)
        return;

    visibleSeconds = zoomLevels[zoomLevelIndex];
    cachedZoomIndex = -1;
    repaint();
}

void DetailWaveform::zoomIn()
{
    if (zoomLevelIndex <= 0)
        return;
    zoomLevelIndex = juce::jmax (0, zoomLevelIndex - 1);
    visibleSeconds = zoomLevels[zoomLevelIndex];
    wheelAccum = 0.0f;
    cachedZoomIndex = -1;
    repaint();
}

void DetailWaveform::zoomOut()
{
    if (zoomLevelIndex >= numZoomLevels - 1)
        return;
    zoomLevelIndex = juce::jmin (numZoomLevels - 1, zoomLevelIndex + 1);
    visibleSeconds = zoomLevels[zoomLevelIndex];
    wheelAccum = 0.0f;
    cachedZoomIndex = -1;
    repaint();
}

// ---------------------------------------------------------------------------
// Coordinate mapping (PRD-0016)
// ---------------------------------------------------------------------------

int64_t DetailWaveform::pixelXToSamplePosition (float pixelX) const
{
    if (totalSamples <= 0 || getWidth() <= 0 || audioState == nullptr || sampleRate <= 0.0)
        return 0;

    int64_t centerSample     = audioState->playheadPosition.load (std::memory_order_relaxed);
    int64_t halfVisibleSamps = static_cast<int64_t> (visibleSeconds * 0.5 * sampleRate);
    int64_t viewStart        = centerSample - halfVisibleSamps;
    int64_t viewSpan         = halfVisibleSamps * 2;

    float fraction = pixelX / static_cast<float> (getWidth());
    int64_t samplePos = viewStart + static_cast<int64_t> (fraction * static_cast<double> (viewSpan));

    return juce::jlimit (int64_t (0), totalSamples - 1, samplePos);
}

// ---------------------------------------------------------------------------
// Seek handling (PRD-0016)
// ---------------------------------------------------------------------------

void DetailWaveform::handleSeekAt (const juce::MouseEvent& e)
{
    float clampedX = juce::jlimit (0.0f, static_cast<float> (getWidth()), static_cast<float> (e.x));
    int64_t samplePos = pixelXToSamplePosition (clampedX);

    // Shift+Alt/Option: quantize snap to nearest beat
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

void DetailWaveform::updateCursor (const juce::MouseEvent& e)
{
    if (waveformData != nullptr && totalSamples > 0 && e.mods.isShiftDown())
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void DetailWaveform::paintTooltip (juce::Graphics& g)
{
    if (! showTooltip || totalSamples <= 0)
        return;

    auto timeStr = formatTime (tooltipSample);
    auto font = juce::Font (juce::FontOptions (11.0f));
    g.setFont (font);

    int textWidth = font.getStringWidth (timeStr) + 8;
    int textHeight = 16;

    float tipX = tooltipPixelX - static_cast<float> (textWidth) * 0.5f;
    float tipY = 0.0f; // top of component

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

juce::String DetailWaveform::formatTime (int64_t samplePos) const
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double totalSeconds = static_cast<double> (samplePos) / sr;
    int minutes = static_cast<int> (totalSeconds) / 60;
    double secs = totalSeconds - static_cast<double> (minutes * 60);
    int wholeSecs = static_cast<int> (secs);
    int centis = static_cast<int> ((secs - wholeSecs) * 100.0);

    return juce::String::formatted ("%d:%02d.%02d", minutes, wholeSecs, centis);
}
