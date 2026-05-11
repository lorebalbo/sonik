#include "SonikApplication.h"

void SonikApplication::initialise (const juce::String& /*commandLine*/)
{
    auto dbPath = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Application Support")
                      .getChildFile ("Sonik")
                      .getChildFile ("sonik.db");

    trackDatabase    = std::make_unique<TrackDatabase> (dbPath);
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

    mainWindow = std::make_unique<MainWindow> (
        *audioFileLoader, *deckStateManager, *audioEngine, *waveformManager,
        *beatGridManager, *stemSeparationManager, *masterClockManager,
        *trackDatabase);

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
