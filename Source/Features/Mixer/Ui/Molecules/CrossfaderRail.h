#pragma once
//==============================================================================
// PRD-0060: CrossfaderRail — horizontal crossfader plus curve selector.
//
// Composes:
//   • MixFader (orientation = Horizontal, detent = 0.5) bound to
//     "mixer.crossfader".
//   • A two-state curve toggle (SMOOTH / SHARP) bound to
//     "mixer.crossfaderCurve" string property (PRD-0057).
//
// Layout: curve toggle on the left in a compact 64 px gutter, fader fills
// the remaining width. DESIGN.md compliance: 2-px ink border on the chassis,
// zero border-radius, Space Mono labels.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Atoms/MixFader.h"

class CrossfaderRail final : public juce::Component,
                              private juce::ValueTree::Listener
{
public:
    /// @param mixerTree the top-level "Mixer" ValueTree.
    explicit CrossfaderRail (juce::ValueTree mixerTree);
    ~CrossfaderRail() override;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

    MixFader& getFader() noexcept { return fader; }

    bool isCurveSmooth() const noexcept { return curveIsSmooth; }
    void setCurveSmooth (bool smooth);

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void readCurveFromTree();
    void commitCurveToTree();
    juce::Rectangle<int> getCurveButtonArea() const;

    juce::ValueTree mixerTree;
    MixFader        fader;
    bool            curveIsSmooth { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrossfaderRail)
};
