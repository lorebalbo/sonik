#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioFileLoader.h"
#include "Features/Waveform/WaveformManager.h"
#include "Features/BeatGrid/BeatGridManager.h"
#include "Features/KeyDetection/KeyDetectionManager.h"
#include "Features/Deck/UI/MainContentComponent.h"
#include "Features/StemSeparation/ModelManager.h"
#include "Features/StemSeparation/StemSeparationManager.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Sync/MasterClockManager.h"
#include "Features/Library/LibraryAnalysisService.h"
#include "Features/Library/LibraryAnalysisQueue.h"
#include "Features/Library/WatchFolderScanner.h"
#include "Features/Midi/JuceMidiHost.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiMessageBridge.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MidiInboundRouter.h"
#include "Features/Midi/MidiFeedbackEngine.h"
#include "Features/Midi/SoftTakeoverManager.h"
#include "MidiHandlers/DeckMidiHandler.h"
#include "MidiHandlers/MixerMidiHandler.h"
#include "MidiHandlers/LibraryMidiHandler.h"
#include "MidiHandlers/CompositeMidiCommandHandler.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerAtomicSnapshot.h"
#include "Features/Mixer/State/MixerMeterSnapshot.h"
#include "Features/Mixer/State/MixerStateBridge.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/Session/SessionSerializer.h"
#include "Features/Daw/Session/SessionController.h"
#include "Features/Daw/Session/SessionSourceResolution.h"
#include "Features/Daw/Session/Ui/SessionMenu.h"
#include "Features/Daw/Import/ImportSource.h"
#include "Features/Daw/Import/ImportSourcePublisher.h"
#include "Features/Daw/Import/ImportClipPlacer.h"
#include "Features/Daw/Import/AudioFileImporter.h"
#include "Features/Midi/UI/MidiSettingsWindow.h"
#include <memory>

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (AudioFileLoader& loader, DeckStateManager& deckState,
                AudioEngine& engine, WaveformManager& waveformMgr,
                BeatGridManager& beatGridMgr, StemSeparationManager& stemMgr,
                MasterClockManager& clockMgr, LibraryAnalysisQueue& analysisQueue,
                TrackDatabase& trackDb,
                MixerStateSchema& mixerSchema,
                MixerMeterSnapshot& mixerMeters,
                Daw::MasterGridService& gridService)
        : DocumentWindow ("Sonik",
                          juce::Colour (0xfff9f9f9),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainContentComponent (deckState, engine, loader, waveformMgr,
                                beatGridMgr, stemMgr, clockMgr,
                                analysisQueue, trackDb,
                                mixerSchema, mixerMeters, gridService),
                          true);
        setResizable (true, true);
        setResizeLimits (1120, 600, 3840, 2160);
        centreWithSize (1280, 800);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    MainContentComponent* getContent() const
    {
        return dynamic_cast<MainContentComponent*> (getContentComponent());
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

class SonikApplication final : public juce::JUCEApplication
{
public:
    SonikApplication() = default;

    const juce::String getApplicationName() override    { return "Sonik"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;

private:
    std::unique_ptr<TrackDatabase>    trackDatabase;
    std::unique_ptr<sonik::midi::JuceMidiHost>      midiHost;           // PRD-0040
    std::unique_ptr<sonik::midi::MidiDeviceManager> midiDeviceManager;  // PRD-0040
    std::unique_ptr<sonik::midi::MidiMessageBridge> midiMessageBridge;  // PRD-0041
    std::unique_ptr<sonik::midi::MappingStore>      mappingStore;       // PRD-0043
    std::unique_ptr<sonik::midi::MidiFeedbackEngine> midiFeedbackEngine; // PRD-0047
    std::unique_ptr<sonik::midi::SoftTakeoverManager> softTakeoverManager; // PRD-0045
    std::unique_ptr<DeckMidiHandler>                deckMidiHandler;    // PRD-0044
    std::unique_ptr<MixerMidiHandler>               mixerMidiHandler;   // PRD-0044
    std::unique_ptr<LibraryMidiHandler>             libraryMidiHandler; // PRD-0044
    std::unique_ptr<CompositeMidiCommandHandler>    compositeMidiHandler; // PRD-0044
    std::unique_ptr<sonik::midi::MidiInboundRouter> midiInboundRouter;  // PRD-0044

    // PRD-0040 diagnostic logger: prints device hot-plug events to the JUCE
    // log (Console.app on macOS) so manual testing can observe enumeration
    // and hot-plug without a dedicated UI. Coexists with the Settings → MIDI
    // panel (PRD-0048).
    struct MidiDiagnosticLogger final : public sonik::midi::DeviceListChangeListener
    {
        sonik::midi::MidiDeviceManager* manager { nullptr };
        void midiDeviceAdded   (std::uint64_t deviceId) override;
        void midiDeviceRemoved (std::uint64_t deviceId) override;
        void midiDeviceOpened  (std::uint64_t deviceId) override;
        void midiDeviceClosed  (std::uint64_t deviceId) override;
    };
    std::unique_ptr<MidiDiagnosticLogger> midiDiagnosticLogger;

    std::unique_ptr<DeckStateManager> deckStateManager;
    std::unique_ptr<MixerStateSchema>    mixerStateSchema;    // PRD-0052
    std::unique_ptr<MixerAtomicSnapshot> mixerAtomicSnapshot; // PRD-0052
    std::unique_ptr<MixerMeterSnapshot>  mixerMeterSnapshot;  // PRD-0052
    std::unique_ptr<MixerStateBridge>    mixerStateBridge;    // PRD-0052
    juce::ValueTree                      dawStateTree;        // PRD-0063
    std::unique_ptr<MasterClockPublisher> masterClockPublisher;  // PRD-0026
    std::unique_ptr<MasterClockManager>   masterClockManager;    // PRD-0026
    std::unique_ptr<AudioEngine>      audioEngine;
    std::unique_ptr<AudioFileLoader>  audioFileLoader;
    std::unique_ptr<Daw::MasterGridService> masterGridService;  // PRD-0064/0066
    std::unique_ptr<WaveformManager>  waveformManager;
    std::unique_ptr<BeatGridManager>  beatGridManager;
    std::unique_ptr<KeyDetectionManager> keyDetectionManager;
    std::unique_ptr<ModelManager>          modelManager;
    std::unique_ptr<StemSeparationManager> stemSeparationManager;
    std::unique_ptr<LibraryAnalysisService> libraryAnalysisService;
    std::unique_ptr<LibraryAnalysisQueue>   libraryAnalysisQueue;
    std::unique_ptr<WatchFolderScanner>    watchFolderScanner;
    std::unique_ptr<MainWindow>            mainWindow;
    std::unique_ptr<sonik::midi::MidiSettingsWindow> midiSettingsWindow;  // PRD-0048
    bool                                   quitSaveActive = false;

    // EPIC-0012 / PRD-0096: session lifecycle. The application owns the shared
    // ApplicationProperties (recents storage), the serializer, the controller
    // (constructed with the live `daw` branch, the DAW UndoManager, and the
    // PropertiesFile), and the File-menu command surface.
    std::unique_ptr<juce::ApplicationProperties>    appProperties;       // PRD-0096
    std::unique_ptr<Daw::Session::SessionSerializer> sessionSerializer;  // PRD-0095
    std::unique_ptr<Daw::Session::SessionController>  sessionController;  // PRD-0096
    std::unique_ptr<Daw::Session::Ui::SessionMenu>    sessionMenu;        // PRD-0096
    std::unique_ptr<juce::FileChooser>                sessionFileChooser; // active picker
    // PRD-0097: source-id resolution + missing-source relocation integration.
    std::unique_ptr<Daw::Session::SessionSourceResolution> sourceResolution;
    bool                                              quitSessionActive = false;

    // PRD-0098: external audio-file import. The registry mints/de-dupes ref-
    // counted source ids; the publisher atomically serves baked buffers to the
    // engine's resolver; the placer turns decoded sources into clips as one undo
    // transaction; the importer drives the background decode/resample/hash plus
    // the message-thread register/publish/analyse/place. Message thread only.
    std::unique_ptr<Daw::Import::ImportSourceRegistry>  importRegistry;
    std::unique_ptr<Daw::Import::ImportSourcePublisher> importPublisher;
    std::unique_ptr<Daw::Import::ImportClipPlacer>      importPlacer;
    std::unique_ptr<Daw::Import::AudioFileImporter>     audioFileImporter;
    std::unique_ptr<juce::FileChooser>                  importFileChooser;

    void buildSessionLifecycle();   // PRD-0096
    void updateSessionTitleUi (const juce::String& displayTitle, bool dirty);
    void proceedWithPreparationListQuit();  // PRD-0096: post-session-guard quit chain

    void openMidiSettingsWindow();  // PRD-0048

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonikApplication)
};
