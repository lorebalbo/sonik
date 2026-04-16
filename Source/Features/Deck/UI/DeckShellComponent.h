#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformComponent.h"
#include "../../Waveform/WaveformManager.h"
#include "TrackInfoComponent.h"

class DeckShellComponent final : public juce::Component,
                                  public juce::FileDragAndDropTarget,
                                  private juce::ValueTree::Listener
{
public:
    DeckShellComponent (DeckStateManager& deckState,
                        AudioEngine& engine,
                        AudioFileLoader& loader,
                        WaveformManager& waveformMgr,
                        const juce::String& deckId);
    ~DeckShellComponent() override;

    const juce::String& getDeckId() const { return deckId; }

    /// Called when this deck should check whether it is the active deck.
    void updateActiveState();

    // juce::Component
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /// Callback for remove button clicks.
    std::function<void (const juce::String&)> onRemoveRequested;

private:
    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void paintHeader (juce::Graphics& g, juce::Rectangle<int> area);
    void paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area);
    void paintActiveIndicator (juce::Graphics& g);

    bool isTrackLoaded() const;
    bool isPlaying() const;
    bool isActive() const;

    static juce::Colour getDeckAccentColour (const juce::String& id);

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    AudioFileLoader&  audioFileLoader;
    WaveformManager&  waveformManager;
    juce::String      deckId;
    juce::ValueTree   deckTree;
    juce::ValueTree   rootState;

    juce::TextButton  removeButton;
    bool              isDragOver = false;

    std::unique_ptr<WaveformComponent>  waveformComponent;
    std::unique_ptr<TrackInfoComponent>  trackInfoComponent;

    static constexpr int headerHeight        = 32;
    static constexpr int trackInfoHeight     = 90;
    static constexpr int activeIndicatorWidth = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckShellComponent)
};
