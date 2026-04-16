#pragma once

#include "BeatGridData.h"
#include "BeatGridAnalyzer.h"
#include "../Deck/DeckStateManager.h"
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include <juce_data_structures/juce_data_structures.h>
#include <map>
#include <memory>

class BeatGridManager final : private juce::ValueTree::Listener
{
public:
    BeatGridManager (DeckStateManager& deckState,
                     TrackDatabase& database,
                     AudioEngine& engine);
    ~BeatGridManager() override;

    BeatGridManager (const BeatGridManager&) = delete;
    BeatGridManager& operator= (const BeatGridManager&) = delete;

    BeatGridData::Ptr getBeatGridData (const juce::String& deckId) const;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void triggerAnalysis (const juce::String& deckId, const juce::String& contentHash,
                          const juce::String& filePath);

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    BeatGridAnalyzer  analyzer;

    juce::ValueTree rootState;

    std::map<juce::String, BeatGridData::Ptr> beatGridDataMap;
};
