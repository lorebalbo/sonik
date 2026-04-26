#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "GlobalToolbar.h"
#include "DeckLayoutManager.h"

class StemSeparationManager;

class MainContentComponent final : public juce::Component,
                                    private juce::ValueTree::Listener
{
public:
    MainContentComponent (DeckStateManager& deckState,
                          AudioEngine& engine,
                          AudioFileLoader& loader,
                          WaveformManager& waveformMgr,
                          BeatGridManager& beatGridMgr,
                          StemSeparationManager& stemMgr)
        : deckStateManager (deckState),
          audioEngine (engine),
          rootState (deckState.getStateTree()),
          toolbar (deckState),
          layoutManager (deckState, engine, loader, waveformMgr, beatGridMgr, stemMgr)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        toolbar.onAddDeckClicked = [this]()
        {
            handleAddDeck();
        };

        addAndMakeVisible (toolbar);
        addAndMakeVisible (layoutManager);

        rootState.addListener (this);
    }

    ~MainContentComponent() override
    {
        rootState.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFFF9F9F9)); // surface
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        toolbar.setBounds (bounds.removeFromTop (toolbarHeight));
        bounds.reduce (8, 8);  // outer padding: separates decks from the window border
        layoutManager.setBounds (bounds);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // Cmd+1..4 to switch active deck (macOS)
        auto mods = key.getModifiers();
        if (mods.isCommandDown())
        {
            auto ids = deckStateManager.getDeckIds();
            int keyCode = key.getKeyCode();

            // KeyPress encodes '1'..'4' as their ASCII codes
            if (keyCode >= '1' && keyCode <= '4')
            {
                int idx = keyCode - '1';
                if (idx < ids.size())
                {
                    deckStateManager.setActiveDeck (ids[idx]);
                    return true;
                }
            }
        }

        // Space for play/pause
        if (key == juce::KeyPress::spaceKey)
        {
            auto activeDeck = deckStateManager.getActiveDeckId();
            auto* state = deckStateManager.getAudioState (activeDeck);
            if (state != nullptr)
            {
                auto st = static_cast<PlaybackStatusCode> (
                    state->playbackStatus.load (std::memory_order_relaxed));
                if (st == PlaybackStatusCode::playing)
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Pause);
                else if (st == PlaybackStatusCode::stopped
                         || st == PlaybackStatusCode::paused)
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Play);
            }
            return true;
        }

        // S for stop
        if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
        {
            auto activeDeck = deckStateManager.getActiveDeckId();
            audioEngine.sendTransportCommand (activeDeck, TransportCommand::Stop);
            return true;
        }

        return false;
    }

    void visibilityChanged() override
    {
        if (isVisible())
            grabKeyboardFocus();
    }

private:
    void handleAddDeck()
    {
        auto newId = deckStateManager.addDeck();
        if (newId.isNotEmpty())
        {
            // Register with audio engine
            auto* state = deckStateManager.getAudioState (newId);
            if (state != nullptr)
                audioEngine.registerDeck (newId, state);

            toolbar.updateButtons();
        }
    }

    // ValueTree::Listener — update toolbar when deck count or active deck changes
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override
    {
        if (tree == rootState
            && (property == IDs::deckCount || property == IDs::activeDeckId))
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->toolbar.updateButtons();
            });
        }
    }

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    juce::ValueTree   rootState;

    GlobalToolbar      toolbar;
    DeckLayoutManager  layoutManager;
    juce::TooltipWindow tooltipWindow { this };

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainContentComponent)
};
