#include "LoopControlComponent.h"

LoopControlComponent::LoopControlComponent (juce::ValueTree deck)
    : deckTree (deck),
      loopNode (deck.getChildWithName (IDs::Loop))
{
    loopIsActive  = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    int64_t lIn   = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut  = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
    loopIsDefined = (lIn >= 0 && lOut > lIn);

    deckTree.addListener (this);
}

LoopControlComponent::~LoopControlComponent()
{
    deckTree.removeListener (this);
}

void LoopControlComponent::setActiveAutoLoopBeats (float beats)
{
    if (activeAutoBeats != beats)
    {
        activeAutoBeats = beats;
        repaint();
    }
}

void LoopControlComponent::setPendingLoopIn (bool pending)
{
    if (pendingIn != pending)
    {
        pendingIn = pending;
        repaint();
    }
}

bool LoopControlComponent::isDeckEmpty() const
{
    return deckTree.getProperty (IDs::playbackStatus).toString() == "empty";
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

juce::Rectangle<int> LoopControlComponent::getButtonBounds (int index) const
{
    if (index < 0 || index >= numButtons)
        return {};

    auto bounds = getLocalBounds();
    int totalGaps  = (numButtons - 1) * buttonGap;
    int totalWidth = bounds.getWidth() - totalGaps;
    int btnWidth   = totalWidth / numButtons;
    int x = bounds.getX() + index * (btnWidth + buttonGap);

    // Last button absorbs remaining space
    if (index == numButtons - 1)
        btnWidth = bounds.getRight() - x;

    return { x, bounds.getY(), btnWidth, bounds.getHeight() };
}

int LoopControlComponent::getButtonAt (int x, int y) const
{
    for (int i = 0; i < numButtons; ++i)
    {
        if (getButtonBounds (i).contains (x, y))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void LoopControlComponent::paint (juce::Graphics& g)
{
    bool empty = isDeckEmpty();

    for (int i = 0; i < numButtons; ++i)
    {
        auto area = getButtonBounds (i);
        const auto& def = buttons[i];

        bool isActive = false;

        if (def.type == BtnType::Toggle)
            isActive = loopIsActive;
        else if (def.type == BtnType::AutoLoop)
            isActive = loopIsActive && (activeAutoBeats == def.autoLoopBeats);
        else if (def.type == BtnType::LoopIn)
            isActive = pendingIn;

        // Background
        if (empty)
        {
            g.setColour (juce::Colour (0xFFE2E2E2));
        }
        else if (isActive)
        {
            g.setColour (juce::Colour (0xFF000000));
        }
        else if (i == hoveredButton)
        {
            g.setColour (juce::Colour (0xFFD0D0D0));
        }
        else
        {
            g.setColour (juce::Colour (0xFFF9F9F9));
        }
        g.fillRect (area);

        // Border
        g.setColour (juce::Colour (0xFF000000).withAlpha (empty ? 0.2f : 0.4f));
        g.drawRect (area, 1);

        // Text
        if (empty)
            g.setColour (juce::Colour (0xFF999999));
        else if (isActive)
            g.setColour (juce::Colour (0xFFF9F9F9));
        else
            g.setColour (juce::Colour (0xFF000000));

        // Inactive but loop defined: show LOOP button at reduced opacity
        if (def.type == BtnType::Toggle && ! loopIsActive && loopIsDefined && ! empty)
            g.setColour (juce::Colour (0xFF000000).withAlpha (0.5f));

        g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
        g.drawText (def.label, area, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void LoopControlComponent::mouseDown (const juce::MouseEvent& e)
{
    if (isDeckEmpty())
        return;

    int idx = getButtonAt (e.x, e.y);
    if (idx < 0)
        return;

    const auto& def = buttons[idx];

    switch (def.type)
    {
        case BtnType::LoopIn:
            if (onLoopIn) onLoopIn();
            break;
        case BtnType::LoopOut:
            if (onLoopOut) onLoopOut();
            break;
        case BtnType::Toggle:
            if (loopIsActive)
            {
                if (onToggleLoop) onToggleLoop();
            }
            else if (loopIsDefined)
            {
                if (onReLoop) onReLoop();
            }
            break;
        case BtnType::Halve:
            if (onLoopHalve) onLoopHalve();
            break;
        case BtnType::Double:
            if (onLoopDouble) onLoopDouble();
            break;
        case BtnType::AutoLoop:
            if (onAutoLoop) onAutoLoop (def.autoLoopBeats);
            break;
    }
}

void LoopControlComponent::mouseMove (const juce::MouseEvent& e)
{
    int idx = getButtonAt (e.x, e.y);
    if (idx != hoveredButton)
    {
        hoveredButton = idx;
        repaint();
    }
}

void LoopControlComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredButton != -1)
    {
        hoveredButton = -1;
        repaint();
    }
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void LoopControlComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                      const juce::Identifier& property)
{
    if (changedTree.hasType (IDs::Loop))
    {
        if (property == IDs::active || property == IDs::loopIn || property == IDs::loopOut)
        {
            loopIsActive  = static_cast<bool> (loopNode.getProperty (IDs::active, false));
            int64_t lIn   = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
            int64_t lOut  = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
            loopIsDefined = (lIn >= 0 && lOut > lIn);
            repaint();
        }
    }

    if (changedTree == deckTree && property == IDs::playbackStatus)
        repaint();
}
