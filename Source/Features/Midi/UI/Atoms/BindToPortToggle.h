#pragma once
//==============================================================================
// PRD-0051 Atom: BindToPortToggle
//
// A two-state monochrome toggle styled per DESIGN.md (2px ink border, fill
// inversion when ON, Space Mono, zero radius). Sits inside `DeviceHeader`'s
// PortBindingControls row.
//
// Visually identical to LearnButton's palette so the panel feels consistent.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonik::midi::ui
{
    class BindToPortToggle final : public juce::TextButton
    {
    public:
        BindToPortToggle()
        {
            setClickingTogglesState (true);
            setButtonText ("BIND TO THIS PORT");

            // DESIGN.md palette: monochrome, fill inverts when active.
            setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFFFDFDFD));
            setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF2D2D2D));
            setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFF2D2D2D));
            setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFFFDFDFD));
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BindToPortToggle)
    };
}
