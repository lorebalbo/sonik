#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include "../Deck/Database/TrackDatabase.h"
#include "../BeatGrid/BeatGridData.h"
#include "../Quantize/QuantizeService.h"

struct DeckAudioState;

class LoopEngine final : private juce::ValueTree::Listener
{
public:
    LoopEngine (juce::ValueTree deckTree,
                AudioEngine& engine,
                const juce::String& deckId,
                TrackDatabase& db);
    ~LoopEngine() override;

    void setAudioState (DeckAudioState* state);
    void setBeatGridData (BeatGridData::Ptr data);

    // --- Loop actions (UI thread) ---
    void autoLoop (float beatCount);
    void setLoopIn();
    void setLoopOut();
    void toggleLoop();
    void reLoop();
    void loopHalve();
    void loopDouble();

    /// Shift active loop boundaries by `offset` samples (positive = forward, negative = backward).
    /// Called by BeatJumpEngine when a loop is active.
    /// When quantize is enabled, pass snap=true with anchor/beatInterval to re-snap boundaries.
    /// Returns true if the shift succeeded, false if rejected (e.g., insufficient room).
    bool shiftLoop (int64_t offset, bool snap = false, int64_t anchor = 0, double beatInterval = 0.0);

    // --- State access ---
    struct LoopInfo
    {
        int64_t inSamples      = -1;
        int64_t outSamples     = -1;
        bool    active         = false;
        int     mode           = 0;       // 0=none, 1=auto, 2=manual
        float   activeAutoBeats = 0.0f;
        bool    pendingIn      = false;
        int64_t pendingInPos   = -1;
    };

    LoopInfo getLoopInfo() const;

    // --- Listener ---
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void loopStateChanged() = 0;
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

    void notifyListeners();
    void loadLoopsFromDB (const juce::String& contentHash);
    void autoSaveCurrentLoop();

    double  getBeatInterval() const;
    double  getSampleRate() const;
    int64_t readPlayhead() const;

    juce::ValueTree   tree;
    juce::ValueTree   loopNode;
    juce::ValueTree   trackMetaNode;
    AudioEngine&      audioEngine;
    juce::String      deckId;
    TrackDatabase&    database;
    BeatGridData::Ptr beatGridData;
    DeckAudioState*   audioState = nullptr;

    juce::String currentContentHash;

    float   activeAutoBeats      = 0.0f;
    bool    hasPendingLoopIn     = false;
    int64_t pendingLoopInSamples = -1;

    juce::ThreadPool dbWritePool { 1 };
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopEngine)
};
