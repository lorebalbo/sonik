#pragma once
//==============================================================================
// PRD-0048 Atom: LoadErrorBanner
//
// A monochrome banner displayed at the top of the MIDI Settings panel when
// MappingStore::getLoadErrors() returns at least one entry.  Lists each
// failed file (basename + message) and offers a "Reload" action.
//
// Styling follows DESIGN.md: #FDFDFD bg, 2px solid #2D2D2D border, Space
// Mono, no border-radius.  The banner is intentionally an Atom: it owns no
// store reference, instead it exposes setErrors() and an onReload callback.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingStore.h"

#include <functional>
#include <vector>

namespace sonik::midi::ui
{
    class LoadErrorBanner final : public juce::Component
    {
    public:
        std::function<void()> onReload;

        LoadErrorBanner()
        {
            reloadButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFFFDFDFD));
            reloadButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF2D2D2D));
            reloadButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFFFDFDFD));
            reloadButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFF2D2D2D));
            reloadButton.setButtonText ("RELOAD");
            reloadButton.onClick = [this]() { if (onReload) onReload(); };
            addAndMakeVisible (reloadButton);
        }

        void setErrors (std::vector<MappingLoadError> errs)
        {
            errors = std::move (errs);
            setVisible (! errors.empty());
            if (auto* parent = getParentComponent())
                parent->resized();
            repaint();
        }

        int getPreferredHeight() const noexcept
        {
            if (errors.empty()) return 0;
            // 24px header + 18px per error row + 8px padding top/bottom.
            return 24 + 18 * (int) errors.size() + 16;
        }

        void paint (juce::Graphics& g) override
        {
            constexpr juce::uint32 kBg     = 0xFFFDFDFD;
            constexpr juce::uint32 kFg     = 0xFF2D2D2D;
            constexpr juce::uint32 kBorder = 0xFF2D2D2D;

            g.fillAll (juce::Colour (kBg));
            g.setColour (juce::Colour (kBorder));
            g.drawRect (getLocalBounds(), 2);

            g.setColour (juce::Colour (kFg));
            g.setFont (juce::Font (juce::FontOptions { "Space Mono", 13.0f, juce::Font::bold }));
            auto bounds = getLocalBounds().reduced (10, 8);
            const auto headerArea = bounds.removeFromTop (20);
            g.drawText ("MAPPING LOAD ERRORS (" + juce::String ((int) errors.size()) + ")",
                        headerArea, juce::Justification::centredLeft, true);

            // Reserve space for reload button on the right of header.
            bounds.removeFromTop (4);
            g.setFont (juce::Font (juce::FontOptions { "Space Mono", 11.0f, juce::Font::plain }));
            for (const auto& e : errors)
            {
                auto row = bounds.removeFromTop (18);
                const auto basename = juce::File (e.sourcePath).getFileName();
                const auto line = basename + " : " + e.message;
                g.drawText (line, row, juce::Justification::centredLeft, true);
            }
        }

        void resized() override
        {
            // Place the RELOAD button in the top-right corner of the banner.
            auto top = getLocalBounds().reduced (10, 8).removeFromTop (20);
            const int btnW = 80;
            reloadButton.setBounds (top.removeFromRight (btnW));
        }

    private:
        std::vector<MappingLoadError> errors;
        juce::TextButton               reloadButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoadErrorBanner)
    };
}
