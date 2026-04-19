#include "StemSeparationManager.h"

// ============================================================================
// Construction / Destruction
// ============================================================================

StemSeparationManager::StemSeparationManager (DeckStateManager& deckState,
                                               TrackDatabase& database,
                                               ModelManager& modelMgr,
                                               AudioEngine& engine)
    : deckStateManager (deckState),
      trackDatabase (database),
      modelManager (modelMgr),
      audioEngine (engine),
      stemCache (database),
      rootState (deckState.getStateTree())
{
    // Startup cleanup: remove orphan/incomplete cache entries
    stemCache.cleanupOnStartup();

    // Listen for property changes on the state tree
    rootState.addListener (this);
}

StemSeparationManager::~StemSeparationManager()
{
    rootState.removeListener (this);

    // Signal all pending callAsync lambdas that we're gone
    alive->store (false, std::memory_order_release);

    // Cancel all active jobs
    threadPool.removeAllJobs (true, 5000);
    activeJobs.clear();
}

// ============================================================================
// Public API
// ============================================================================

void StemSeparationManager::startSeparation (const juce::String& deckId)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    // Check model readiness
    if (! modelManager.isModelReady())
    {
        auto stems = deckTree.getChildWithName (IDs::Stems);
        stems.setProperty (IDs::status, "model_unavailable", nullptr);
        return;
    }

    // Get track info
    auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
    auto contentHash = trackMeta.getProperty (IDs::contentHash).toString();

    if (contentHash.isEmpty())
        return;

    // Check track duration (reject < 5 seconds)
    double duration = static_cast<double> (trackMeta.getProperty (IDs::duration));
    if (duration < kMinTrackDurationSeconds)
    {
        auto stems = deckTree.getChildWithName (IDs::Stems);
        stems.setProperty (IDs::status, "none", nullptr);
        return;
    }

    // Cancel any existing separation for this deck
    cancelSeparation (deckId);

    // Get the decoded audio buffer
    auto buffer = audioEngine.getDeckBuffer (deckId);
    if (buffer == nullptr)
        return;

    // Check if cache already exists (shouldn't normally hit here, but guard)
    if (stemCache.hasCachedStems (contentHash))
    {
        loadCachedStemsAsync (deckId, contentHash);
        return;
    }

    // Determine if another deck's separation is in progress → queued
    auto stems = deckTree.getChildWithName (IDs::Stems);
    if (! activeJobs.empty())
        stems.setProperty (IDs::status, "queued", nullptr);
    else
        stems.setProperty (IDs::status, "separating", nullptr);

    stems.setProperty (IDs::progress,  0.0f, nullptr);
    stems.setProperty (IDs::stemError, "",   nullptr);

    double deviceRate = audioEngine.getSampleRate();

    // Create and enqueue the separation job
    auto aliveFlag = alive;
    auto* job = new StemSeparationEngine (
        deckId, contentHash, buffer,
        stemCache, stems, deviceRate,
        modelManager.getPythonPath(),
        modelManager.getScriptPath(),
        ModelManager::getModelDirectory(),
        [this, aliveFlag] (const juce::String& dk, StemData::Ptr result, const juce::String& error)
        {
            if (! aliveFlag->load (std::memory_order_acquire))
                return;
            onSeparationComplete (dk, result, error);
        });

    activeJobs[deckId] = job;
    threadPool.addJob (job, true); // pool takes ownership
}

void StemSeparationManager::cancelSeparation (const juce::String& deckId)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto it = activeJobs.find (deckId);
    if (it != activeJobs.end())
    {
        auto* job = it->second;
        threadPool.removeJob (job, true, 5000);
        activeJobs.erase (it);
    }

    // Reset stems state
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (deckTree.isValid())
    {
        auto stems = deckTree.getChildWithName (IDs::Stems);
        stems.setProperty (IDs::status,    "none", nullptr);
        stems.setProperty (IDs::progress,  0.0f,   nullptr);
        stems.setProperty (IDs::stemError, "",     nullptr);
    }
}

StemData::Ptr StemSeparationManager::getStemData (const juce::String& deckId) const
{
    auto it = stemDataMap.find (deckId);
    if (it != stemDataMap.end())
        return it->second;
    return nullptr;
}

bool StemSeparationManager::isModelReady() const
{
    return modelManager.isModelReady();
}

void StemSeparationManager::setStemReadyCallback (StemReadyCallback callback)
{
    stemReadyCallback = std::move (callback);
}

// ============================================================================
// ValueTree listener — watches for track load completion
// ============================================================================

void StemSeparationManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                        const juce::Identifier& property)
{
    // Watch for loadingStatus changes on deck trees
    if (property != IDs::loadingStatus)
        return;

    auto newStatus = tree.getProperty (IDs::loadingStatus).toString();
    if (newStatus != "idle")
        return;

    if (! tree.hasType (IDs::Deck))
        return;

    auto deckId = tree.getProperty (IDs::id).toString();
    if (deckId.isEmpty())
        return;

    // Track load complete — check cache automatically
    checkCacheForDeck (deckId);
}

// ============================================================================
// Internal helpers
// ============================================================================

void StemSeparationManager::checkCacheForDeck (const juce::String& deckId)
{
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
    auto contentHash = trackMeta.getProperty (IDs::contentHash).toString();

    if (contentHash.isEmpty())
        return;

    // Check track duration
    double duration = static_cast<double> (trackMeta.getProperty (IDs::duration));
    if (duration < kMinTrackDurationSeconds)
        return;

    // Check cache (fast DB query + file existence)
    if (stemCache.hasCachedStems (contentHash))
    {
        loadCachedStemsAsync (deckId, contentHash);
    }
    // else: stems remain at "none", user must trigger manually
}

void StemSeparationManager::loadCachedStemsAsync (const juce::String& deckId,
                                                     const juce::String& contentHash)
{
    // Set status to loading_cached
    auto deckTree = deckStateManager.getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto stems = deckTree.getChildWithName (IDs::Stems);
    stems.setProperty (IDs::status, "loading_cached", nullptr);

    double deviceRate = audioEngine.getSampleRate();

    // Load on the thread pool (file I/O, not message thread)
    // We create a simple lambda-wrapping ThreadPoolJob for this.
    struct CacheLoadJob : public juce::ThreadPoolJob
    {
        CacheLoadJob (StemSeparationManager& mgr_,
                       const juce::String& deckId_,
                       const juce::String& contentHash_,
                       double deviceRate_)
            : juce::ThreadPoolJob ("CacheLoad_" + deckId_),
              mgr (mgr_), dk (deckId_), hash (contentHash_), rate (deviceRate_)
        {}

        JobStatus runJob() override
        {
            auto result = mgr.stemCache.loadCachedStems (hash, rate);
            auto deckId = dk;
            auto aliveFlag = mgr.alive;
            auto* manager = &mgr;
            auto stems = result;

            juce::MessageManager::callAsync ([aliveFlag, manager, deckId, stems]()
            {
                if (! aliveFlag->load (std::memory_order_acquire))
                    return; // Manager destroyed — bail out

                if (stems != nullptr)
                {
                    manager->stemDataMap[deckId] = stems;

                    auto dt = manager->deckStateManager.getDeckState (deckId);
                    if (dt.isValid())
                    {
                        auto stemsNode = dt.getChildWithName (IDs::Stems);
                        stemsNode.setProperty (IDs::status,   "ready", nullptr);
                        stemsNode.setProperty (IDs::progress, 1.0f,    nullptr);
                    }

                    if (manager->stemReadyCallback)
                        manager->stemReadyCallback (deckId, stems);
                }
                else
                {
                    auto dt = manager->deckStateManager.getDeckState (deckId);
                    if (dt.isValid())
                    {
                        auto stemsNode = dt.getChildWithName (IDs::Stems);
                        stemsNode.setProperty (IDs::status, "none", nullptr);
                    }
                }
            });

            return jobHasFinished;
        }

        StemSeparationManager& mgr;
        juce::String dk, hash;
        double rate;
    };

    threadPool.addJob (new CacheLoadJob (*this, deckId, contentHash, deviceRate), true);
}

void StemSeparationManager::onSeparationComplete (const juce::String& deckId,
                                                     StemData::Ptr result,
                                                     const juce::String& error)
{
    // This runs on the message thread (dispatched from StemSeparationEngine)
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    activeJobs.erase (deckId);

    if (result != nullptr && error.isEmpty())
    {
        stemDataMap[deckId] = result;

        auto deckTree = deckStateManager.getDeckState (deckId);
        if (deckTree.isValid())
        {
            auto stems = deckTree.getChildWithName (IDs::Stems);
            stems.setProperty (IDs::status,   "ready", nullptr);
            stems.setProperty (IDs::progress, 1.0f,    nullptr);
        }

        if (stemReadyCallback)
            stemReadyCallback (deckId, result);

        // Run cache eviction after successful separation
        stemCache.evictIfNeeded (getActiveContentHashes());
    }
    // Error case is handled by StemSeparationEngine::reportError
}

std::set<juce::String> StemSeparationManager::getActiveContentHashes() const
{
    std::set<juce::String> hashes;
    auto deckIds = deckStateManager.getDeckIds();

    for (const auto& dk : deckIds)
    {
        auto deckTree = deckStateManager.getDeckState (dk);
        if (deckTree.isValid())
        {
            auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
            auto hash = trackMeta.getProperty (IDs::contentHash).toString();
            if (hash.isNotEmpty())
                hashes.insert (hash);
        }
    }

    return hashes;
}
