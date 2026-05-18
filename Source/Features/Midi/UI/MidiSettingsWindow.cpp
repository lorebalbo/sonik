#include "MidiSettingsWindow.h"

#include "MidiSettingsPanel.h"

namespace sonik::midi
{
    namespace
    {
        // PRD-0048 / DESIGN.md: strict monochrome palette.
        constexpr juce::uint32 kBg = 0xFFFDFDFD; // surface
    }

    //--------------------------------------------------------------------------
    MidiSettingsWindow::MidiSettingsWindow (MappingStore&        store,
                                            MidiDeviceManager&   deviceMgr,
                                            MidiInboundRouter&   router,
                                            SoftTakeoverManager& softTakeover)
        : juce::DocumentWindow ("Sonik - MIDI",
                                juce::Colour (kBg),
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          mappingStore       (store),
          deviceManager      (deviceMgr),
          inboundRouter      (router),
          softTakeoverManager(softTakeover)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ui::MidiSettingsPanel (mappingStore,
                                                    deviceManager,
                                                    inboundRouter,
                                                    softTakeoverManager),
                         false);
        setResizable (true, true);
        setResizeLimits (640, 480, 2400, 1600);
        centreWithSize (960, 640);
        setVisible (true);
    }

    void MidiSettingsWindow::closeButtonPressed()
    {
        // Defer destruction to the message loop so we don't delete `this`
        // from within the handler.
        if (onClose)
        {
            auto callback = std::move (onClose);
            juce::MessageManager::callAsync ([fn = std::move (callback)]() { fn(); });
        }
        else
        {
            setVisible (false);
        }
    }
}
