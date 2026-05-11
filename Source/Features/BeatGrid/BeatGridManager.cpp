#include "BeatGridManager.h"

BeatGridManager::BeatGridManager (DeckStateManager& deckState,
                                  TrackDatabase& database,
                                  AudioEngine& engine)
    : deckStateManager (deckState),
    db (database),
      audioEngine (engine),
      analyzer (database),
      rootState (deckState.getStateTree())
{
    rootState.addListener (this);
}

BeatGridManager::~BeatGridManager()
{
    rootState.removeListener (this);
}

BeatGridData::Ptr BeatGridManager::getBeatGridData (const juce::String& deckId) const
{
    auto it = beatGridDataMap.find (deckId);
    if (it != beatGridDataMap.end())
        return it->second;
    return nullptr;
}

void BeatGridManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                 const juce::Identifier& property)
{
    // Watch for loadingStatus changes on deck trees
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

        auto trackMeta = tree.getChildWithName (IDs::TrackMetadata);
        auto contentHash = trackMeta.getProperty (IDs::contentHash).toString();
        auto filePath    = trackMeta.getProperty (IDs::filePath).toString();
        if (contentHash.isEmpty())
            return;

        // Check if beat grid analysis is already done or in progress
        auto beatGrid = tree.getChildWithName (IDs::BeatGrid);
        auto analysisStatus = beatGrid.getProperty (IDs::analysisStatus).toString();
        if (analysisStatus == "done" || analysisStatus == "completed" || analysisStatus == "analyzing")
            return;

        triggerAnalysis (deckId, contentHash, filePath);
    }
}

void BeatGridManager::triggerAnalysis (const juce::String& deckId,
                                       const juce::String& contentHash,
                                       const juce::String& filePath)
{
    auto buffer = audioEngine.getDeckBuffer (deckId);
    if (buffer == nullptr)
        return;

    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto beatGrid = deckTree.getChildWithName (IDs::BeatGrid);
    beatGrid.setProperty (IDs::analysisStatus,   "analyzing", nullptr);
    beatGrid.setProperty (IDs::analysisProgress, 0.0f,        nullptr);

    analyzer.analyze (contentHash, filePath, std::move (buffer),
        [this, deckId, filePath, contentHash] (const juce::String& /*hash*/, BeatGridData::Ptr data)
        {
            auto dt = deckStateManager.getDeckState (deckId);
            if (! dt.isValid())
                return;

            auto bg = dt.getChildWithName (IDs::BeatGrid);

            if (data != nullptr)
            {
                beatGridDataMap[deckId] = data;

                if (data->bpm > 0.0)
                    db.updateLibraryTrackBpm (filePath, contentHash, data->bpm);

                bg.setProperty (IDs::bpm,                 data->bpm,                 nullptr);
                bg.setProperty (IDs::anchorSample,        static_cast<double> (data->anchorSample), nullptr);
                bg.setProperty (IDs::beatIntervalSamples, data->beatIntervalSamples, nullptr);
                bg.setProperty (IDs::confidence,          static_cast<double> (data->confidence),    nullptr);
                bg.setProperty (IDs::manuallyAdjusted,    data->manuallyAdjusted,    nullptr);

                bg.setProperty (IDs::analysisStatus,   "done", nullptr);
                bg.setProperty (IDs::analysisProgress, 1.0f,   nullptr);
            }
            else
            {
                bg.setProperty (IDs::analysisStatus,   "error", nullptr);
                bg.setProperty (IDs::analysisProgress, 0.0f,    nullptr);
            }
        });
}
