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

    watchFolderScanner->startScan();
}

void SonikApplication::shutdown()
{
    if (libraryAnalysisQueue != nullptr)
        libraryAnalysisQueue->cancelAllJobs();

    if (deckStateManager != nullptr)
        deckStateManager->saveSession();

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
