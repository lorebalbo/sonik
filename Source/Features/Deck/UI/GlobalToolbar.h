#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "Features/Shared/Ui/SonikTheme.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerMeterSnapshot.h"
#include "Features/Mixer/State/MixerIdentifiers.h"
#include "Features/Mixer/Ui/Atoms/MixRotaryKnob.h"
#include "Features/Mixer/Ui/Atoms/MixLevelMeter.h"

class GlobalToolbar final : public juce::Component,
                            private juce::Timer
{
public:
    GlobalToolbar (DeckStateManager& deckState,
                   MixerStateSchema& mixerSchema,
                   MixerMeterSnapshot& mixerMeters)
        : deckStateManager (deckState),
          masterKnob  (mixerSchema.getMasterTree(),
                       MixerIDs::gain,
                       makeMasterKnobConfig()),
          masterMeter (mixerMeters.master, juce::String())
    {
        // Title — Space Mono bold ink, like a printed chassis label.
        titleLabel.setText ("SONIK", juce::dontSendNotification);
        titleLabel.setFont (sonik::ui::theme::mono (16.0f, true));
        titleLabel.setColour (juce::Label::textColourId, sonik::ui::theme::ink());
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (titleLabel);

        // Live clock — echoes the Figma header status cluster (HH:MM, ink mono).
        clockLabel.setFont (sonik::ui::theme::mono (12.0f, false));
        clockLabel.setColour (juce::Label::textColourId, sonik::ui::theme::ink());
        clockLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (clockLabel);
        updateClock();
        startTimerHz (1);

        // Add Deck button — chrome comes from SonikLookAndFeel (flat surface
        // fill, 2px ink border, instant hover/press inversion).
        addDeckButton.setButtonText ("ADD DECK");
        addDeckButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        addDeckButton.onClick = [this]()
        {
            if (onAddDeckClicked)
                onAddDeckClicked();
        };
        addAndMakeVisible (addDeckButton);

        // PRD-0048: MIDI settings button.
        midiButton.setButtonText ("MIDI");
        midiButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        midiButton.setTooltip ("MIDI controller mappings");
        midiButton.onClick = [this]()
        {
            if (onMidiClicked)
                onMidiClicked();
        };
        addAndMakeVisible (midiButton);

        // PRD-0060 (revised): master section in the global toolbar.
        addAndMakeVisible (masterKnob);
        addAndMakeVisible (masterMeter);

        updateButtons();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (sonik::ui::theme::containerHighest());
        g.fillRect (getLocalBounds());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (8, 0);
        titleLabel.setBounds (bounds.removeFromLeft (200));

        // Far-right live clock (Figma status cluster).
        clockLabel.setBounds (bounds.removeFromRight (52).withSizeKeepingCentre (52, getHeight()));
        bounds.removeFromRight (10);

        addDeckButton.setBounds (bounds.removeFromRight (100).withSizeKeepingCentre (100, 28));
        bounds.removeFromRight (8); // gap
        midiButton.setBounds (bounds.removeFromRight (80).withSizeKeepingCentre (80, 28));
        bounds.removeFromRight (6); // gap

        // Master section: compact rotary + thin vertical meter sitting to
        // the LEFT of the MIDI button. Total footprint ≈ 8 (meter) + 6 + 32
        // (knob) = 46 px wide, fitting within toolbarHeight (40).
        const int knobBox = juce::jmin (32, getHeight() - 4);
        masterKnob.setBounds (bounds.removeFromRight (knobBox)
                                    .withSizeKeepingCentre (knobBox, knobBox));
        bounds.removeFromRight (6);
        masterMeter.setBounds (bounds.removeFromRight (8)
                                     .withSizeKeepingCentre (8, getHeight() - 8));
    }

    void updateAddDeckButton()
    {
        bool atMax = deckStateManager.getDeckCount() >= 4;
        addDeckButton.setEnabled (! atMax);
        addDeckButton.setTooltip (atMax ? "Maximum 4 decks reached" : juce::String());
    }

    void updateButtons()
    {
        updateAddDeckButton();
    }

    void timerCallback() override { updateClock(); }

    void updateClock()
    {
        const auto now = juce::Time::getCurrentTime();
        clockLabel.setText (now.formatted ("%H:%M"), juce::dontSendNotification);
    }

    // Test accessors.
    MixRotaryKnob& getMasterKnob() noexcept  { return masterKnob; }
    MixLevelMeter& getMasterMeter() noexcept { return masterMeter; }

    std::function<void()> onAddDeckClicked;
    std::function<void()> onMidiClicked;

private:
    static MixRotaryKnob::Config makeMasterKnobConfig()
    {
        MixRotaryKnob::Config cfg;
        cfg.label          = "MAST";
        cfg.taper          = MixRotaryKnob::Normalisation::DbTapered;
        cfg.minValue       = -60.0f;
        cfg.maxValue       =  12.0f;
        cfg.defaultValue   =   0.0f;
        cfg.wheelIncrement =   0.5f;
        cfg.compact        = true;
        return cfg;
    }

    DeckStateManager& deckStateManager;
    juce::Label       titleLabel;
    juce::Label       clockLabel;
    juce::TextButton  addDeckButton;
    juce::TextButton  midiButton;
    MixRotaryKnob     masterKnob;
    MixLevelMeter     masterMeter;

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalToolbar)
};
