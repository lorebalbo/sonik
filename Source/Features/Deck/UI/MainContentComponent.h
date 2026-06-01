#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../Waveform/WaveformData.h"
#include "../Database/TrackDatabase.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "../../Sync/MasterClockManager.h"
#include "GlobalToolbar.h"
#include "DeckLayoutManager.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/Ui/Organisms/DawPanel.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Projection/LiveProjectionTimer.h"
#include "Features/Daw/Projection/DeckManagerProjectionSource.h"
#include "Features/Library/UI/LibraryComponent.h"
#include "Features/Library/WatchFolderScanner.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerMeterSnapshot.h"
#include "Features/Mixer/Ui/Organisms/MixerComponent.h"
#include "Features/Deck/DeckIdentifiers.h"
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
                          TrackDatabase& trackDb,
                          MixerStateSchema& mixerSchema,
                          MixerMeterSnapshot& mixerMeters,
                          Daw::MasterGridService& gridService)
        : deckStateManager (deckState),
          audioEngine (engine),
          audioFileLoader (loader),
          rootState (deckState.getStateTree()),
          toolbar (deckState, mixerSchema, mixerMeters),
          layoutManager (deckState, engine, loader, waveformMgr, beatGridMgr, stemMgr, clockMgr),
          mixerComponent (mixerSchema, mixerMeters,
                          rootState.getChildWithName (IDs::Decks)),
          dawPanel (gridService,
                    DawState::getOrCreateDawBranch (rootState),
                    [rootTree = rootState](int deckIndex) -> juce::ValueTree
                    {
                        auto decks = rootTree.getChildWithName (IDs::Decks);
                        int count = 0;
                        for (int i = 0; i < decks.getNumChildren(); ++i)
                        {
                            auto deck = decks.getChild (i);
                            if (deck.hasType (IDs::Deck))
                            {
                                if (count == deckIndex)
                                    return deck;
                                ++count;
                            }
                        }
                        return {};
                    },
                    // PRD-0068: resolve a clip's sourceFileId to cached WaveformData
                    // via the PRD-0006 SQLite cache (content-hash keyed). A miss
                    // returns nullptr so the clip draws a placeholder; never analyses.
                    [&trackDb](const juce::String& sourceFileId) -> WaveformData::Ptr
                    {
                        if (sourceFileId.isEmpty())
                            return nullptr;

                        juce::MemoryBlock block;
                        if (! trackDb.loadWaveformData (sourceFileId, block))
                            return nullptr;

                        return WaveformData::deserialize (block, sourceFileId);
                    }),
          libraryComponent (std::make_unique<LibraryComponent> (rootState, trackDb, analysisQueue))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        toolbar.onAddDeckClicked = [this]()
        {
            handleAddDeck();
        };

        dawPanel.onPreferredHeightChanged = [this]() { resized(); };

        addAndMakeVisible (toolbar);
        addAndMakeVisible (dawPanel);
        addAndMakeVisible (layoutManager);
        addAndMakeVisible (mixerComponent);
        addAndMakeVisible (*libraryComponent);

        rootState.addListener (this);

        // PRD-0069: start the live-deck projection bridge. It reads each deck's
        // published atomics on the message thread and grows live clips on the
        // matching DAW lane(s). The DawPanel's now-line (PRD-0070) follows the
        // bridge's monotonic now-line sample.
        projectionSource = std::make_unique<Daw::DeckManagerProjectionSource> (deckStateManager);
        liveProjection   = std::make_unique<Daw::LiveProjectionTimer> (
                               *projectionSource,
                               DawState::getOrCreateDawBranch (rootState),
                               gridService);
        dawPanel.setNowLineProvider ([this]() -> juce::int64
                                     { return liveProjection->getNowLineSample(); });
        liveProjection->start();
    }

    ~MainContentComponent() override
    {
        if (liveProjection != nullptr)
            liveProjection->stop();
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

        // PRD-0066: the DAW arrangement panel docks directly beneath the global
        // toolbar, above the deck rack. It owns its height (collapsed/expanded).
        dawPanel.setBounds (b.removeFromTop (dawPanel.getPreferredHeight()));

        // Deck area: fixed height derived from the number of decks — never
        // compressed, never stretched.  Horizontal margins only (no top/bottom).
        const int deckH = layoutManager.getPreferredHeight();
        auto deckArea = b.removeFromTop (deckH);
        deckArea.removeFromLeft  (8);
        deckArea.removeFromRight (8);
        layoutManager.setBounds (deckArea);

        // PRD-0060 (revised): the mixer no longer occupies a separate band
        // beneath the deck rack — it sits in the centre column carved out by
        // DeckLayoutManager so it visually anchors between deck A and deck B.
        // We translate the manager-local mixer rect to parent coordinates.
        auto mixerCol = layoutManager.getMixerColumnArea();
        if (! mixerCol.isEmpty())
        {
            mixerCol.translate (layoutManager.getX(), layoutManager.getY());
            mixerComponent.setBounds (mixerCol);
            mixerComponent.setVisible (true);
        }
        else
        {
            mixerComponent.setVisible (false);
        }

        // Library fills all remaining vertical space flush below the deck rack.
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

    /** Access the library component for MIDI control wiring. */
    LibraryComponent* getLibraryComponent() const noexcept { return libraryComponent.get(); }

    /** Access the layout manager for MIDI handler engine registration. */
    DeckLayoutManager& getLayoutManager() noexcept { return layoutManager; }

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
    MixerComponent     mixerComponent;
    Daw::DawPanel      dawPanel;            // PRD-0066
    juce::TooltipWindow tooltipWindow { this };

    // ---- Library panel ----------------------------------------------------

    std::unique_ptr<LibraryComponent> libraryComponent;

    // ---- PRD-0069 live-deck projection bridge ----------------------------
    std::unique_ptr<Daw::DeckManagerProjectionSource> projectionSource;
    std::unique_ptr<Daw::LiveProjectionTimer>         liveProjection;

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainContentComponent)
};
