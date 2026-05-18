#include "PortBindingControls.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kFg = 0xFF2D2D2D;

        constexpr const char* kTooltipBound =
            "This profile is bound to this physical USB port. "
            "Plugging this controller into a different port will move the binding.";
        constexpr const char* kTooltipUnboundEditable =
            "Pin this profile to this physical USB port. Required when two "
            "identical controllers are connected and each needs its own mapping.";
        constexpr const char* kTooltipBundled =
            "Duplicate to a User Profile first to bind this mapping to a "
            "specific USB port. Bundled profiles cannot be modified.";
        constexpr const char* kTooltipPlatformUnstable =
            "Your operating system does not provide stable USB-port "
            "identifiers. Multi-instance disambiguation is unavailable on "
            "this platform.";
    }

    PortBindingControls::PortBindingControls()
    {
        identifierLabel.setColour (juce::Label::textColourId, juce::Colour (kFg));
        identifierLabel.setFont   (juce::FontOptions ("Space Mono", 10.0f, juce::Font::plain));
        identifierLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (identifierLabel);

        toggle.onClick = [this]()
        {
            if (onToggle != nullptr)
                onToggle (toggle.getToggleState());
        };
        addAndMakeVisible (toggle);

        State s; setState (s);
    }

    void PortBindingControls::paint (juce::Graphics&) {}

    void PortBindingControls::resized()
    {
        auto bounds = getLocalBounds();
        constexpr int kToggleW = 170;

        toggle.setBounds (bounds.removeFromRight (kToggleW));
        bounds.removeFromRight (8);
        identifierLabel.setBounds (bounds);
    }

    juce::String PortBindingControls::formatIdentifierForLabel (const juce::String& raw)
    {
        if (raw.isEmpty())
            return juce::String ("port: <none>");

        constexpr int kMaxChars = 16;
        if (raw.length() <= kMaxChars)
            return juce::String ("port: ") + raw;

        // Ellipsise on the left (the right-hand side is the unique tail on
        // both Core MIDI and WinRT MIDI identifiers per PRD §1.3.1).
        const auto tail = raw.substring (raw.length() - kMaxChars);
        return juce::String (juce::CharPointer_UTF8 ("port: \xE2\x80\xA6")) + tail;
    }

    void PortBindingControls::setState (const State& s)
    {
        currentState = s;

        identifierLabel.setText (formatIdentifierForLabel (s.juceIdentifier),
                                 juce::dontSendNotification);

        toggle.setToggleState (s.bound, juce::dontSendNotification);

        const bool platformOk = s.platformAvailable;
        const bool editable   = ! s.activeIsBundled;
        const bool enabled    = platformOk && editable;
        toggle.setEnabled (enabled);

        if (! platformOk)
            toggle.setTooltip (kTooltipPlatformUnstable);
        else if (! editable)
            toggle.setTooltip (kTooltipBundled);
        else if (s.bound)
            toggle.setTooltip (kTooltipBound);
        else
            toggle.setTooltip (kTooltipUnboundEditable);
    }
}
