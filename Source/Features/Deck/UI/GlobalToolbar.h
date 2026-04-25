#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"

class GlobalToolbar final : public juce::Component
{
public:
    explicit GlobalToolbar (DeckStateManager& deckState)
        : deckStateManager (deckState)
    {
        // Title
        titleLabel.setText ("SONIK", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (16.0f).withStyle ("Bold"));
        titleLabel.setColour (juce::Label::textColourId, juce::Colours::black);
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (titleLabel);

        // Add Deck button
        addDeckButton.setButtonText ("ADD DECK");
        addDeckButton.setColour (juce::TextButton::buttonColourId, juce::Colours::black);
        addDeckButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xFFF9F9F9));
        addDeckButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFF9F9F9));
        addDeckButton.onClick = [this]()
        {
            if (onAddDeckClicked)
                onAddDeckClicked();
        };
        addAndMakeVisible (addDeckButton);

        // Remove Deck button
        removeDeckButton.setButtonText ("REMOVE DECK");
        removeDeckButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFFF3C3B));
        removeDeckButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xFFFFFFFF));
        removeDeckButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFFFFFFF));
        removeDeckButton.onClick = [this]()
        {
            if (onRemoveDeckClicked)
                onRemoveDeckClicked();
        };
        addAndMakeVisible (removeDeckButton);

        updateButtons();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xFFE2E2E2)); // surface-container-highest
        g.fillRect (getLocalBounds());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (8, 0);
        titleLabel.setBounds (bounds.removeFromLeft (200));
        addDeckButton.setBounds    (bounds.removeFromRight (100).withSizeKeepingCentre (100, 28));
        bounds.removeFromRight (8);
        removeDeckButton.setBounds (bounds.removeFromRight (120).withSizeKeepingCentre (120, 28));
    }

    void updateAddDeckButton()
    {
        bool atMax = deckStateManager.getDeckCount() >= 4;
        addDeckButton.setEnabled (! atMax);
        addDeckButton.setTooltip (atMax ? "Maximum 4 decks reached" : juce::String());
    }

    void updateRemoveDeckButton()
    {
        auto activeId = deckStateManager.getActiveDeckId();
        bool canRemove = deckStateManager.canRemoveDeck (activeId);
        removeDeckButton.setEnabled (canRemove);

        juce::String tip;
        if (deckStateManager.getDeckCount() <= 1)
            tip = "Cannot remove the last deck";
        else if (! canRemove)
            tip = "Stop playback to remove this deck";
        removeDeckButton.setTooltip (tip);
    }

    void updateButtons()
    {
        updateAddDeckButton();
        updateRemoveDeckButton();
    }

    std::function<void()> onAddDeckClicked;
    std::function<void()> onRemoveDeckClicked;

private:
    DeckStateManager& deckStateManager;
    juce::Label       titleLabel;
    juce::TextButton  addDeckButton;
    juce::TextButton  removeDeckButton;

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalToolbar)
};
