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

    // Create and start the audio engine
    audioEngine = std::make_unique<AudioEngine> (deckStateManager->getStateTree());

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

    mainWindow = std::make_unique<MainWindow> (
        *audioFileLoader, *deckStateManager, *audioEngine, *waveformManager, *beatGridManager);
}

void SonikApplication::shutdown()
{
    if (deckStateManager != nullptr)
        deckStateManager->saveSession();

    mainWindow.reset();

    // Stop file loader before engine
    audioFileLoader.reset();

    // Stop waveform manager before engine
    waveformManager.reset();

    // Stop beat grid manager before engine
    beatGridManager.reset();

    // Stop audio engine BEFORE destroying DeckStateManager
    audioEngine.reset();

    deckStateManager.reset();
    trackDatabase.reset();
}

void SonikApplication::systemRequestedQuit()
{
    quit();
}
