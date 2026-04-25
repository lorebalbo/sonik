#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"

/// SYNC toggle button.
/// Active (synced): white text on black background.
/// Inactive: black text on light background.
/// Follows the Figma "Button" component visual style.
class SyncButtonComponent final : public juce::Component,
                                   public juce::SettableTooltipClient,
                                   private juce::ValueTree::Listener
{
public:
    explicit SyncButtonComponent (juce::ValueTree deckTree)
        : tree (deckTree)
    {
        enabled = static_cast<bool> (tree.getProperty (IDs::syncEnabled, false));
        tree.addListener (this);
        setTooltip ("Toggle BPM sync with master deck");
    }

    ~SyncButtonComponent() override
    {
        tree.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        // Background
        g.setColour (enabled ? juce::Colour (0xFF2D2D2D) : juce::Colour (0xFFF9F9F9));
        g.fillRect (bounds);

        // Border — 2px matching Figma Button style
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (bounds, 2);

        // Label
        g.setColour (enabled ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF2D2D2D));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawText ("SYNC", bounds, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        enabled = ! enabled;
        tree.setProperty (IDs::syncEnabled, enabled, nullptr);
        repaint();
    }

private:
    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override
    {
        if (changedTree == tree && property == IDs::syncEnabled)
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncButtonComponent)
};
