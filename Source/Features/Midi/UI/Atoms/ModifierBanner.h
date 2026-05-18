#pragma once
//==============================================================================
// PRD-0048 Atom: ModifierBanner
//
// Real-time visualiser of the modifier mask currently held on a single
// device.  Polls `MidiInboundRouter::getModifierMask` at 30 Hz, resolves
// each set bit to a human-readable name via `getModifierBitName`, and
// displays them as a comma-separated list ("Modifiers held: SHIFT, FX1").
//
// When no bits are set, the banner is collapsed to zero height to avoid
// adding visual noise.
//
// Styling per DESIGN.md: monochrome #FDFDFD bg, 2px ink border, Space Mono,
// zero border-radius, fill inversion on the "active" state.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingStore.h"
#include "../../MidiInboundRouter.h"

#include <cstdint>

namespace sonik::midi::ui
{
    class ModifierBanner final : public juce::Component,
                                 private juce::Timer
    {
    public:
        ModifierBanner (std::uint64_t      deviceIdIn,
                        MidiInboundRouter& routerIn,
                        MappingStore&      storeIn)
            : deviceId (deviceIdIn),
              router   (routerIn),
              store    (storeIn)
        {
            // 30 Hz per PRD-0048 §AC: "updates within 50 ms of a modifier
            // bit change" → 33 ms poll interval comfortably under budget.
            startTimerHz (30);
        }

        ~ModifierBanner() override { stopTimer(); }

        int getPreferredHeight() const noexcept
        {
            return hasAnyHeld ? 22 : 0;
        }

        void paint (juce::Graphics& g) override
        {
            if (! hasAnyHeld) return;

            constexpr juce::uint32 kBg     = 0xFF2D2D2D; // inverted: active state
            constexpr juce::uint32 kFg     = 0xFFFDFDFD;
            constexpr juce::uint32 kBorder = 0xFF2D2D2D;

            g.fillAll (juce::Colour (kBg));
            g.setColour (juce::Colour (kBorder));
            g.drawRect (getLocalBounds(), 2);

            g.setColour (juce::Colour (kFg));
            g.setFont (juce::Font (juce::FontOptions { "Space Mono", 12.0f, juce::Font::plain }));
            g.drawText ("MODIFIERS HELD: " + displayText,
                        getLocalBounds().reduced (8, 2),
                        juce::Justification::centredLeft,
                        true);
        }

    private:
        void timerCallback() override
        {
            const auto mask = router.getModifierMask (deviceId);
            if (mask == lastMask) return;
            lastMask = mask;

            auto mapping = store.getActiveMappingForDevice (deviceId);
            juce::StringArray names;
            if (mapping != nullptr)
            {
                for (std::uint8_t bit = 0; bit < 32; ++bit)
                {
                    if ((mask & (1u << bit)) == 0u) continue;
                    auto name = router.getModifierBitName (*mapping, bit);
                    names.add (name.has_value() ? *name : ("bit" + juce::String ((int) bit)));
                }
            }

            const bool wasHeld = hasAnyHeld;
            hasAnyHeld  = ! names.isEmpty();
            displayText = names.joinIntoString (", ");

            if (wasHeld != hasAnyHeld)
            {
                if (auto* parent = getParentComponent())
                    parent->resized();
            }
            repaint();
        }

        std::uint64_t      deviceId;
        MidiInboundRouter& router;
        MappingStore&      store;

        std::uint32_t lastMask    { 0xFFFFFFFFu }; // force first repaint
        bool          hasAnyHeld  { false };
        juce::String  displayText;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModifierBanner)
    };
}
