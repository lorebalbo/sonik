#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"

/// Small toggle button for key-lock (master tempo).
/// Active: white text on black background. Inactive: black text on #E2E2E2.
class KeyLockButton final : public juce::Component,
                             private juce::ValueTree::Listener
{
public:
    explicit KeyLockButton (juce::ValueTree deckTree)
        : tree (deckTree)
    {
        enabled = static_cast<bool> (tree.getProperty (IDs::keyLockEnabled, false));
        tree.addListener (this);
    }

    ~KeyLockButton() override
    {
        tree.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        // Background
        if (enabled)
            g.setColour (juce::Colour (0xFF000000));
        else
            g.setColour (juce::Colour (0xFFE2E2E2));

        g.fillRect (bounds);

        // Border
        g.setColour (juce::Colour (0xFF000000));
        g.drawRect (bounds, 1);

        // Label
        if (enabled)
            g.setColour (juce::Colour (0xFFF9F9F9));
        else
            g.setColour (juce::Colour (0xFF000000));

        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.drawText ("KEY", bounds, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        enabled = ! enabled;
        tree.setProperty (IDs::keyLockEnabled, enabled, nullptr);
        repaint();
    }

private:
    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override
    {
        if (changedTree == tree && property == IDs::keyLockEnabled)
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
    }

    juce::ValueTree tree;
    bool enabled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyLockButton)
};
