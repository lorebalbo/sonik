#include "BeatJumpComponent.h"

BeatJumpComponent::BeatJumpComponent (juce::ValueTree tree)
    : deckTree (tree)
{
    currentSize = static_cast<double> (deckTree.getProperty (IDs::beatJumpSize, 4.0));
    setTooltip ("Beat Jump");
    deckTree.addListener (this);
}

BeatJumpComponent::~BeatJumpComponent()
{
    deckTree.removeListener (this);
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

juce::Rectangle<int> BeatJumpComponent::getBackwardBounds() const
{
    auto b = getLocalBounds();
    int thirdW = b.getWidth() / 3;
    return b.removeFromLeft (thirdW);
}

juce::Rectangle<int> BeatJumpComponent::getSizeBounds() const
{
    auto b = getLocalBounds();
    int thirdW = b.getWidth() / 3;
    return b.withTrimmedLeft (thirdW).withTrimmedRight (thirdW);
}

juce::Rectangle<int> BeatJumpComponent::getForwardBounds() const
{
    auto b = getLocalBounds();
    int thirdW = b.getWidth() / 3;
    return b.removeFromRight (thirdW);
}

BeatJumpComponent::Region BeatJumpComponent::getRegionAt (int x, int y) const
{
    auto pt = juce::Point<int> (x, y);
    if (getBackwardBounds().contains (pt))  return Region::Backward;
    if (getSizeBounds().contains (pt))      return Region::Size;
    if (getForwardBounds().contains (pt))   return Region::Forward;
    return Region::None;
}

juce::String BeatJumpComponent::formatSize (double beats) const
{
    if (beats == 0.5)  return "1/2";
    if (beats == 1.0)  return "1";
    if (beats == 2.0)  return "2";
    if (beats == 4.0)  return "4";
    if (beats == 8.0)  return "8";
    if (beats == 16.0) return "16";
    if (beats == 32.0) return "32";
    return juce::String (beats);
}

bool BeatJumpComponent::isDeckEmpty() const
{
    return deckTree.getProperty (IDs::playbackStatus).toString() == "empty";
}

bool BeatJumpComponent::hasBeatgrid() const
{
    auto bg = deckTree.getChildWithName (IDs::BeatGrid);
    if (bg.isValid())
        return static_cast<double> (bg.getProperty (IDs::bpm, 0.0)) > 0.0;
    return false;
}

void BeatJumpComponent::triggerFlash (Region r)
{
    flashRegion = r;
    flashStartTime = juce::Time::currentTimeMillis();

    // Schedule repaint after flash duration to clear
    auto safeThis = juce::Component::SafePointer (this);
    juce::Timer::callAfterDelay (flashDurationMs + 10, [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->repaint();
    });
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void BeatJumpComponent::paint (juce::Graphics& g)
{
    bool empty     = isDeckEmpty();
    bool hasBg     = hasBeatgrid();
    float alpha    = empty ? 0.3f : 1.0f;
    float sizeAlpha = (! empty && ! hasBg) ? 0.5f : alpha;

    // Check if flash is still active
    bool flashActive = false;
    if (flashRegion != Region::None)
    {
        int64_t elapsed = juce::Time::currentTimeMillis() - flashStartTime;
        flashActive = elapsed < flashDurationMs;
        if (! flashActive)
            flashRegion = Region::None;
    }

    auto drawButton = [&] (juce::Rectangle<int> bounds, const juce::String& label,
                           Region region, const juce::String& tooltip)
    {
        bool isHovered = (hoveredRegion == region && ! empty);
        bool isFlashing = (flashActive && flashRegion == region);

        if (isFlashing)
        {
            // Accent flash: inverted colours
            g.setColour (juce::Colour (0xFF000000).withAlpha (alpha));
            g.fillRect (bounds);
            g.setColour (juce::Colour (0xFFF9F9F9).withAlpha (alpha));
        }
        else if (isHovered)
        {
            g.setColour (juce::Colour (0xFFE2E2E2).withAlpha (alpha));
            g.fillRect (bounds);
            g.setColour (juce::Colour (0xFF000000).withAlpha (alpha));
        }
        else
        {
            g.setColour (juce::Colour (0xFFF3F3F4).withAlpha (alpha));
            g.fillRect (bounds);
            g.setColour (juce::Colour (0xFF000000).withAlpha (alpha));
        }

        // Border
        g.setColour (juce::Colour (0xFF000000).withAlpha (alpha * 0.5f));
        g.drawRect (bounds, 1);

        // Label
        float textAlpha = (region == Region::Size) ? sizeAlpha : alpha;
        g.setColour (isFlashing
            ? juce::Colour (0xFFF9F9F9).withAlpha (textAlpha)
            : juce::Colour (0xFF000000).withAlpha (textAlpha));
        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.drawText (label, bounds, juce::Justification::centred);
    };

    drawButton (getBackwardBounds(), juce::String::charToString (0x25C0), Region::Backward, "Beat Jump Backward");
    drawButton (getSizeBounds(), formatSize (currentSize), Region::Size, "Beat Jump Size (click to change)");
    drawButton (getForwardBounds(), juce::String::charToString (0x25B6), Region::Forward, "Beat Jump Forward");
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void BeatJumpComponent::mouseDown (const juce::MouseEvent& e)
{
    if (isDeckEmpty())
        return;

    auto region = getRegionAt (e.x, e.y);

    switch (region)
    {
        case Region::Backward:
            triggerFlash (Region::Backward);
            if (onJumpBackward)
                onJumpBackward();
            repaint();
            break;

        case Region::Forward:
            triggerFlash (Region::Forward);
            if (onJumpForward)
                onJumpForward();
            repaint();
            break;

        case Region::Size:
            if (onCycleSize)
                onCycleSize (! e.mods.isShiftDown()); // Shift+click = cycle backward
            break;

        case Region::None:
            break;
    }
}

void BeatJumpComponent::mouseMove (const juce::MouseEvent& e)
{
    auto newRegion = getRegionAt (e.x, e.y);
    if (newRegion != hoveredRegion)
    {
        hoveredRegion = newRegion;

        // Update tooltip based on region
        switch (hoveredRegion)
        {
            case Region::Backward: setTooltip ("Beat Jump Backward"); break;
            case Region::Forward:  setTooltip ("Beat Jump Forward"); break;
            case Region::Size:     setTooltip ("Beat Jump Size (click to change)"); break;
            case Region::None:     setTooltip ("Beat Jump"); break;
        }

        repaint();
    }
}

void BeatJumpComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRegion != Region::None)
    {
        hoveredRegion = Region::None;
        repaint();
    }
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void BeatJumpComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                    const juce::Identifier& property)
{
    if (changedTree == deckTree)
    {
        if (property == IDs::beatJumpSize)
        {
            double newSize = static_cast<double> (changedTree[property]);
            if (newSize != currentSize)
            {
                currentSize = newSize;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
            }
        }
        else if (property == IDs::playbackStatus)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }

    // BeatGrid bpm changed
    if (changedTree.hasType (IDs::BeatGrid) && property == IDs::bpm)
    {
        if (changedTree.getParent() == deckTree)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }
}
