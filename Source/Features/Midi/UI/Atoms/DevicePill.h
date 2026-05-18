#pragma once
//==============================================================================
// PRD-0048 Atom: DevicePill
//
// Small monochrome pill showing the connection state of a MIDI device
// ("CONNECTED" or "DISCONNECTED").  Strict DESIGN.md palette: 2px border,
// inverted fill on the "connected" state to emphasise active devices.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonik::midi::ui
{
    class DevicePill final : public juce::Component
    {
    public:
        DevicePill() = default;

        void setConnected (bool isConnected)
        {
            if (connected == isConnected)
                return;
            connected = isConnected;
            repaint();
        }

        bool isConnected() const noexcept { return connected; }

        void paint (juce::Graphics& g) override
        {
            constexpr juce::uint32 kBg = 0xFFFDFDFD;
            constexpr juce::uint32 kFg = 0xFF2D2D2D;

            const auto bounds = getLocalBounds().toFloat().reduced (1.0f);

            // Connected: filled ink with light text.
            // Disconnected: light fill with ink text (inverted).
            if (connected)
            {
                g.setColour (juce::Colour (kFg));
                g.fillRect (bounds);
                g.setColour (juce::Colour (kFg));
                g.drawRect (bounds, 2.0f);
                g.setColour (juce::Colour (kBg));
            }
            else
            {
                g.setColour (juce::Colour (kBg));
                g.fillRect (bounds);
                g.setColour (juce::Colour (kFg));
                g.drawRect (bounds, 2.0f);
                g.setColour (juce::Colour (kFg));
            }

            g.setFont (juce::FontOptions ("Space Mono", 10.0f, juce::Font::plain));
            g.drawText (connected ? "CONNECTED" : "DISCONNECTED",
                        getLocalBounds(),
                        juce::Justification::centred,
                        false);
        }

    private:
        bool connected { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePill)
    };
}
