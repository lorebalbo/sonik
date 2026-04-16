#include "WaveformManager.h"

WaveformManager::WaveformManager (DeckStateManager& deckState,
                                  TrackDatabase& database,
                                  AudioEngine& engine)
    : deckStateManager (deckState),
      audioEngine (engine),
      analyzer (database),
      rootState (deckState.getStateTree())
{
    rootState.addListener (this);
}

WaveformManager::~WaveformManager()
{
    rootState.removeListener (this);
}

WaveformData::Ptr WaveformManager::getWaveformData (const juce::String& deckId) const
{
    auto it = waveformDataMap.find (deckId);
    if (it != waveformDataMap.end())
        return it->second;
    return nullptr;
}

void WaveformManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                 const juce::Identifier& property)
{
    // Watch for loadingStatus changes on deck trees
    if (property == IDs::loadingStatus)
    {
        auto newStatus = tree.getProperty (IDs::loadingStatus).toString();
        if (newStatus != "idle")
            return;

        // Find the deck ID — the loadingStatus is on the Deck node itself
        if (! tree.hasType (IDs::Deck))
            return;

        auto deckId = tree.getProperty (IDs::id).toString();
        if (deckId.isEmpty())
            return;

        // Check if track is loaded (has contentHash)
        auto trackMeta = tree.getChildWithName (IDs::TrackMetadata);
        auto contentHash = trackMeta.getProperty (IDs::contentHash).toString();
        if (contentHash.isEmpty())
            return;

        // Check if waveform analysis is already done or in progress
        auto waveform = tree.getChildWithName (IDs::Waveform);
        auto analysisStatus = waveform.getProperty (IDs::analysisStatus).toString();
        if (analysisStatus == "done" || analysisStatus == "analyzing")
            return;

        triggerAnalysis (deckId, contentHash);
    }
}

void WaveformManager::triggerAnalysis (const juce::String& deckId,
                                       const juce::String& contentHash)
{
    // Get the audio buffer from the engine
    auto buffer = audioEngine.getDeckBuffer (deckId);
    if (buffer == nullptr)
        return;

    // Update state
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto waveform = deckTree.getChildWithName (IDs::Waveform);
    waveform.setProperty (IDs::analysisStatus,   "analyzing", nullptr);
    waveform.setProperty (IDs::analysisProgress, 0.0f,        nullptr);

    // Launch analysis
    analyzer.analyze (contentHash, std::move (buffer),
        [this, deckId] (const juce::String& /*hash*/, WaveformData::Ptr data)
        {
            // This callback runs on the message thread
            auto dt = deckStateManager.getDeckState (deckId);
            if (! dt.isValid())
                return;

            auto wf = dt.getChildWithName (IDs::Waveform);

            if (data != nullptr)
            {
                waveformDataMap[deckId] = data;
                wf.setProperty (IDs::analysisStatus,   "done", nullptr);
                wf.setProperty (IDs::analysisProgress, 1.0f,   nullptr);
            }
            else
            {
                wf.setProperty (IDs::analysisStatus,   "error", nullptr);
                wf.setProperty (IDs::analysisProgress, 0.0f,    nullptr);
            }
        });
}
