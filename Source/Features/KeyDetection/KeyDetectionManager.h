#pragma once

#include "KeyDetectionAnalyzer.h"
#include "../Deck/DeckStateManager.h"
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include <juce_data_structures/juce_data_structures.h>

class KeyDetectionManager final : private juce::ValueTree::Listener
{
public:
    KeyDetectionManager (DeckStateManager& deckState,
                         TrackDatabase& database,
                         AudioEngine& engine);
    ~KeyDetectionManager() override;

    KeyDetectionManager (const KeyDetectionManager&) = delete;
    KeyDetectionManager& operator= (const KeyDetectionManager&) = delete;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void triggerAnalysis (const juce::String& deckId,
                          const juce::String& contentHash,
                          const juce::String& filePath);

    DeckStateManager& deckStateManager;
    TrackDatabase&    db;
    AudioEngine&      audioEngine;
    KeyDetectionAnalyzer analyzer;

    juce::ValueTree rootState;
};
