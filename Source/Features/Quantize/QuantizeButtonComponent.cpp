#include "QuantizeButtonComponent.h"
#include "Features/Shared/Ui/SonikDraw.h"

void QuantizeButtonComponent::paint (juce::Graphics& g)
{
    const bool isEmpty = (deckStatus == "empty");

    // Half-strength when quantize is on but no beatgrid exists yet (the
    // setting is latched, the snapping has nothing to snap to).
    const float partial = (! isEmpty && enabled && ! hasBeatgrid) ? 0.5f : 1.0f;

    sonik::ui::draw::paintLatchButton (g, getLocalBounds(), "QUANT",
                                       { .active  = enabled,
                                         .hover   = isMouseOver(),
                                         .pressed = false,
                                         .enabled = ! isEmpty,
                                         .alpha   = partial });
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
                {
                    safeThis->updateCursor();
                    safeThis->repaint();
                }
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
