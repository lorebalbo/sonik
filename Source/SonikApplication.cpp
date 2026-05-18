#include "SonikApplication.h"

namespace
{
struct AsyncCompletion final
{
    juce::WaitableEvent event;
    std::atomic<bool> finished { false };
    std::atomic<bool> success  { false };
};

bool waitForAsyncCompletion (const LibraryAnalysisQueue::JobContext& ctx,
                             const std::shared_ptr<AsyncCompletion>& completion)
{
    while (! completion->finished.load (std::memory_order_acquire))
    {
        if (ctx.shouldExit && ctx.shouldExit())
        {
            if (ctx.sharedCancel != nullptr)
                ctx.sharedCancel->store (true, std::memory_order_release);
            return false;
        }

        completion->event.wait (100);
    }

    return completion->success.load (std::memory_order_acquire);
}

LibraryAnalysisQueue::JobExecutor makeAnalysisExecutor (LibraryAnalysisService& analysisService)
{
    return [&analysisService] (const LibraryAnalysisQueue::JobContext& ctx)
    {
        auto completion = std::make_shared<AsyncCompletion>();
        auto progressFn = ctx.progress;

        analysisService.analyzeTrack (ctx.filePath, ctx.contentHash,
            [completion] (const juce::String&, bool succeeded)
            {
                completion->success.store (succeeded, std::memory_order_release);
                completion->finished.store (true, std::memory_order_release);
                completion->event.signal();
            },
            ctx.sharedCancel,
            [progressFn = std::move (progressFn)] (int percent) mutable
            {
                if (progressFn)
                    progressFn (percent);
            });

        return waitForAsyncCompletion (ctx, completion);
    };
}

LibraryAnalysisQueue::JobExecutor makeStemExecutor (StemSeparationManager& stemManager)
{
    return [&stemManager] (const LibraryAnalysisQueue::JobContext& ctx)
    {
        auto completion = std::make_shared<AsyncCompletion>();

        stemManager.startSeparationForFile (ctx.filePath, ctx.contentHash, ctx.sharedCancel,
            [completion] (bool success)
            {
                completion->success.store (success, std::memory_order_release);
                completion->finished.store (true, std::memory_order_release);
                completion->event.signal();
            });

        return waitForAsyncCompletion (ctx, completion);
    };
}
} // namespace

void SonikApplication::initialise (const juce::String& /*commandLine*/)
{
    auto dbPath = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Application Support")
                      .getChildFile ("Sonik")
                      .getChildFile ("sonik.db");

    trackDatabase    = std::make_unique<TrackDatabase> (dbPath);

    // MIDI I/O subsystem (PRD-0040) — constructed before any feature that
    // may later subscribe to MIDI events. Enumeration + hot-plug only;
    // no devices are opened automatically at this stage.
    midiHost          = std::make_unique<sonik::midi::JuceMidiHost>();
    midiDeviceManager = std::make_unique<sonik::midi::MidiDeviceManager> (*midiHost);
    midiDeviceManager->initialise();

    // Attach diagnostic logger so manual hot-plug testing is observable.
    midiDiagnosticLogger = std::make_unique<MidiDiagnosticLogger>();
    midiDiagnosticLogger->manager = midiDeviceManager.get();
    midiDeviceManager->addDeviceListChangeListener (midiDiagnosticLogger.get());
    {
        const auto initial = midiDeviceManager->getDevices();
        DBG ("[MIDI] Startup enumeration: " << (int) initial.size() << " device(s)");
        for (const auto& d : initial)
            DBG ("[MIDI]   id=" << juce::String::toHexString ((juce::int64) d.deviceId)
                 << "  " << (d.isInput ? "IN " : "OUT")
                 << "  mfg='"   << d.manufacturer
                 << "' name='" << d.productName
                 << "' ordinal=" << d.ordinal
                 << " connected=" << (d.isConnected ? 1 : 0));
    }

    deckStateManager = std::make_unique<DeckStateManager> (*trackDatabase);

    deckStateManager->restoreSession();

    // Ensure at least 2 decks exist after restore
    while (deckStateManager->getDeckCount() < 2)
        deckStateManager->addDeck();

    // Create the master clock publisher and manager (PRD-0026).
    // Manager must be created after decks exist so its initial listener state is correct.
    masterClockPublisher = std::make_unique<MasterClockPublisher>();
    masterClockManager   = std::make_unique<MasterClockManager> (
        deckStateManager->getStateTree(), *masterClockPublisher);

    // Create and start the audio engine
    audioEngine = std::make_unique<AudioEngine> (deckStateManager->getStateTree());

    // PRD-0041: RT-safe MIDI message bridge. Constructed after the device
    // manager (producers exist) and before the audio engine begins
    // processing (the audio thread will drain the FIFO each callback once
    // PRD-0044 wires producers to it).
    midiMessageBridge = std::make_unique<sonik::midi::MidiMessageBridge>();
    audioEngine->setMidiMessageBridge (midiMessageBridge.get());

    // PRD-0043: Mapping storage. Loads bundled profiles synchronously and
    // enumerates user profiles from
    // ~/Library/Application Support/Sonik/MidiMappings on a worker thread.
    // Constructor creates the user directory if it does not already exist.
    mappingStore = std::make_unique<sonik::midi::MappingStore> (*midiDeviceManager);

    // PRD-0045: Soft-takeover (pickup mode) tracker. Listens to the root
    // state tree for non-MIDI parameter changes (track load, mouse moves) so
    // the next hardware sample must cross the new value before it engages.
    softTakeoverManager = std::make_unique<sonik::midi::SoftTakeoverManager> (
        deckStateManager->getStateTree(), *mappingStore);

    // PRD-0044: Inbound MIDI command dispatch. The router subscribes to
    // MidiDeviceManager + MappingStore and forwards to the composite handler
    // on the Message thread (audio-thread paths go via the bridge FIFO).
    deckMidiHandler      = std::make_unique<DeckMidiHandler> (*deckStateManager, *softTakeoverManager);
    mixerMidiHandler     = std::make_unique<MixerMidiHandler>();
    libraryMidiHandler   = std::make_unique<LibraryMidiHandler>();
    compositeMidiHandler = std::make_unique<CompositeMidiCommandHandler> (
        *deckMidiHandler, *mixerMidiHandler, *libraryMidiHandler);
    midiInboundRouter = std::make_unique<sonik::midi::MidiInboundRouter> (
        *midiDeviceManager, *midiMessageBridge, *mappingStore, *compositeMidiHandler);

    // TODO: per-device opt-in auto-open policy belongs to a follow-up PRD.
    // Until then, open every enumerated endpoint so inbound events flow and
    // outbound LED feedback can reach hardware.
    for (const auto& dev : midiDeviceManager->getDevices())
    {
        if (dev.isInput)
            midiDeviceManager->openInput (dev.deviceId);
        else
            midiDeviceManager->openOutput (dev.deviceId);
    }

    // PRD-0047: MIDI output and LED feedback engine.
    midiFeedbackEngine = std::make_unique<sonik::midi::MidiFeedbackEngine> (
        deckStateManager->getStateTree(),
        *midiDeviceManager,
        *mappingStore,
        *softTakeoverManager);

    // Inject the master clock publisher into every deck slot (PRD-0026).
    audioEngine->setMasterClockPublisher (masterClockPublisher.get());

    auto decksNode = deckStateManager->getStateTree().getChildWithName (IDs::Decks);
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto deckTree = decksNode.getChild (i);
        auto deckId   = deckTree.getProperty (IDs::id).toString();
        auto* state   = deckStateManager->getAudioState (deckId);

        if (state != nullptr)
            audioEngine->registerDeck (deckId, state);
    }

    audioEngine->start();

    // Create the file loader (uses engine's sample rate)
    audioFileLoader = std::make_unique<AudioFileLoader> (
        *deckStateManager, *audioEngine, audioEngine->getSampleRate());

    // Create the waveform manager (PRD-0006)
    waveformManager = std::make_unique<WaveformManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the beat grid manager (PRD-0008)
    beatGridManager = std::make_unique<BeatGridManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the key detection manager (PRD-0009)
    keyDetectionManager = std::make_unique<KeyDetectionManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the model manager for stem separation (PRD-0019)
    // Validates model file and discovers Python on a background thread.
    modelManager = std::make_unique<ModelManager> (
        deckStateManager->getStateTree());

    // Create the stem separation manager (PRD-0020)
    stemSeparationManager = std::make_unique<StemSeparationManager> (
        *deckStateManager, *trackDatabase,
        *modelManager, *audioEngine);

    // Wire stem-ready callback to deliver stems to audio engine (PRD-0021)
    stemSeparationManager->setStemReadyCallback (
        [this] (const juce::String& deckId, StemData::Ptr stems)
        {
            if (audioEngine != nullptr && stems != nullptr)
            {
                audioEngine->setDeckStemBuffers (
                    deckId,
                    stems->stems[StemData::Vocals],
                    stems->stems[StemData::Drums],
                    stems->stems[StemData::Bass],
                    stems->stems[StemData::Other]);
            }
        });

    libraryAnalysisService = std::make_unique<LibraryAnalysisService> (*trackDatabase);
    libraryAnalysisQueue = std::make_unique<LibraryAnalysisQueue> (
        makeAnalysisExecutor (*libraryAnalysisService),
        makeStemExecutor (*stemSeparationManager));

    mainWindow = std::make_unique<MainWindow> (
        *audioFileLoader, *deckStateManager, *audioEngine, *waveformManager,
        *beatGridManager, *stemSeparationManager, *masterClockManager,
        *libraryAnalysisQueue, *trackDatabase);

    // Create the watch-folder scanner (PRD-0031) and kick off the startup scan.
    // Constructed after trackDatabase is ready; destroyed in shutdown() before
    // trackDatabase is reset. Created AFTER mainWindow so the LibraryComponent
    // exists and can register as a listener.
    watchFolderScanner = std::make_unique<WatchFolderScanner> (*trackDatabase);

    // Wire the scanner to the LibraryComponent listener interface.
    if (auto* content = mainWindow->getContent())
        content->registerScannerWithLibrary (*watchFolderScanner);

    // PRD-0048: wire toolbar MIDI button.
    if (auto* content = mainWindow->getContent())
        content->setOnMidiClicked ([this]() { openMidiSettingsWindow(); });

    watchFolderScanner->startScan();
}

void SonikApplication::shutdown()
{
    if (libraryAnalysisQueue != nullptr)
        libraryAnalysisQueue->cancelAllJobs();

    if (deckStateManager != nullptr)
        deckStateManager->saveSession();

    midiSettingsWindow.reset();  // PRD-0048
    mainWindow.reset();

    // Stop the watch-folder scanner before the database is torn down.
    if (watchFolderScanner != nullptr)
        watchFolderScanner->cancelScan();
    watchFolderScanner.reset();

    // Stop file loader before engine
    audioFileLoader.reset();

    // Stop waveform manager before engine
    waveformManager.reset();

    // Stop beat grid manager before engine
    beatGridManager.reset();

    libraryAnalysisQueue.reset();
    libraryAnalysisService.reset();

    // Stop stem separation manager before model manager
    stemSeparationManager.reset();

    // Stop model manager before engine
    modelManager.reset();

    // Stop audio engine BEFORE destroying DeckStateManager
    audioEngine.reset();

    // Master clock manager must be destroyed before deckStateManager
    // (it holds a ValueTree reference and listens to rootState).
    masterClockManager.reset();
    masterClockPublisher.reset();

    deckStateManager.reset();

    // Tear down MIDI before the database (PRD-0040). Destruction stops the
    // hot-plug timer and closes every open MIDI input/output on the Message
    // thread.
    if (midiDeviceManager != nullptr && midiDiagnosticLogger != nullptr)
        midiDeviceManager->removeDeviceListChangeListener (midiDiagnosticLogger.get());
    midiDiagnosticLogger.reset();
    midiInboundRouter.reset();   // PRD-0044 (unsubscribes from device manager, store, bridge)
    compositeMidiHandler.reset();
    libraryMidiHandler.reset();
    mixerMidiHandler.reset();
    deckMidiHandler.reset();
    midiFeedbackEngine.reset(); // PRD-0047 (depends on mappingStore + device manager + state tree)
    softTakeoverManager.reset(); // PRD-0045 (depends on mappingStore + deckStateManager)
    mappingStore.reset();        // PRD-0043
    midiMessageBridge.reset();   // PRD-0041
    midiDeviceManager.reset();
    midiHost.reset();

    trackDatabase.reset();
}

void SonikApplication::systemRequestedQuit()
{
    if (quitSaveActive)
        return;

    if (mainWindow != nullptr)
    {
        if (auto* content = mainWindow->getContent())
        {
            quitSaveActive = true;
            content->savePreparationListBeforeQuit (
                [this] (bool shouldQuit)
                {
                    quitSaveActive = false;
                    if (shouldQuit)
                        quit();
                });
            return;
        }
    }

    quit();
}

//==============================================================================
// PRD-0040 diagnostic logger implementation
namespace
{
    static juce::String midiDeviceDescription (sonik::midi::MidiDeviceManager& mgr, std::uint64_t id)
    {
        for (const auto& d : mgr.getDevices())
            if (d.deviceId == id)
                return juce::String (d.isInput ? "IN  " : "OUT ")
                       + "'" + d.productName + "' (ordinal=" + juce::String (d.ordinal) + ")";
        return juce::String ("<unknown>");
    }
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceAdded (std::uint64_t deviceId)
{
    if (manager != nullptr)
        DBG ("[MIDI] +ADDED   id=" << juce::String::toHexString ((juce::int64) deviceId)
             << "  " << midiDeviceDescription (*manager, deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceRemoved (std::uint64_t deviceId)
{
    if (manager != nullptr)
        DBG ("[MIDI] -REMOVED id=" << juce::String::toHexString ((juce::int64) deviceId)
             << "  " << midiDeviceDescription (*manager, deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceOpened (std::uint64_t deviceId)
{
    DBG ("[MIDI]  OPENED  id=" << juce::String::toHexString ((juce::int64) deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceClosed (std::uint64_t deviceId)
{
    DBG ("[MIDI]  CLOSED  id=" << juce::String::toHexString ((juce::int64) deviceId));
}

//==============================================================================
// PRD-0048: MIDI Settings window lifecycle.
void SonikApplication::openMidiSettingsWindow()
{
    if (midiSettingsWindow != nullptr)
    {
        midiSettingsWindow->toFront (true);
        return;
    }

    if (mappingStore == nullptr || midiDeviceManager == nullptr
        || midiInboundRouter == nullptr || softTakeoverManager == nullptr)
        return;

    midiSettingsWindow = std::make_unique<sonik::midi::MidiSettingsWindow> (
        *mappingStore, *midiDeviceManager, *midiInboundRouter, *softTakeoverManager);

    midiSettingsWindow->onClose = [this]()
    {
        midiSettingsWindow.reset();
    };
}

