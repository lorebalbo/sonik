#pragma once
//==============================================================================
// PRD-0048 Atom: LearnButton
//
// A small monochrome button that toggles between "LEARN" and "LEARNING…"
// states.  Used inside each editable BindingTable row.  Styling follows
// DESIGN.md (2px ink border, fill inversion on the learning state).
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonik::midi::ui
{
    class LearnButton final : public juce::TextButton
    {
    public:
        LearnButton()
        {
            setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFFFDFDFD));
            setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF2D2D2D));
            setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFFFDFDFD));
            setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFF2D2D2D));
            setIdle();
        }

        void setIdle()
        {
            setButtonText ("LEARN");
            setToggleState (false, juce::dontSendNotification);
        }

        void setLearning (int secondsRemaining)
        {
            setButtonText ("WAIT " + juce::String (secondsRemaining) + "s");
            setToggleState (true, juce::dontSendNotification);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LearnButton)
    };
}
