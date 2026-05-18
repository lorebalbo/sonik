#pragma once
//==============================================================================
// PRD-0051 Molecule: PortBindingControls
//
// Encapsulates the per-device port-identifier label + "Bind to This Port"
// toggle that lives inside each `DeviceHeader` row. Handles tooltip messaging
// for the three states:
//   - Active user profile, identifier path available     -> toggle live
//   - Active *bundled* profile                           -> toggle disabled, hint to Duplicate first
//   - OS-reported identifiers unstable on this platform  -> toggle disabled, platform hint
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Atoms/BindToPortToggle.h"

#include <cstdint>
#include <functional>

namespace sonik::midi::ui
{
    class PortBindingControls final : public juce::Component
    {
    public:
        PortBindingControls();

        void paint   (juce::Graphics&) override;
        void resized() override;

        struct State
        {
            juce::String juceIdentifier;       // raw OS identifier; may be empty
            bool         bound          { false };  // mapping currently has identifierHint == identifier
            bool         activeIsBundled { false }; // disable + tooltip "duplicate first"
            bool         platformAvailable { true }; // disable + tooltip "OS does not provide stable ids"
        };

        void setState (const State&);

        /** Called when the user clicks the toggle. The new requested
            "should be bound" boolean is passed; the parent commits it to
            `MappingStore::setIdentifierHint`. */
        std::function<void (bool /*shouldBind*/)> onToggle;

        int getPreferredHeight() const noexcept { return 26; }

    private:
        static juce::String formatIdentifierForLabel (const juce::String& raw);

        juce::Label        identifierLabel;
        BindToPortToggle   toggle;
        State              currentState;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PortBindingControls)
    };
}
