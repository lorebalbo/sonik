#pragma once

#include "StemData.h"
#include "StemCache.h"
#include "StemSeparationEngine.h"
#include "ModelManager.h"
#include "../Deck/DeckStateManager.h"
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include <juce_data_structures/juce_data_structures.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <map>
#include <functional>
#include <memory>

/// Orchestrates stem separation for all decks.
///
/// Responsibilities:
///  - Listens for track load events (ValueTree listener on loadingStatus)
///  - Checks cache automatically on track load
///  - Exposes startSeparation/cancelSeparation for manual control
///  - Owns a dedicated ThreadPool(1) for separation jobs
///  - Delivers stem buffers via ValueTree + optional callback
///  - Runs cache eviction after successful separations
///  - Runs startup cleanup on construction
///
/// Constructed in SonikApplication with explicit DI.
class StemSeparationManager final : private juce::ValueTree::Listener
{
public:
    using StemReadyCallback = std::function<void (const juce::String& deckId,
                                                   StemData::Ptr stems)>;

    /// @param deckState     Reference to the deck state manager.
    /// @param database      Reference to the track database.
    /// @param modelMgr      Reference to the model manager.
    /// @param engine        Reference to the audio engine (for buffer access).
    StemSeparationManager (DeckStateManager& deckState,
                            TrackDatabase& database,
                            ModelManager& modelMgr,
                            AudioEngine& engine);
    ~StemSeparationManager() override;

    StemSeparationManager (const StemSeparationManager&) = delete;
    StemSeparationManager& operator= (const StemSeparationManager&) = delete;

    /// Manually trigger stem separation for a deck.
    /// Requires a loaded track and a ready model.
    void startSeparation (const juce::String& deckId);

    void startSeparationForFile (const juce::String& filePath,
                                 const juce::String& contentHash,
                                 std::function<void(bool success)> completion);

    void startSeparationForFile (const juce::String& filePath,
                                 const juce::String& contentHash,
                                 std::shared_ptr<std::atomic<bool>> cancelFlag,
                                 std::function<void(bool success)> completion);

    /// Cancel an in-progress separation for a deck.
    void cancelSeparation (const juce::String& deckId);

    /// Get the stem data for a deck (nullptr if not separated).
    StemData::Ptr getStemData (const juce::String& deckId) const;

    /// Returns true if the model is loaded and ready for inference.
    bool isModelReady() const;

    /// Register a callback for when stems become ready.
    void setStemReadyCallback (StemReadyCallback callback);

private:
    class FileSeparationJob;

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    // Internal helpers
    void checkCacheForDeck (const juce::String& deckId);
    void loadCachedStemsAsync (const juce::String& deckId, const juce::String& contentHash);
    void onSeparationComplete (const juce::String& deckId,
                                StemData::Ptr result,
                                const juce::String& error);
    std::set<juce::String> getActiveContentHashes() const;

    DeckStateManager&  deckStateManager;
    TrackDatabase&     trackDatabase;
    ModelManager&      modelManager;
    AudioEngine&       audioEngine;
    StemCache          stemCache;

    juce::ValueTree    rootState;
    juce::ThreadPool   threadPool { 1 };

    // Active jobs and results per deck
    std::map<juce::String, StemSeparationEngine*> activeJobs;
    std::map<juce::String, StemData::Ptr>         stemDataMap;

    StemReadyCallback  stemReadyCallback;

    // Alive flag for safe callAsync lambdas — prevents use-after-free
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    static constexpr double kMinTrackDurationSeconds = 5.0;
};
