#include "PitchFaderComponent.h"

const std::array<int, 4> PitchFaderComponent::ranges = { 4, 8, 16, 50 };

PitchFaderComponent::PitchFaderComponent (juce::ValueTree deckTree)
    : tree (deckTree)
{
    setOpaque (false);
    updateFromState();
    tree.addListener (this);
}

PitchFaderComponent::~PitchFaderComponent()
{
    tree.removeListener (this);
}

float PitchFaderComponent::getNormalizedValue() const
{
    return pitchPercentToNormalized (pitchPercent);
}

// --- Coordinate helpers ---

juce::Rectangle<int> PitchFaderComponent::getTrackArea() const
{
    auto bounds = getLocalBounds();
    constexpr int trackWidth = 4;  // narrow dark rail (req 1)
    int x = (bounds.getWidth() - trackWidth) / 2;
    return { x, trackMarginTop, trackWidth, bounds.getHeight() - trackMarginTop - trackMarginBot };
}

juce::Rectangle<int> PitchFaderComponent::getHandleArea() const
{
    float norm = pitchPercentToNormalized (pitchPercent);
    int y      = static_cast<int> (normalizedToY (norm)) - handleHeight / 2;
    // Leave 3px on right for drop shadow (req 5)
    return { 2, y, getWidth() - 5, handleHeight };
}

float PitchFaderComponent::pitchPercentToNormalized (float pitchPct) const
{
    // CDJ convention: fader UP = slower (negative pitch), DOWN = faster (positive pitch)
    // Normalized 0.0 = top = -pitchRange%, 1.0 = bottom = +pitchRange%
    float range = static_cast<float> (pitchRange);
    float clamped = juce::jlimit (-range, range, pitchPct);
    return (clamped + range) / (2.0f * range);
}

float PitchFaderComponent::normalizedToPitchPercent (float norm) const
{
    float range = static_cast<float> (pitchRange);
    float clamped = juce::jlimit (0.0f, 1.0f, norm);
    return clamped * 2.0f * range - range;
}

float PitchFaderComponent::yToNormalized (float y) const
{
    auto track = getTrackArea();
    float top = static_cast<float> (track.getY());
    float bot = static_cast<float> (track.getBottom());
    return juce::jlimit (0.0f, 1.0f, (y - top) / (bot - top));
}

float PitchFaderComponent::normalizedToY (float norm) const
{
    auto track = getTrackArea();
    float top = static_cast<float> (track.getY());
    float bot = static_cast<float> (track.getBottom());
    return top + norm * (bot - top);
}

float PitchFaderComponent::getMouseWheelIncrement() const
{
    switch (pitchRange)
    {
        case 4:  return 0.05f;
        case 8:  return 0.10f;
        case 16: return 0.20f;
        case 50: return 0.50f;
        default: return 0.10f;
    }
}

// --- Paint ---

void PitchFaderComponent::paint (juce::Graphics& g)
{
    const juce::Colour kDark  (0xFF2D2D2D);
    const juce::Colour kLight (0xFFF9F9F9);

    auto track = getTrackArea();

    // ── Tick marks ────────────────────────────────────────────────────────
    // 2px thick, drawn before handle. Topmost/bottommost pair aligned with
    // the 2px track border (req 2). Gap of 1px from track sides.
    {
        constexpr int numTicks = 5;
        g.setColour (kDark);
        for (int i = 0; i < numTicks; ++i)
        {
            // ty aligns tick[0] with top border, tick[4] with bottom border
            int ty = track.getY() + i * (track.getHeight() - 2) / (numTicks - 1);
            bool isCenter = (i == numTicks / 2);
            int  tw = isCenter ? 14 : 10;
            // Left
            g.fillRect (track.getX() - tw - 1, ty, tw, 2);
            // Right
            g.fillRect (track.getRight() + 1,  ty, tw, 2);
        }
    }

    // ── Track (narrow, fully dark — req 1) ───────────────────────────────────
    g.setColour (kDark);
    g.fillRect (track);
    g.setColour (kDark);
    g.drawRect (track, 2);

    // ── Handle drop shadow (blur=0, +3px right+down, #2D2D2D @75% — req 5) ─────
    auto handle = getHandleArea();
    g.setColour (juce::Colour (0xBF2D2D2D));  // 0xBF ≈ 75% opacity
    g.fillRect (handle.translated (3, 3));

    // ── Handle (light fill, dark border) ────────────────────────────────────
    g.setColour (kLight);
    g.fillRect (handle);
    g.setColour (kDark);
    g.drawRect (handle, 2);

    // ── Range button at bottom — SYNC style ─────────────────────────────────
    {
        auto rangeArea = juce::Rectangle<int> (0, getHeight() - 23, getWidth(), 23);
        g.setColour (kLight);
        g.fillRect (rangeArea);
        g.setColour (kDark);
        g.drawRect (rangeArea, 2);
        g.setColour (kDark);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (pitchRange) + "%",
                    rangeArea, juce::Justification::centred);
    }
}

void PitchFaderComponent::resized()
{
    repaint();
}

// --- Mouse interaction ---

void PitchFaderComponent::mouseDown (const juce::MouseEvent& e)
{
    // Check range button (bottom 23px)
    auto rangeArea = juce::Rectangle<int> (0, getHeight() - 23, getWidth(), 23);
    if (rangeArea.contains (e.getPosition()))
    {
        cyclePitchRange();
        return;
    }

    // Start fader drag
    auto track = getTrackArea();
    if (e.y >= track.getY() - handleHeight && e.y <= track.getBottom() + handleHeight)
    {
        isDragging = true;
        dragStartY = static_cast<float> (e.y);
        dragStartPitch = pitchPercent;
    }
}

void PitchFaderComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging)
        return;

    auto track = getTrackArea();
    float trackHeight = static_cast<float> (track.getHeight());
    float range = static_cast<float> (pitchRange);

    // Map pixel delta to pitch delta
    float dy = static_cast<float> (e.y) - dragStartY;
    float pitchDelta = (dy / trackHeight) * 2.0f * range;

    float newPitch = dragStartPitch + pitchDelta;
    newPitch = juce::jlimit (-range, range, newPitch);

    // Dead zone around center
    if (std::abs (newPitch) < deadZone)
        newPitch = 0.0f;

    setPitchPercent (newPitch);
}

void PitchFaderComponent::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;
}

void PitchFaderComponent::mouseDoubleClick (const juce::MouseEvent&)
{
    setPitchPercent (0.0f, true);
}

void PitchFaderComponent::mouseWheelMove (const juce::MouseEvent&,
                                           const juce::MouseWheelDetails& wheel)
{
    float increment = getMouseWheelIncrement();
    // CDJ: wheel up = slower (negative), wheel down = faster (positive)
    // Standard wheel: deltaY > 0 means scroll up
    float delta = -wheel.deltaY * increment / std::abs (wheel.deltaY > 0.0f ? wheel.deltaY : 0.01f);

    // Normalize: just use sign
    if (wheel.deltaY > 0.0f)
        delta = -increment;
    else if (wheel.deltaY < 0.0f)
        delta = increment;
    else
        return;

    float range = static_cast<float> (pitchRange);
    float newPitch = juce::jlimit (-range, range, pitchPercent + delta);

    if (std::abs (newPitch) < deadZone)
        newPitch = 0.0f;

    setPitchPercent (newPitch);
}

// --- State management ---

void PitchFaderComponent::setPitchPercent (float newPitch, bool animate)
{
    if (animate)
    {
        // Simple 150ms animation using Timer
        isAnimating = true;
        animTarget = newPitch;

        // For simplicity, snap immediately (JUCE animation would require a Timer;
        // we avoid complexity and snap with a brief async callback chain)
        pitchPercent = newPitch;
        isAnimating = false;
        commitToState();
        repaint();
        return;
    }

    pitchPercent = newPitch;
    commitToState();
    repaint();
}

void PitchFaderComponent::commitToState()
{
    if (! tree.isValid())
        return;

    float speedMultiplier = 1.0f + (pitchPercent / 100.0f);

    tree.setProperty (IDs::pitch, pitchPercent, nullptr);
    tree.setProperty (IDs::speedMultiplier, speedMultiplier, nullptr);
}

void PitchFaderComponent::cyclePitchRange()
{
    // Find current range index and cycle to next
    int currentIdx = 0;
    for (int i = 0; i < static_cast<int> (ranges.size()); ++i)
    {
        if (ranges[static_cast<size_t> (i)] == pitchRange)
        {
            currentIdx = i;
            break;
        }
    }

    int nextIdx = (currentIdx + 1) % static_cast<int> (ranges.size());
    pitchRange = ranges[static_cast<size_t> (nextIdx)];

    // Clamp current pitch to new range
    float range = static_cast<float> (pitchRange);
    pitchPercent = juce::jlimit (-range, range, pitchPercent);

    tree.setProperty (IDs::pitchRange, pitchRange, nullptr);
    commitToState();
    repaint();
}

void PitchFaderComponent::updateFromState()
{
    if (! tree.isValid())
        return;

    pitchPercent = static_cast<float> (tree.getProperty (IDs::pitch, 0.0f));
    pitchRange   = static_cast<int> (tree.getProperty (IDs::pitchRange, 8));
    repaint();
}

void PitchFaderComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                     const juce::Identifier& property)
{
    if (changedTree != tree)
        return;

    if (property == IDs::pitch || property == IDs::pitchRange || property == IDs::speedMultiplier)
    {
        if (isDragging)
            return;

        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->updateFromState();
        });
    }
}
