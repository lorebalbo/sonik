#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "../../Sync/MasterClockManager.h"
#include "DeckShellComponent.h"
#include <vector>
#include <memory>

class StemSeparationManager;

class DeckLayoutManager final : public juce::Component,
                                 private juce::ValueTree::Listener
{
public:
    DeckLayoutManager (DeckStateManager& deckState,
                       AudioEngine& engine,
                       AudioFileLoader& loader,
                       WaveformManager& waveformMgr,
                       BeatGridManager& beatGridMgr,
                       StemSeparationManager& stemMgr,
                       MasterClockManager& clockMgr);
    ~DeckLayoutManager() override;

    void resized() override;

    /// Returns the exact pixel height needed to display all current decks at
    /// their fixed preferred height.  MainContentComponent uses this to size
    /// the deck area so it is neither compressed nor stretched.
    int getPreferredHeight() const noexcept
    {
        const int n = static_cast<int> (deckShells.size());
        if (n <= 2)
            return kPreferredDeckH;
        return 2 * kPreferredDeckH + deckGap;
    }

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
    StemSeparationManager& stemSeparationManager;
    MasterClockManager&   masterClockManager;
    juce::ValueTree   decksNode;

    std::vector<std::unique_ptr<DeckShellComponent>> deckShells;

    static constexpr int deckGap         = 4;
    static constexpr int minDeckWidth    = 420;
    static constexpr int minDeckHeight   = 280;
    // Fixed preferred height per deck — matches DeckShellComponent::kMinDeckH.
    // 20 + 59 + 8 + 23 + 8 + 113 + 12 + 23 + 12 + 23 + 20 = 321 px.
    static constexpr int kPreferredDeckH = 321;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckLayoutManager)
};
