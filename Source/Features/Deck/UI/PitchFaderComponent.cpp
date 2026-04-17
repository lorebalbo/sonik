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
    int trackWidth = juce::jmax (6, bounds.getWidth() / 3);
    int x = (bounds.getWidth() - trackWidth) / 2;
    return { x, trackMarginTop, trackWidth, bounds.getHeight() - trackMarginTop - trackMarginBot };
}

juce::Rectangle<int> PitchFaderComponent::getHandleArea() const
{
    auto track = getTrackArea();
    float norm = pitchPercentToNormalized (pitchPercent);
    int y = static_cast<int> (normalizedToY (norm)) - handleHeight / 2;

    int handleWidth = track.getWidth() + handleMargin * 2;
    int hx = track.getX() - handleMargin;
    return { hx, y, handleWidth, handleHeight };
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
    auto bounds = getLocalBounds();

    // Pitch percentage display at top
    {
        juce::String displayText;
        if (std::abs (pitchPercent) < 0.005f)
            displayText = "0.00%";
        else if (pitchPercent > 0.0f)
            displayText = "+" + juce::String (pitchPercent, 2) + "%";
        else
            displayText = juce::String (pitchPercent, 2) + "%";

        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.drawText (displayText,
                    bounds.removeFromTop (displayHeight),
                    juce::Justification::centred);
    }

    // Range label top: "-X%" (slower, CDJ convention: top = negative pitch)
    {
        juce::String topLabel = "-" + juce::String (pitchRange) + "%";
        g.setColour (juce::Colour (0xFF888888));
        g.setFont (juce::FontOptions (9.0f));
        auto labelArea = juce::Rectangle<int> (0, trackMarginTop - labelHeight, getWidth(), labelHeight);
        g.drawText (topLabel, labelArea, juce::Justification::centred);
    }

    // Fader track
    auto track = getTrackArea();
    g.setColour (juce::Colour (0xFFE2E2E2)); // surface-container-highest
    g.fillRect (track);

    // Center tick mark (1px line)
    int centerY = track.getY() + track.getHeight() / 2;
    g.setColour (juce::Colour (0xFFAAAAAA));
    g.drawHorizontalLine (centerY, static_cast<float> (track.getX() - 3),
                          static_cast<float> (track.getRight() + 3));

    // Handle
    auto handle = getHandleArea();
    g.setColour (juce::Colours::black);
    g.fillRect (handle);

    // Range label bottom: "+X%" (faster, CDJ convention: bottom = positive pitch)
    {
        juce::String botLabel = "+" + juce::String (pitchRange) + "%";
        g.setColour (juce::Colour (0xFF888888));
        g.setFont (juce::FontOptions (9.0f));
        auto labelArea = juce::Rectangle<int> (0, getHeight() - trackMarginBot, getWidth(), labelHeight);
        g.drawText (botLabel, labelArea, juce::Justification::centred);
    }

    // Pitch range button and reset button at very bottom
    int btnY = getHeight() - trackMarginBot + labelHeight + 2;
    int btnW = juce::jmin (getWidth() - 4, 28);
    int btnX = (getWidth() - btnW) / 2;

    // Range button
    {
        auto rangeArea = juce::Rectangle<int> (btnX, btnY, btnW, 14);
        g.setColour (juce::Colour (0xFFE2E2E2));
        g.fillRect (rangeArea);
        g.setColour (juce::Colours::black);
        g.drawRect (rangeArea, 1);
        g.setFont (juce::FontOptions (8.0f).withStyle ("Bold"));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (pitchRange),
                    rangeArea, juce::Justification::centred);
    }

    // Reset button below range
    {
        auto resetArea = juce::Rectangle<int> (btnX, btnY + 16, btnW, 14);
        g.setColour (juce::Colour (0xFFE2E2E2));
        g.fillRect (resetArea);
        g.setColour (juce::Colours::black);
        g.drawRect (resetArea, 1);
        g.setFont (juce::FontOptions (8.0f).withStyle ("Bold"));
        g.drawText ("0", resetArea, juce::Justification::centred);
    }
}

void PitchFaderComponent::resized()
{
    repaint();
}

// --- Mouse interaction ---

void PitchFaderComponent::mouseDown (const juce::MouseEvent& e)
{
    auto bounds = getLocalBounds();
    int btnW = juce::jmin (getWidth() - 4, 28);
    int btnX = (getWidth() - btnW) / 2;
    int btnY = getHeight() - trackMarginBot + labelHeight + 2;

    // Check range button hit
    auto rangeArea = juce::Rectangle<int> (btnX, btnY, btnW, 14);
    if (rangeArea.contains (e.getPosition()))
    {
        cyclePitchRange();
        return;
    }

    // Check reset button hit
    auto resetArea = juce::Rectangle<int> (btnX, btnY + 16, btnW, 14);
    if (resetArea.contains (e.getPosition()))
    {
        setPitchPercent (0.0f, true);
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
