#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"
#include "Features/Shared/Ui/SonikDraw.h"

/// Small toggle button for key-lock (master tempo).
/// Standard DESIGN.md latch: full fill inversion when active.
class KeyLockButton final : public juce::Component,
                             public juce::SettableTooltipClient,
                             private juce::ValueTree::Listener
{
public:
    explicit KeyLockButton (juce::ValueTree deckTree)
        : tree (deckTree)
    {
        enabled = static_cast<bool> (tree.getProperty (IDs::keyLockEnabled, false));
        setTooltip ("Key Lock: keep the musical key constant while pitch-bending");
        setRepaintsOnMouseActivity (true); // instant hover feedback (DESIGN.md §6)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        tree.addListener (this);
    }

    ~KeyLockButton() override
    {
        tree.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        sonik::ui::draw::paintLatchButton (g, getLocalBounds(), "KEY",
                                           { .active = enabled,
                                             .hover  = isMouseOver() });
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
