#include "QuantizeButtonComponent.h"

void QuantizeButtonComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    bool isEmpty = (deckStatus == "empty");

    // Determine overall opacity
    float alpha = 1.0f;
    if (isEmpty)
        alpha = 0.3f;
    else if (enabled && ! hasBeatgrid)
        alpha = 0.5f;

    if (enabled && ! isEmpty)
    {
        // ON state: filled black background
        g.setColour (juce::Colour (0xFF000000).withAlpha (alpha));
        g.fillRect (bounds);

        // Border
        g.setColour (juce::Colour (0xFF000000));
        g.drawRect (bounds, 1);

        // "Q" label in light colour
        g.setColour (juce::Colour (0xFFF9F9F9).withAlpha (alpha));
    }
    else
    {
        // OFF state: surface background with outline
        g.setColour (juce::Colour (0xFFE2E2E2).withAlpha (alpha));
        g.fillRect (bounds);

        // Border
        g.setColour (juce::Colour (0xFF000000).withAlpha (alpha));
        g.drawRect (bounds, 1);

        // "Q" label in dark colour, low opacity when off
        float textAlpha = isEmpty ? 0.3f : 0.5f;
        g.setColour (juce::Colour (0xFF000000).withAlpha (textAlpha));
    }

    g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
    g.drawText ("Q", bounds, juce::Justification::centred);
}

void QuantizeButtonComponent::mouseDown (const juce::MouseEvent&)
{
    // Non-interactive when deck is empty
    if (deckStatus == "empty")
        return;

    enabled = ! enabled;
    tree.setProperty (IDs::quantizeEnabled, enabled, nullptr);
    repaint();
}

void QuantizeButtonComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                         const juce::Identifier& property)
{
    if (changedTree == tree)
    {
        if (property == IDs::quantizeEnabled)
        {
            bool newVal = static_cast<bool> (changedTree[property]);
            if (newVal != enabled)
            {
                enabled = newVal;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
            }
        }
        else if (property == IDs::playbackStatus)
        {
            deckStatus = changedTree[property].toString();
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }

    // BeatGrid bpm changed (child node of our deck tree)
    if (changedTree.hasType (IDs::BeatGrid) && property == IDs::bpm)
    {
        if (changedTree.getParent() == tree)
        {
            bool newHasBeatgrid = static_cast<double> (changedTree[property]) > 0.0;
            if (newHasBeatgrid != hasBeatgrid)
            {
                hasBeatgrid = newHasBeatgrid;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
            }
        }
    }
}
