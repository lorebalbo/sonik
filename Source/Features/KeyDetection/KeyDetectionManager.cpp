#include "KeyDetectionManager.h"

KeyDetectionManager::KeyDetectionManager (DeckStateManager& deckState,
                                           TrackDatabase& database,
                                           AudioEngine& engine)
    : deckStateManager (deckState),
      audioEngine (engine),
      analyzer (database),
      rootState (deckState.getStateTree())
{
    rootState.addListener (this);
}

KeyDetectionManager::~KeyDetectionManager()
{
    rootState.removeListener (this);
}

void KeyDetectionManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                     const juce::Identifier& property)
{
    if (property == IDs::loadingStatus)
    {
        auto newStatus = tree.getProperty (IDs::loadingStatus).toString();
        if (newStatus != "idle")
            return;

        if (! tree.hasType (IDs::Deck))
            return;

        auto deckId = tree.getProperty (IDs::id).toString();
        if (deckId.isEmpty())
            return;

        auto trackMeta   = tree.getChildWithName (IDs::TrackMetadata);
        auto contentHash = trackMeta.getProperty (IDs::contentHash).toString();
        auto filePath    = trackMeta.getProperty (IDs::filePath).toString();
        if (contentHash.isEmpty())
            return;

        // Check if key analysis is already done or in progress
        auto keyInfo = tree.getChildWithName (IDs::KeyInfo);
        auto analysisStatus = keyInfo.getProperty (IDs::analysisStatus).toString();
        if (analysisStatus == "done" || analysisStatus == "analyzing")
            return;

        triggerAnalysis (deckId, contentHash, filePath);
    }
}

void KeyDetectionManager::triggerAnalysis (const juce::String& deckId,
                                            const juce::String& contentHash,
                                            const juce::String& filePath)
{
    auto buffer = audioEngine.getDeckBuffer (deckId);
    if (buffer == nullptr)
        return;

    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto keyInfo = deckTree.getChildWithName (IDs::KeyInfo);
    keyInfo.setProperty (IDs::analysisStatus,   "analyzing", nullptr);
    keyInfo.setProperty (IDs::analysisProgress, 0.0f,        nullptr);

    analyzer.analyze (contentHash, filePath, std::move (buffer),
        [this, deckId] (const juce::String& /*hash*/, int keyIndex, float confidence)
        {
            auto dt = deckStateManager.getDeckState (deckId);
            if (! dt.isValid())
                return;

            auto ki = dt.getChildWithName (IDs::KeyInfo);

            ki.setProperty (IDs::keyIndex,    keyIndex,   nullptr);
            ki.setProperty (IDs::confidence,  static_cast<double> (confidence), nullptr);

            ki.setProperty (IDs::analysisStatus,   "done", nullptr);
            ki.setProperty (IDs::analysisProgress, 1.0f,   nullptr);
        });
}
