#pragma once
//==============================================================================
// PRD-0048 Organism: MidiSettingsPanel
//
// Root component of the MIDI Settings window.  Phase 3 scope: enumerates
// currently-known devices from `MidiDeviceManager`, renders one
// `DeviceHeader` per device inside a vertical `juce::Viewport`, and reacts
// to device add/remove + mapping store changes.
//
// Sections that will be wired in later phases (binding table, modifier
// banner, disengaged list, load-error banner) are intentionally absent
// here — Phase 4+ will introduce them under each DeviceHeader.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DeviceListChangeListener.h"
#include "../MappingStore.h"
#include "../Bundle/MappingExportService.h"
#include "../Bundle/MappingImportService.h"
#include "Atoms/LoadErrorBanner.h"
#include "../MidiDeviceManager.h"
#include "../MidiInboundRouter.h"
#include "../SoftTakeoverManager.h"
#include "Molecules/DeviceHeader.h"

namespace sonik::midi::ui
{
    class MidiSettingsPanel final : public juce::Component,
                                    private sonik::midi::DeviceListChangeListener,
                                    private sonik::midi::MappingStoreListener
    {
    public:
        MidiSettingsPanel (MappingStore&        store,
                           MidiDeviceManager&   deviceManager,
                           MidiInboundRouter&   router,
                           SoftTakeoverManager& softTakeover,
                           juce::String         appVersion = "0.1.0");
        ~MidiSettingsPanel() override;

        void paint   (juce::Graphics&) override;
        void resized() override;

    private:
        // Inner stacked-content component that lives inside the Viewport.
        class Content final : public juce::Component
        {
        public:
            void resized() override;
            void setHeaders (std::vector<std::unique_ptr<DeviceHeader>>&);
        };

        void rebuildDeviceList();           // Message thread
        void rebuildOnMessageThread();      // schedules + dedups

        // PRD-0050: Import/Export wiring.
        void onImportClicked();
        void onExportClicked();
        void launchImportFlow (const juce::File& source);
        void launchExportFlow (const juce::String& mappingId);
        std::uint64_t firstActiveInputDeviceId() const;

        // ---- DeviceListChangeListener -------------------------------------
        void midiDeviceAdded   (std::uint64_t) override;
        void midiDeviceRemoved (std::uint64_t) override;
        void midiDeviceOpened  (std::uint64_t) override;
        void midiDeviceClosed  (std::uint64_t) override;

        // ---- MappingStoreListener -----------------------------------------
        void userProfilesLoaded() override;
        void activeMappingChanged (std::uint64_t deviceId) override;
        void mappingAdded   (juce::String) override;
        void mappingRemoved (juce::String) override;

        // Backref keepers --------------------------------------------------
        MappingStore&        store;
        MidiDeviceManager&   deviceManager;
        MidiInboundRouter&   inboundRouter;
        SoftTakeoverManager& softTakeoverManager;

        juce::Viewport                              viewport;
        Content                                     content;
        LoadErrorBanner                             loadErrorBanner;
        std::vector<std::unique_ptr<DeviceHeader>>  headers;

        // PRD-0050: Import / Export toolbar + services.
        juce::Component                             toolbar;
        juce::TextButton                            importButton { "Import\xE2\x80\xA6" };
        juce::TextButton                            exportButton { "Export\xE2\x80\xA6" };
        juce::ThreadPool                            ioPool { 1 };
        std::unique_ptr<MappingExportService>       exportService;
        std::unique_ptr<MappingImportService>       importService;
        std::unique_ptr<juce::FileChooser>          activeFileChooser;
        std::unique_ptr<juce::Component>            activeDialog;
        std::unique_ptr<juce::DialogWindow>         activeDialogWindow;

        void refreshLoadErrorBanner();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSettingsPanel)
    };
}
