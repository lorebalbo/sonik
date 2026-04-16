#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "DeckShellComponent.h"
#include <vector>
#include <memory>

class DeckLayoutManager final : public juce::Component,
                                 private juce::ValueTree::Listener
{
public:
    DeckLayoutManager (DeckStateManager& deckState,
                       AudioEngine& engine,
                       AudioFileLoader& loader,
                       WaveformManager& waveformMgr,
                       BeatGridManager& beatGridMgr);
    ~DeckLayoutManager() override;

    void resized() override;

private:
    // ValueTree::Listener
    void valueTreeChildAdded (juce::ValueTree& parent,
                              juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent,
                                juce::ValueTree& child,
                                int index) override;

    void rebuildDeckShells();
    void addDeckShell (const juce::String& deckId);
    void removeDeckShell (const juce::String& deckId);
    void applyLayout();
    void handleRemoveRequest (const juce::String& deckId);

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    AudioFileLoader&  audioFileLoader;
    WaveformManager&  waveformManager;
    BeatGridManager&  beatGridManager;
    juce::ValueTree   decksNode;

    std::vector<std::unique_ptr<DeckShellComponent>> deckShells;

    static constexpr int deckGap       = 4;
    static constexpr int minDeckWidth  = 420;
    static constexpr int minDeckHeight = 280;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckLayoutManager)
};
