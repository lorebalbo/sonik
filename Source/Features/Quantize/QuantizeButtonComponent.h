#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../Deck/DeckIdentifiers.h"

/// "Q" toggle button for per-deck quantize mode.
/// Follows DESIGN.md: monochrome palette, zero border-radius, no gradients.
class QuantizeButtonComponent final : public juce::Component,
                                       public juce::SettableTooltipClient,
                                       private juce::ValueTree::Listener
{
public:
    explicit QuantizeButtonComponent (juce::ValueTree deckTree)
        : tree (deckTree)
    {
        enabled = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
        hasBeatgrid = getBeatgridBpm() > 0.0;
        deckStatus = tree.getProperty (IDs::playbackStatus, "empty").toString();

        setTooltip ("Quantize: snap cues, loops, and jumps to beat positions");
        setRepaintsOnMouseActivity (true); // instant hover feedback (DESIGN.md §6)
        updateCursor();
        tree.addListener (this);
    }

    ~QuantizeButtonComponent() override
    {
        tree.removeListener (this);
    }

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    double getBeatgridBpm() const
    {
        auto bg = tree.getChildWithName (IDs::BeatGrid);
        if (bg.isValid())
            return static_cast<double> (bg.getProperty (IDs::bpm, 0.0));
        return 0.0;
    }

    void updateCursor()
    {
        setMouseCursor (deckStatus == "empty" ? juce::MouseCursor::NormalCursor
                                              : juce::MouseCursor::PointingHandCursor);
    }

    juce::ValueTree tree;
    bool enabled     = false;
    bool hasBeatgrid = false;
    juce::String deckStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuantizeButtonComponent)
};
