#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformComponent.h"
#include "../../Waveform/WaveformManager.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "TrackInfoComponent.h"
#include "PitchFaderComponent.h"
#include "KeyLockButton.h"
#include "KeyStepperComponent.h"
#include "SyncButtonComponent.h"
#include "../../Sync/UI/SyncButton.h"
#include "../../Sync/UI/MasterButton.h"
#include "../../Sync/MasterClockManager.h"
#include "ControllerWidget.h"
#include "../../Cue/HotCueManager.h"
#include "../../Cue/HotCuePadComponent.h"
#include "../../Quantize/QuantizeButtonComponent.h"
#include "../../Loop/LoopEngine.h"
#include "../../Loop/LoopControlComponent.h"
#include "../../BeatJump/BeatJumpEngine.h"
#include "../../BeatJump/BeatJumpComponent.h"
#include "../../SlipMode/SlipButtonComponent.h"
#include "../../StemSeparation/UI/StemSeparateButton.h"
#include "../../StemSeparation/UI/StemToggleComponent.h"

class StemSeparationManager;

class DeckShellComponent final : public juce::Component,
                                  public juce::FileDragAndDropTarget,
                                  public juce::DragAndDropTarget,
                                  private juce::ValueTree::Listener,
                                  private HotCueManager::Listener,
                                  private LoopEngine::Listener
{
public:
    DeckShellComponent (DeckStateManager& deckState,
                        AudioEngine& engine,
                        AudioFileLoader& loader,
                        WaveformManager& waveformMgr,
                        BeatGridManager& beatGridMgr,
                        StemSeparationManager& stemMgr,
                        MasterClockManager& clockMgr,
                        const juce::String& deckId);
    ~DeckShellComponent() override;

    const juce::String& getDeckId() const { return deckId; }

    /// Called when this deck should check whether it is the active deck.
    void updateActiveState();

    // juce::Component
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // juce::FileDragAndDropTarget  (OS file drag from Finder)
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // juce::DragAndDropTarget  (in-app drag from library table)
    bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragEnter (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragExit  (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDropped   (const juce::DragAndDropTarget::SourceDetails& details) override;

    /// Callback for remove button clicks.
    std::function<void (const juce::String&)> onRemoveRequested;

private:
    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void paintDeckBackground (juce::Graphics& g);
    void paintEmptyState     (juce::Graphics& g, juce::Rectangle<int> area);

    bool isTrackLoaded() const;
    bool isPlaying() const;
    bool isActive() const;

    // Playback helpers wired into ControllerWidget JUMP tab callbacks
    void handleCuePress();
    void handleStopPress();
    void handlePlayPress();

    // Beatgrid helpers wired into ControllerWidget GRID tab callbacks
    void handleGridSet();
    void handleGridDelete();
    void handleGridNudge (int delta);
    void handleBpmSave   (double newBpm);

    /// Persist the current in-memory beatgrid to SQLite, preserving other fields.
    void persistBeatGridToDb();

    // ---- Track loading via pendingLoadPath (PRD-0034) ----------------------
    /// Entry point: validates the path, checks file existence and format,
    /// auto-disengages SYNC, fires AudioFileLoader, sets loadedFilePath.
    void handlePendingLoad      (const juce::String& path);
    /// Opens a FileChooser to let the user relocate a missing file.
    void showRelocateDialog     (const juce::String& originalPath);
    /// Removes a track from the library_tracks table by file_path.
    void removeTrackFromLibrary (const juce::String& filePath);

    DeckStateManager&      deckStateManager;
    AudioEngine&           audioEngine;
    AudioFileLoader&       audioFileLoader;
    WaveformManager&       waveformManager;
    BeatGridManager&       beatGridManager;
    StemSeparationManager& stemSeparationManager;
    MasterClockManager&    masterClockManager;
    juce::String           deckId;
    juce::ValueTree        deckTree;
    juce::ValueTree        rootState;

    bool isDragOver = false;

    // Deck header (always visible, shows track info + deck badge)
    std::unique_ptr<TrackInfoComponent>   trackInfoComponent;

    // Stems row
    std::unique_ptr<StemSeparateButton>   stemSeparateButton;
    std::unique_ptr<StemToggleComponent>  stemVocToggle;
    std::unique_ptr<StemToggleComponent>  stemInstToggle;

    // Waveform
    std::unique_ptr<WaveformComponent>    waveformComponent;

    // Time & Pitch sidebar (right of waveform)
    std::unique_ptr<KeyLockButton>        keyLockButton;
    std::unique_ptr<KeyStepperComponent>  keyStepperComponent;
    std::unique_ptr<PitchFaderComponent>  pitchFaderComponent;

    // Control row (below waveform)
    std::unique_ptr<MasterButton>            masterButton;
    std::unique_ptr<SyncButton>              syncButton;
    std::unique_ptr<QuantizeButtonComponent> quantizeButton;
    std::unique_ptr<SlipButtonComponent>     slipButton;

    // Controller Widget (tabbed LOOP / CUE / JUMP / GRID)
    std::unique_ptr<HotCueManager>        hotCueManager;
    std::unique_ptr<HotCuePadComponent>   hotCuePadComponent;
    std::unique_ptr<LoopEngine>           loopEngine;
    std::unique_ptr<LoopControlComponent> loopControlComponent;
    std::unique_ptr<BeatJumpEngine>       beatJumpEngine;
    std::unique_ptr<BeatJumpComponent>    beatJumpComponent;
    std::unique_ptr<ControllerWidget>     controllerWidget;

    void hotCuesChanged() override;
    void updateWaveformHotCues();

    void loopStateChanged() override;
    void updateLoopControlState();

    // -----------------------------------------------------------------------
    // Layout constants matching Figma Deck frame (592 x 505, padding 20)
    // -----------------------------------------------------------------------
    static constexpr int kPad              = 20;   // outer padding
    static constexpr int kGap              = 12;   // gap between rows
    static constexpr int kHeaderH          = 59;   // Deck Header height
    static constexpr int kStemsH           = 23;   // Stems row height
    static constexpr int kMainH            = 226;  // Waveform + Pitch section height
    static constexpr int kControlRowH      = 23;   // SYNC / QUANT / SLIP row height
    static constexpr int kControllerH      = 86;   // Controller Widget height
    static constexpr int kPitchSidebarW    = 70;   // Time & Pitch section width
    static constexpr int kPanelW           = 474;  // Waveform / panel width (Figma 592px deck)
    static constexpr int kSidebarGap       = 8;    // gap between panel and sidebar

    // Dynamic panel width — adapts to the actual component width.
    // When the deck is exactly 592 px wide this equals kPanelW (474).
    int getPanelW() const noexcept
    {
        return juce::jmax (1, getWidth() - 2 * kPad - kSidebarGap - kPitchSidebarW);
    }

    // Minimum deck height derived from above constants
    static constexpr int kMinDeckH = kPad + kHeaderH + kGap
                                   + kStemsH  + kGap
                                   + kMainH   + kGap
                                   + kControlRowH + kGap
                                   + kControllerH + kPad;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckShellComponent)
};

