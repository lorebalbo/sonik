#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "../../Sync/MasterClockManager.h"
#include "GlobalToolbar.h"
#include "DeckLayoutManager.h"
#include "Features/Library/UI/LibraryComponent.h"
#include "Features/Library/WatchFolderScanner.h"
#include <functional>
#include <memory>

class StemSeparationManager;

class MainContentComponent final : public juce::Component,
                                    public juce::DragAndDropContainer,
                                    private juce::ValueTree::Listener
{
public:
    MainContentComponent (DeckStateManager& deckState,
                          AudioEngine& engine,
                          AudioFileLoader& loader,
                          WaveformManager& waveformMgr,
                          BeatGridManager& beatGridMgr,
                          StemSeparationManager& stemMgr,
                          MasterClockManager& clockMgr,
                          LibraryAnalysisQueue& analysisQueue,
                          TrackDatabase& trackDb)
        : deckStateManager (deckState),
          audioEngine (engine),
          audioFileLoader (loader),
          rootState (deckState.getStateTree()),
          toolbar (deckState),
          layoutManager (deckState, engine, loader, waveformMgr, beatGridMgr, stemMgr, clockMgr),
          libraryComponent (std::make_unique<LibraryComponent> (rootState, trackDb, analysisQueue))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        toolbar.onAddDeckClicked = [this]()
        {
            handleAddDeck();
        };

        addAndMakeVisible (toolbar);
        addAndMakeVisible (layoutManager);
        addAndMakeVisible (*libraryComponent);

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
        auto b = getLocalBounds();
        toolbar.setBounds (b.removeFromTop (toolbarHeight));

        // Deck area: fixed height derived from the number of decks — never
        // compressed, never stretched.  Horizontal margins only (no top/bottom).
        const int deckH = layoutManager.getPreferredHeight();
        auto deckArea = b.removeFromTop (deckH);
        deckArea.removeFromLeft  (8);
        deckArea.removeFromRight (8);
        layoutManager.setBounds (deckArea);

        // Library fills all remaining vertical space flush to the deck area.
        libraryComponent->setBounds (b);
    }

    void registerScannerWithLibrary (WatchFolderScanner& scanner)
    {
        if (libraryComponent)
            libraryComponent->setScanner (scanner);
    }

    /** PRD-0048: route the toolbar MIDI button click to the application. */
    void setOnMidiClicked (std::function<void()> cb)
    {
        toolbar.onMidiClicked = std::move (cb);
    }

    void savePreparationListBeforeQuit (std::function<void(bool)> completion)
    {
        if (libraryComponent != nullptr)
            libraryComponent->savePreparationListBeforeQuit (std::move (completion));
        else if (completion)
            completion (true);
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
                {
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Pause);
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "paused");
                }
                else if (st == PlaybackStatusCode::stopped
                         || st == PlaybackStatusCode::paused)
                {
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Play);
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "playing");
                }
            }
            return true;
        }

        // S for stop
        if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
        {
            auto activeDeck = deckStateManager.getActiveDeckId();
            auto* state = deckStateManager.getAudioState (activeDeck);
            if (state != nullptr)
            {
                auto st = static_cast<PlaybackStatusCode> (
                    state->playbackStatus.load (std::memory_order_relaxed));
                if (st == PlaybackStatusCode::playing || st == PlaybackStatusCode::paused)
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "stopped");
            }
            audioEngine.sendTransportCommand (activeDeck, TransportCommand::Stop);
            return true;
        }

        return false;
    }

    void visibilityChanged() override
    {
        if (!isVisible())
            return;

        juce::Component::SafePointer<MainContentComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr && safeThis->isShowing())
                safeThis->grabKeyboardFocus();
        });
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
                {
                    safeThis->toolbar.updateButtons();
                    safeThis->resized(); // re-layout when deck count changes
                }
            });
        }
    }

    DeckStateManager& deckStateManager;
    AudioEngine&      audioEngine;
    AudioFileLoader&  audioFileLoader;
    juce::ValueTree   rootState;

    GlobalToolbar      toolbar;
    DeckLayoutManager  layoutManager;
    juce::TooltipWindow tooltipWindow { this };

    // ---- Library panel ----------------------------------------------------

    std::unique_ptr<LibraryComponent> libraryComponent;

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainContentComponent)
};
