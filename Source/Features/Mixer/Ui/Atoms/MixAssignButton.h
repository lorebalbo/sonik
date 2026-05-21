#pragma once
//==============================================================================
// PRD-0059: MixAssignButton — a binary-latch button that assigns a channel
// to crossfader side A or B (or any other latching mixer assignment).
//
// Visually identical to MixKillButton (full inversion on active, 2-px ink
// border, zero border-radius, Space Mono label). The label is the only
// differentiator at the visual level; the binding semantic is independent.
//==============================================================================

#include "MixKillButton.h"

class MixAssignButton final : public juce::Component,
                                private juce::ValueTree::Listener
{
public:
    MixAssignButton (juce::ValueTree boundTree,
                     juce::Identifier propertyId,
                     juce::String     labelText);

    ~MixAssignButton() override;

    bool isActive() const noexcept              { return active; }
    void setActive (bool shouldBeActive);
    void toggle();

    const juce::String& getLabelText() const noexcept { return label; }

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

    static constexpr int kMinimumSize = 16;     // PRD §1.4

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void readFromTree();
    void writeToTree();

    juce::ValueTree  tree;
    juce::Identifier propertyId;
    juce::String     label;
    bool             active = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixAssignButton)
};
