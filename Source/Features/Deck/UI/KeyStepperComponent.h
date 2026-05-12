#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"
#include "../../KeyDetection/KeyUtils.h"

/// < KEY > stepper that shows the current Camelot key (adjusted by keyShift semitones)
/// and lets the user nudge the key up or down by a semitone.
/// Range is signed −12..+12 semitones with clamping (no wrap).
/// Visual style: three adjacent cells separated by 2px borders:
///   [<] [A9] [>]  matching the Figma "Stepper" component.
class KeyStepperComponent final : public juce::Component,
                                   private juce::ValueTree::Listener
{
public:
    explicit KeyStepperComponent (juce::ValueTree deckTree)
        : tree (deckTree)
    {
        tree.addListener (this);
        refreshState();
    }

    ~KeyStepperComponent() override
    {
        tree.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        const int arrowW = 20;

        auto leftBtn  = bounds.removeFromLeft (arrowW);
        auto rightBtn = bounds.removeFromRight (arrowW);
        auto labelBox = bounds;

        // Background for arrow buttons.  When at the clamp edge, the arrow
        // cell is filled with a mid-grey to communicate "disabled".
        const juce::Colour arrowBg     (0xFFF9F9F9);
        const juce::Colour arrowBgDim  (0xFFB0B0B0);

        g.setColour (keyShift <= -12 ? arrowBgDim : arrowBg);
        g.fillRect (leftBtn);
        g.setColour (keyShift >= 12 ? arrowBgDim : arrowBg);
        g.fillRect (rightBtn);

        // Background for label (dark — active display)
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.fillRect (labelBox);

        // Borders — 2px, matching Figma
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (leftBtn, 2);
        g.drawRect (labelBox, 2);
        g.drawRect (rightBtn, 2);

        // Arrow labels
        auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
        g.setFont (monoFont);

        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawText ("<", leftBtn, juce::Justification::centred);
        g.drawText (">", rightBtn, juce::Justification::centred);

        // Key label (white on dark)
        g.setColour (juce::Colour (0xFFF9F9F9));
        g.drawText (getDisplayKey(), labelBox, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto bounds = getLocalBounds();
        const int arrowW = 20;

        if (e.x < arrowW)
            adjustShift (-1);
        else if (e.x >= bounds.getWidth() - arrowW)
            adjustShift (+1);
    }

private:
    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override
    {
        if (changedTree == tree && (property == IDs::keyShift))
        {
            refreshState();
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }

        // Also listen to the KeyInfo child for the base key
        if (changedTree.hasType (IDs::KeyInfo) && property == IDs::keyIndex)
        {
            refreshState();
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }

    void refreshState()
    {
        keyShift = juce::jlimit (-12, 12,
                                  static_cast<int> (tree.getProperty (IDs::keyShift, 0)));

        auto keyTree = tree.getChildWithName (IDs::KeyInfo);
        if (keyTree.isValid())
            baseKeyIndex = static_cast<int> (keyTree.getProperty (IDs::keyIndex, -1));
        else
            baseKeyIndex = -1;
    }

    void adjustShift (int delta)
    {
        int newShift = juce::jlimit (-12, 12, keyShift + delta);
        if (newShift == keyShift)
            return;  // at clamp edge

        keyShift = newShift;
        tree.setProperty (IDs::keyShift, keyShift, nullptr);
        repaint();
    }

    juce::String getDisplayKey() const
    {
        if (baseKeyIndex < 0)
        {
            // No key detected yet — show the signed semitone offset.
            if (keyShift == 0)  return "--";
            return (keyShift > 0 ? juce::String ("+") : juce::String())
                   + juce::String (keyShift);
        }

        // Camelot wheel has 24 entries (12 keys × 2 modes major/minor).
        // The key changes by one Camelot index per semitone within the same
        // mode; mode (major/minor) does not change with pitch shift.
        // Use positive modulo to handle negative shifts safely.
        int adjusted = ((baseKeyIndex + keyShift) % 24 + 24) % 24;
        return KeyUtils::toCamelot (adjusted);
    }

    juce::ValueTree tree;
    int baseKeyIndex = -1;
    int keyShift     = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyStepperComponent)
};
