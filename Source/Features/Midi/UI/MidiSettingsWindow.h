#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../MappingStore.h"
#include "../MidiDeviceManager.h"
#include "../MidiInboundRouter.h"
#include "../SoftTakeoverManager.h"

namespace sonik::midi
{
    /** PRD-0048: Window shell that hosts the MIDI Learn & Mapping Manager
        organism.  Phase 2 provides only the framing window; the actual
        `MidiSettingsPanel` organism is added in Phase 3+.

        Ownership: created lazily by `SonikApplication` the first time the
        user clicks the toolbar "MIDI" button.  Closing the window deletes
        the instance via `onClose`. */
    class MidiSettingsWindow final : public juce::DocumentWindow
    {
    public:
        MidiSettingsWindow (MappingStore&        store,
                            MidiDeviceManager&   deviceManager,
                            MidiInboundRouter&   router,
                            SoftTakeoverManager& softTakeover);

        ~MidiSettingsWindow() override = default;

        void closeButtonPressed() override;

        /** Called on the message thread when the user closes the window.
            The application is expected to `reset()` its owning unique_ptr. */
        std::function<void()> onClose;

    private:
        MappingStore&        mappingStore;
        MidiDeviceManager&   deviceManager;
        MidiInboundRouter&   inboundRouter;
        SoftTakeoverManager& softTakeoverManager;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSettingsWindow)
    };
}
