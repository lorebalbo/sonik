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

        updateAddDeckButton();
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
        addDeckButton.setBounds (bounds.removeFromRight (100).withSizeKeepingCentre (100, 28));
    }

    void updateAddDeckButton()
    {
        bool atMax = deckStateManager.getDeckCount() >= 4;
        addDeckButton.setEnabled (! atMax);
        addDeckButton.setTooltip (atMax ? "Maximum 4 decks reached" : juce::String());
    }

    std::function<void()> onAddDeckClicked;

private:
    DeckStateManager& deckStateManager;
    juce::Label       titleLabel;
    juce::TextButton  addDeckButton;

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalToolbar)
};
