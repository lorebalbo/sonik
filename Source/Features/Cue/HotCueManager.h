#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "HotCueData.h"
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include "../Deck/Database/TrackDatabase.h"
#include "../BeatGrid/BeatGridData.h"
#include <array>
#include <optional>

struct DeckAudioState;

class HotCueManager final : private juce::ValueTree::Listener,
                             private juce::Timer
{
public:
    HotCueManager (juce::ValueTree deckTree,
                   AudioEngine& engine,
                   const juce::String& deckId,
                   TrackDatabase& db);
    ~HotCueManager() override;

    // --- Actions (called from UI thread) ---

    void setCue (int padIndex);
    void triggerCue (int padIndex);
    void deleteCue (int padIndex);
    void undoDelete();
    void setColor (int padIndex, int colorIndex);
    void setLabel (int padIndex, const juce::String& label);

    // --- State access ---

    std::array<HotCueInfo, 8> getHotCues() const;

    // --- Audio state (live playhead from audio thread) ---

    void setAudioState (DeckAudioState* state);

    // --- BeatGrid data (updated when analysis completes) ---

    void setBeatGridData (BeatGridData::Ptr data);

    // --- Listener interface ---

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void hotCuesChanged() = 0;
    };

    void addListener (Listener* l);
    void removeListener (Listener* l);

private:
    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    // Timer (undo window expiry)
    void timerCallback() override;

    void loadCuesFromDB (const juce::String& contentHash);
    void saveCuesToDB();
    juce::String serializeCuesToJson() const;
    void parseCuesFromJson (const juce::String& json, int64_t totalSamples);
    void cancelPendingUndo();
    void notifyListeners();

    juce::ValueTree tree;
    juce::ValueTree cuePointsNode;
    juce::ValueTree trackMetaNode;
    AudioEngine&    audioEngine;
    juce::String    deckId;
    TrackDatabase&  database;
    BeatGridData::Ptr beatGridData;
    DeckAudioState* audioState = nullptr;

    juce::String currentContentHash;

    // Undo state
    struct UndoState
    {
        int          padIndex   = -1;
        int64_t      position   = -1;
        int          colorIndex = 0;
        juce::String label;
    };
    std::optional<UndoState> pendingUndo;

    // Background DB writer
    juce::ThreadPool dbWritePool { 1 };

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotCueManager)
};
