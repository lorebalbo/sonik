#pragma once

#include "WaveformData.h"
#include "WaveformAnalyzer.h"
#include "../Deck/DeckStateManager.h"
#include "../Deck/DeckIdentifiers.h"
#include "../AudioEngine/AudioEngine.h"
#include <juce_data_structures/juce_data_structures.h>
#include <map>
#include <memory>

class WaveformManager final : private juce::ValueTree::Listener
{
public:
    WaveformManager (DeckStateManager& deckState,
                     TrackDatabase& database,
                     AudioEngine& engine);
    ~WaveformManager() override;

    WaveformManager (const WaveformManager&) = delete;
    WaveformManager& operator= (const WaveformManager&) = delete;

    WaveformData::Ptr getWaveformData (const juce::String& deckId) const;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void triggerAnalysis (const juce::String& deckId, const juce::String& contentHash);

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    WaveformAnalyzer  analyzer;

    juce::ValueTree rootState;

    std::map<juce::String, WaveformData::Ptr> waveformDataMap;
};
