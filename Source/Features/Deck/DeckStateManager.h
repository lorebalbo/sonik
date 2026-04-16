#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "DeckIdentifiers.h"
#include "AudioThreadState.h"
#include "Database/TrackDatabase.h"
#include <map>
#include <memory>
#include <set>

struct TrackMetadata
{
    juce::String filePath;
    juce::String contentHash;
    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String initialKeyString;   // embedded key tag (TKEY/KEY/INITIALKEY)
    double       duration      = 0.0;
    double       sampleRate    = 0.0;
    int          bitDepth      = 0;
    int          channelCount  = 0;
    int64_t      totalSamples  = 0;
    bool         hasAlbumArt   = false;
};

class DeckStateManager final
{
public:
    explicit DeckStateManager (TrackDatabase& database);
    ~DeckStateManager() = default;

    DeckStateManager (const DeckStateManager&) = delete;
    DeckStateManager& operator= (const DeckStateManager&) = delete;

    // Deck management
    juce::String addDeck();
    bool         removeDeck (const juce::String& deckId);
    bool         canRemoveDeck (const juce::String& deckId) const;

    // Accessors
    juce::ValueTree      getDeckState (const juce::String& deckId) const;
    juce::String         getActiveDeckId() const;
    void                 setActiveDeck (const juce::String& deckId);
    int                  getDeckCount() const;
    juce::ValueTree      getStateTree() const;
    juce::StringArray    getDeckIds() const;

    // Track loading
    void loadTrack (const juce::String& deckId, const TrackMetadata& metadata);
    bool ejectTrack (const juce::String& deckId);
    bool canEjectTrack (const juce::String& deckId) const;

    // Playback
    bool setPlaybackStatus (const juce::String& deckId, const juce::String& newStatus);
    void setMasterTempo (const juce::String& deckId);

    // Audio thread state access (safe to read from audio thread)
    DeckAudioState* getAudioState (const juce::String& deckId);

    // Session persistence
    void saveSession();
    void restoreSession();

private:
    juce::ValueTree createDeckTree (const juce::String& deckId) const;
    void resetTrackSpecificState (juce::ValueTree& deckTree);
    juce::String getPlaybackStatus (const juce::ValueTree& deckTree) const;
    bool isValidTransition (const juce::String& from, const juce::String& to) const;
    juce::String findNextAvailableLetter() const;

    TrackDatabase& db;
    juce::ValueTree rootState;

    struct DeckRuntime
    {
        DeckAudioState audioState;
        std::unique_ptr<AudioStateSync> sync;
    };

    std::map<juce::String, std::unique_ptr<DeckRuntime>> deckRuntimes;
    std::set<juce::String> usedLetters;

    static constexpr int maxDecks = 4;
    static inline const juce::String letterPool[] = { "A", "B", "C", "D" };
};
