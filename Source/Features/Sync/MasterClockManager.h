#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "MasterClockPublisher.h"
#include "../Deck/DeckIdentifiers.h"

/// Manages master clock assignment and publishes MasterClockSnapshot updates.
///
/// Runs EXCLUSIVELY on the JUCE message thread. All ValueTree reads/writes are
/// synchronous on the message thread; the publisher is the only cross-thread boundary.
///
/// Responsibilities:
///   - Mutual exclusion: exactly one deck has isMaster=true at any time.
///   - Auto-promotion: when the master stops/ejects, promote the lowest-index playing deck.
///   - Dormant state: masterDeckIndex=-1, masterIsPlaying=false, lastMasterBPM retained.
///   - Manual override via setMaster(deckIndex).
///   - Pause/resume: publishes isPlaying flag changes without altering BPM or phase origin.
///   - Tracks BPM/anchor updates on the master deck and republishes when they change.
class MasterClockManager final : private juce::ValueTree::Listener
{
public:
    /// @param rootState  The root SonikState ValueTree (from DeckStateManager::getStateTree()).
    ///                   This class holds a reference-counted copy; lifetime is safe.
    /// @param publisher  The SeqLock publisher injected from SonikApplication. NOT owned here.
    MasterClockManager (juce::ValueTree rootState, MasterClockPublisher& publisher);
    ~MasterClockManager() override;

    MasterClockManager (const MasterClockManager&)            = delete;
    MasterClockManager& operator= (const MasterClockManager&) = delete;

    /// Manually assign a deck as master.
    /// No-op if deckIndex is already master AND that deck is currently playing.
    /// Otherwise: demotes current master, sets isSynced=false on new deck, promotes new deck.
    void setMaster (int deckIndex);

    /// Returns the index of the current master deck, or -1 if dormant.
    int getMasterDeckIndex() const;

private:
    // --- juce::ValueTree::Listener ---
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded       (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved     (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged    (juce::ValueTree&) override {}

    // --- Helpers ---

    /// Get the deck ValueTree at the given Decks-array index. Returns invalid tree if out of range.
    juce::ValueTree getDeckTreeAt (int index) const;

    /// Return the Decks-array index of the given Deck tree, or -1 if not found.
    int getDeckIndex (const juce::ValueTree& deckTree) const;

    /// Promote the deck at deckIndex: set isMaster=true, update masterDeckIndex, publish.
    void promoteDeck (int deckIndex);

    /// Demote the current master (if any): set isMaster=false, masterDeckIndex=-1.
    void demoteCurrentMaster();

    /// Publish a snapshot derived from the deck at deckIndex (reads BPM + anchor from ValueTree).
    void publishFromDeck (int deckIndex);

    /// Publish a dormant snapshot: masterIsPlaying=false, lastMasterBPM retained, phaseOrigin=0.
    void publishDormant();

    /// Return "playing", "paused", "stopped", or "empty" for a deck tree.
    static juce::String getPlaybackStatus (const juce::ValueTree& deckTree);

    /// Find the lowest-index deck that is currently "playing". Returns -1 if none.
    int findLowestPlayingDeckIndex() const;

    juce::ValueTree       rootState_;
    MasterClockPublisher& publisher_;

    /// Most-recently-published BPM; retained when entering dormant state.
    double lastMasterBPM_ = 0.0;
};
