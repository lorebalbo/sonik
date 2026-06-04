#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <optional>
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

    //--------------------------------------------------------------------------
    // PRD-0092: Automation tempo override (additive, opt-in).
    //
    // During DAW timeline playback the AutomationApplier may drive the master
    // tempo from the recorded "master/tempo" lane. To keep MasterClockManager the
    // SINGLE tempo authority (no forked tempo path), the applier writes the
    // automated BPM here rather than poking the publisher directly. When the
    // override is active, the published MasterClockSnapshot's masterBPM uses the
    // override so the grid, synced decks, and grid-aligned clips all follow the
    // automated tempo from this one authority.
    //
    // Inactive by default ⇒ existing behaviour (derived deck BPM) is unchanged,
    // so existing MasterClockTests are unaffected. Message-thread only.
    //--------------------------------------------------------------------------

    /// Engage / update the automation tempo override and re-publish immediately.
    void setAutomationTempoOverride (double bpm);

    /// Disengage the automation tempo override; revert to derived BPM and
    /// re-publish immediately.
    void clearAutomationTempoOverride();

    /// True while the automation tempo override is engaged.
    bool hasAutomationTempoOverride() const noexcept { return automationTempoOverride_.has_value(); }

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

    /// Compute effective master BPM from beat-grid BPM and deck speed multiplier.
    /// For an unsynced master deck, speedMultiplier mirrors the pitch-fader value.
    static double getDeckEffectiveBpm (const juce::ValueTree& deckTree);

    /// Read native beat-grid BPM (source-domain BPM before pitch/sync speed scaling).
    static double getDeckNativeBpm (const juce::ValueTree& deckTree);

    /// Convert deck ID (A/B/C/D) to fixed audio slot index (0/1/2/3).
    static int deckIdToSlot (const juce::String& deckId);

    /// Find the lowest-index deck that is currently "playing". Returns -1 if none.
    int findLowestPlayingDeckIndex() const;

    juce::ValueTree       rootState_;
    MasterClockPublisher& publisher_;

    /// Most-recently-published BPM; retained when entering dormant state.
    double lastMasterBPM_ = 0.0;
    double lastMasterNativeBPM_ = 0.0;

    /// PRD-0092: when engaged, the published masterBPM uses this value instead of
    /// the derived deck BPM. std::nullopt ⇒ inactive (default).
    std::optional<double> automationTempoOverride_;

    /// Re-publish the current master state (or dormant) — used after the override
    /// is set/cleared so the new BPM reaches the audio thread immediately.
    void republishCurrent();
};
