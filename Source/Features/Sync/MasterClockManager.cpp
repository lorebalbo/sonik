#include "MasterClockManager.h"
#include <cmath>

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

MasterClockManager::MasterClockManager (juce::ValueTree rootState, MasterClockPublisher& publisher)
    : rootState_ (rootState), publisher_ (publisher)
{
    rootState_.addListener (this);

    // Publish an initial dormant snapshot so the publisher always holds valid data.
    publishDormant();
}

MasterClockManager::~MasterClockManager()
{
    rootState_.removeListener (this);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MasterClockManager::setMaster (int deckIndex)
{
    auto deckTree = getDeckTreeAt (deckIndex);
    if (! deckTree.isValid())
        return;

    const int currentMaster = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));

    // No-op: already the master AND currently playing.
    if (currentMaster == deckIndex)
    {
        if (getPlaybackStatus (deckTree) == "playing")
            return;

        // Already master but not playing: republish current state (no structural change needed).
        publishFromDeck (deckIndex);
        return;
    }

    // Demote current master.
    if (currentMaster >= 0)
    {
        auto currentTree = getDeckTreeAt (currentMaster);
        if (currentTree.isValid())
            currentTree.setProperty (IDs::isMaster, false, nullptr);
    }

    // SYNC+MASTER interaction: clear isSynced on the new master before promoting it.
    deckTree.setProperty (IDs::isSynced, false, nullptr);

    // Promote the new master.
    rootState_.setProperty (IDs::masterDeckIndex, deckIndex, nullptr);
    deckTree.setProperty (IDs::isMaster, true, nullptr);

    publishFromDeck (deckIndex);
}

int MasterClockManager::getMasterDeckIndex() const
{
    return static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));
}

// ---------------------------------------------------------------------------
// PRD-0092: Automation tempo override
// ---------------------------------------------------------------------------

void MasterClockManager::setAutomationTempoOverride (double bpm)
{
    automationTempoOverride_ = bpm;
    republishCurrent();
}

void MasterClockManager::clearAutomationTempoOverride()
{
    if (! automationTempoOverride_.has_value())
        return;

    automationTempoOverride_.reset();
    republishCurrent();
}

void MasterClockManager::republishCurrent()
{
    const int currentMaster = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));
    if (currentMaster >= 0 && getDeckTreeAt (currentMaster).isValid())
        publishFromDeck (currentMaster);
    else
        publishDormant();
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void MasterClockManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                    const juce::Identifier& property)
{
    // --- Deck-level property changes ---
    if (tree.getType() == IDs::Deck)
    {
        if (property == IDs::playbackStatus)
        {
            const int deckIndex    = getDeckIndex (tree);
            if (deckIndex < 0) return;

            const juce::String status = getPlaybackStatus (tree);
            const int currentMaster   = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));

            if (currentMaster == deckIndex)
            {
                // ---- Master deck changed status ----

                if (status == "playing")
                {
                    // Resume from paused (or initial play): publish latest master clock state.
                    publishFromDeck (deckIndex);
                }
                else if (status == "paused")
                {
                    // Paused: publish latest BPM/anchor with isPlaying=false.
                    publishFromDeck (deckIndex);
                }
                else if (status == "stopped" || status == "empty")
                {
                    // Master stopped or ejected: demote it, find a replacement.
                    tree.setProperty (IDs::isMaster, false, nullptr);
                    rootState_.setProperty (IDs::masterDeckIndex, -1, nullptr);

                    const int nextMaster = findLowestPlayingDeckIndex();
                    if (nextMaster >= 0)
                        promoteDeck (nextMaster);
                    else
                        publishDormant();
                }
            }
            else if (currentMaster < 0)
            {
                // ---- No master assigned: auto-promote on first activity ----
                if (status == "stopped" || status == "playing")
                    promoteDeck (deckIndex);
            }
            // If currentMaster is some other deck, changes to this deck are ignored here.
        }
        else if (property == IDs::speedMultiplier)
        {
            // Pitch-fader changes on the master must immediately update master clock BPM.
            const int deckIndex = getDeckIndex (tree);
            const int currentMaster = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));
            if (deckIndex >= 0 && deckIndex == currentMaster)
                publishFromDeck (deckIndex);
        }
        // Other Deck-level properties (isMaster, isSynced, gain, etc.) are intentionally ignored
        // to avoid feedback loops from our own mutations.
    }

    // --- BeatGrid sub-tree changes on the master deck ---
    else if (tree.getType() == IDs::BeatGrid)
    {
        if (property == IDs::bpm || property == IDs::anchorSample)
        {
            const int currentMaster = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));
            if (currentMaster >= 0)
            {
                auto masterDeckTree = getDeckTreeAt (currentMaster);
                if (masterDeckTree.isValid() &&
                    masterDeckTree.getChildWithName (IDs::BeatGrid) == tree)
                {
                    publishFromDeck (currentMaster);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

juce::ValueTree MasterClockManager::getDeckTreeAt (int index) const
{
    auto decks = rootState_.getChildWithName (IDs::Decks);
    if (index < 0 || index >= decks.getNumChildren())
        return {};
    return decks.getChild (index);
}

int MasterClockManager::getDeckIndex (const juce::ValueTree& deckTree) const
{
    auto decks = rootState_.getChildWithName (IDs::Decks);
    for (int i = 0; i < decks.getNumChildren(); ++i)
        if (decks.getChild (i) == deckTree)
            return i;
    return -1;
}

void MasterClockManager::promoteDeck (int deckIndex)
{
    auto deckTree = getDeckTreeAt (deckIndex);
    if (! deckTree.isValid())
        return;

    // Master deck must not stay in SYNC mode after auto-promotion.
    deckTree.setProperty   (IDs::isSynced, false, nullptr);
    rootState_.setProperty (IDs::masterDeckIndex, deckIndex, nullptr);
    deckTree.setProperty   (IDs::isMaster, true, nullptr);
    publishFromDeck (deckIndex);
}

void MasterClockManager::demoteCurrentMaster()
{
    const int current = static_cast<int> (rootState_.getProperty (IDs::masterDeckIndex, -1));
    if (current < 0) return;

    auto deckTree = getDeckTreeAt (current);
    if (deckTree.isValid())
        deckTree.setProperty (IDs::isMaster, false, nullptr);

    rootState_.setProperty (IDs::masterDeckIndex, -1, nullptr);
}

void MasterClockManager::publishFromDeck (int deckIndex)
{
    auto deckTree = getDeckTreeAt (deckIndex);
    if (! deckTree.isValid())
        return;

    auto beatGrid = deckTree.getChildWithName (IDs::BeatGrid);
    const double  bpm    = getDeckEffectiveBpm (deckTree);
    const double  nativeBpm = getDeckNativeBpm (deckTree);
    const int64_t anchor = beatGrid.isValid()
        ? static_cast<int64_t> (beatGrid.getProperty (IDs::anchorSample, static_cast<int64_t> (0)))
        : 0;

    lastMasterBPM_ = bpm;
    lastMasterNativeBPM_ = nativeBpm;

    MasterClockSnapshot snap;
    // PRD-0092: a single tempo authority. When automation drives the tempo, the
    // published BPM is the automated override; otherwise it is the derived deck
    // BPM. lastMasterBPM_ retains the DERIVED value so clearing the override
    // restores the deck-driven tempo cleanly.
    snap.masterBPM               = automationTempoOverride_.value_or (bpm);
    snap.masterNativeBPM         = nativeBpm;
    snap.masterPhaseOriginSample  = anchor;
    snap.masterIsPlaying          = (getPlaybackStatus (deckTree) == "playing");
    publisher_.publish (snap);

    // Expose the master slot index so AudioEngine can write the master's playhead
    // each audio block for use by PhaseLockEngine (phase formula fix).
    const auto deckId = deckTree.getProperty (IDs::id, "").toString();
    publisher_.masterSlotIndex.store (deckIdToSlot (deckId), std::memory_order_relaxed);
}

void MasterClockManager::publishDormant()
{
    // Keep last BPM, but signal that nobody is playing and reset phase origin.
    MasterClockSnapshot snap;
    // PRD-0092: an engaged automation override still governs the published BPM
    // even while dormant, so the grid follows the recorded tempo curve.
    snap.masterBPM               = automationTempoOverride_.value_or (lastMasterBPM_);
    snap.masterNativeBPM         = lastMasterNativeBPM_;
    snap.masterPhaseOriginSample  = 0;
    snap.masterIsPlaying          = false;
    publisher_.publish (snap);
    publisher_.masterSlotIndex.store (-1, std::memory_order_relaxed);
}

juce::String MasterClockManager::getPlaybackStatus (const juce::ValueTree& deckTree)
{
    return deckTree.getProperty (IDs::playbackStatus, "empty").toString();
}

double MasterClockManager::getDeckEffectiveBpm (const juce::ValueTree& deckTree)
{
    const double baseBpm = getDeckNativeBpm (deckTree);

    if (baseBpm <= 0.0 || ! std::isfinite (baseBpm))
        return 0.0;

    const double speedMul = static_cast<double> (deckTree.getProperty (IDs::speedMultiplier, 1.0f));
    if (! std::isfinite (speedMul) || speedMul <= 0.0)
        return baseBpm;

    return baseBpm * speedMul;
}

double MasterClockManager::getDeckNativeBpm (const juce::ValueTree& deckTree)
{
    const auto beatGrid = deckTree.getChildWithName (IDs::BeatGrid);
    const double baseBpm = beatGrid.isValid()
        ? static_cast<double> (beatGrid.getProperty (IDs::bpm, 0.0))
        : 0.0;

    if (! std::isfinite (baseBpm) || baseBpm <= 0.0)
        return 0.0;

    return baseBpm;
}

int MasterClockManager::deckIdToSlot (const juce::String& deckId)
{
    if (deckId == "A") return 0;
    if (deckId == "B") return 1;
    if (deckId == "C") return 2;
    if (deckId == "D") return 3;
    return -1;
}

int MasterClockManager::findLowestPlayingDeckIndex() const
{
    auto decks = rootState_.getChildWithName (IDs::Decks);
    for (int i = 0; i < decks.getNumChildren(); ++i)
    {
        auto deck = decks.getChild (i);
        if (getPlaybackStatus (deck) == "playing")
            return i;
    }
    return -1;
}
