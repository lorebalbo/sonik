#pragma once
//==============================================================================
// PRD-0059: MixKillButton — a binary-latch button atom for the EQ kill bands
// (and any other "press to engage" boolean control in the mixer).
//
// DESIGN.md compliance:
//   • Inactive: #fdfdfd fill, #2d2d2d text, 2-px #2d2d2d border.
//   • Active:   #2d2d2d fill, #fdfdfd text, 2-px #2d2d2d border (full inversion).
//   • Zero border-radius. Space Mono Regular label.
//   • Instant state change (no fade animation).
//
// Binding:
//   • One-way ↔ ValueTree boolean. Toggle on click; external writers
//     (MIDI, other UIs) are picked up via juce::ValueTree::Listener.
//   • No audio-thread interaction.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

class MixKillButton final : public juce::Component,
                             private juce::ValueTree::Listener
{
public:
    MixKillButton (juce::ValueTree boundTree,
                   juce::Identifier propertyId,
                   juce::String     labelText = "KILL");

    ~MixKillButton() override;

    bool isActive() const noexcept              { return active; }
    void setActive (bool shouldBeActive);
    void toggle();

    // When true the button renders as a plain circle (filled = active,
    // outline = inactive) instead of the standard rectangle+text style.
    void setCircleStyle (bool shouldUseCircle) noexcept { circleStyle = shouldUseCircle; repaint(); }
    bool isCircleStyle() const noexcept { return circleStyle; }

    const juce::String& getLabelText() const noexcept { return label; }

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

    static constexpr int kMinimumSize = 16;     // PRD §1.4

protected:
    // Shared paint helpers — reused by MixAssignButton which is visually
    // identical with a different label/binding semantic.
    void paintLatchedButton (juce::Graphics& g, bool isActiveNow,
                             const juce::String& text) const;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void readFromTree();
    void writeToTree();

    juce::ValueTree  tree;
    juce::Identifier propertyId;
    juce::String     label;
    bool             active      = false;
    bool             circleStyle = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixKillButton)
};
