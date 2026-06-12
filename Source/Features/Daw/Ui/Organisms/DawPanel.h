#pragma once
//==============================================================================
// PRD-0066: DawPanel organism.
//
// The top-docked DAW panel shell. It owns the TimelineTransform (PRD-0065) for
// its own viewport and hosts the TimeRuler molecule (PRD-0066). A single,
// always-visible collapse/expand toggle in the header switches the panel
// between two fixed heights; collapse/expand is instant (no animation) and asks
// the parent to reflow via onPreferredHeightChanged.
//
// The grid is the single source of truth: the panel never stores tempo/phase,
// it derives its transform from the injected MasterGridService each refresh.
// A low-rate, change-gated timer rebuilds the transform only when the master
// grid actually changes (tempo/phase/transport), so an idle panel does no work.
//
// Message/UI thread only. No audio-thread code.
//==============================================================================

#include <functional>
#include <limits>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Molecules/TimeRuler.h"
#include "../Atoms/Playhead.h"
#include "../FollowController.h"
#include "../DawLayoutMetrics.h"
#include "../ClipInteraction.h"
#include "ChannelGroupStack.h"
#include "../../Automation/AutomationModel.h"
#include "../../Automation/Ui/ContinuousAutomationLaneView.h"
#include "../../Model/MasterGridService.h"
#include "../../Transform/TimelineTransform.h"
#include "../../State/DawState.h"
#include "../../Editing/EditCommands.h"
#include "../../Playback/DawTransport.h"
#include "../../Playback/ArrangementPublisher.h"
#include "../../Playback/ArrangementCompiler.h"
#include "../../Playback/ArrangementRecompileTrigger.h"

namespace Daw
{

class DawPanel final : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer,
                       private juce::ValueTree::Listener
{
public:
    //--------------------------------------------------------------------------
    // Layout metrics. The expanded panel is header + ruler + a scrollable
    // channel-group body sized to show one full channel group plus a peek of
    // the next (the body scrolls vertically when groups overflow, PRD-0067).
    //--------------------------------------------------------------------------
    static constexpr int kHeaderHeight = 44;
    static constexpr int kBodyHeight   = DawLayout::kExpandedGroupHeight + 20;

    static constexpr int kCollapsedHeight = kHeaderHeight;
    static constexpr int kExpandedHeight  = kHeaderHeight
                                          + TimeRuler::kRulerHeight
                                          + kBodyHeight;

    // Default zoom: a bar is ~50 px wide to match the Figma DAW ruler.
    static constexpr double kDefaultPixelsPerBeat = 50.0 / DawState::kBeatsPerBar;

    // dawBranch    — the "Daw" ValueTree branch (holds the tracks container).
    // deckResolver — maps a track's deckIndex to its deck ValueTree.
    // waveformSource — read-only waveform cache accessor for clip rendering.
    DawPanel (MasterGridService& gridService,
              juce::ValueTree dawBranch,
              ChannelGroupStack::DeckResolver deckResolver,
              ClipBlock::WaveformSource waveformSource = {},
              ClipBlock::NameSource clipNameSource = {});
    ~DawPanel() override;

    //--------------------------------------------------------------------------
    // Collapse / expand state.
    //--------------------------------------------------------------------------
    bool isExpanded() const noexcept { return expanded_; }
    void setExpanded (bool shouldBeExpanded);

    //--------------------------------------------------------------------------
    // Full-size view toggle (Logic-style): when full-size the HOST gives the
    // panel the whole content area below the global toolbar (and hides the deck
    // rack / mixer / library); the panel itself only owns the flag + button.
    // Entering full-size forces the expanded state so the timeline is visible.
    //--------------------------------------------------------------------------
    bool isFullSize() const noexcept { return fullSize_; }
    void setFullSize (bool shouldBeFullSize);

    // PRD-0093: master tempo automation lane disclosure (hidden by default, so
    // the panel's default expanded height is unchanged).
    bool isMasterAutomationRevealed() const noexcept { return masterAutoRevealed_; }
    void setMasterAutomationRevealed (bool shouldBeRevealed);

    // The height the parent should give this panel in the current state.
    int  getPreferredHeight() const noexcept
    {
        if (! expanded_)
            return kCollapsedHeight;
        int h = kExpandedHeight;
        if (masterAutoRevealed_)
            h += AutomationLaneMetrics::kAutomationLaneHeight;
        return h;
    }

    // PRD-0093: access to the owned AutomationModel (constructed over the same
    // daw branch the stack observes).
    AutomationModel& getAutomationModel() noexcept { return automationModel_; }

    // Invoked when the preferred height changes so the parent can reflow.
    std::function<void()> onPreferredHeightChanged;

    // PRD-0070: source of the live now-line sample (the bridge's now-line).
    // When unset the now-line is hidden.
    void setNowLineProvider (std::function<std::int64_t()> provider);

    // Track-header volume faders: maps a deck/channel index to its mixer channel
    // ValueTree (the host injects the MixerStateSchema accessor).
    void setMixerChannelResolver (ChannelGroupStack::ChannelResolver resolver)
    {
        stack_.setMixerChannelResolver (std::move (resolver));
    }

    // Fader level meters: maps a channel index to its current linear peak level
    // (the host resolves live + arrangement-playback meter snapshots).
    void setChannelLevelProvider (ChannelGroupView::ChannelLevelProvider provider)
    {
        stack_.setChannelLevelProvider (std::move (provider));
    }

    //--------------------------------------------------------------------------
    // PRD-0082: DAW transport control callbacks.
    // Wire these to a DawTransport instance (owned by the SonikApplication or AudioEngine).
    std::function<void()> onTransportPlay;
    std::function<void()> onTransportPause;
    std::function<void()> onTransportStop;
    std::function<void()> onTransportLoopToggle;

    /// Metronome (testing aid) toggle. The panel owns the on/off state and
    /// passes the new value; the host routes it to AudioEngine::setMetronomeEnabled.
    std::function<void (bool)> onMetronomeToggle;

    /// Polled each timer tick to draw the correct active state for transport buttons.
    std::function<bool()> isTransportPlaying;
    std::function<bool()> isTransportPaused;
    std::function<bool()> isTransportLoopEnabled;

    /// Returns the owned DawTransport so AudioEngine can wire advancePlayhead.
    Daw::DawTransport& getDawTransport() noexcept { return *transport_; }

    /// Returns the shared ArrangementPublisher so AudioEngine can wire the
    /// TimelineRenderer to the same published snapshot the panel compiles.
    Daw::ArrangementPublisher& getArrangementPublisher() noexcept { return arrangementPublisher_; }

    /// Returns the owned recompile trigger so the host can inject a playback-aware
    /// compiler (EPIC-0010) and force recompiles. Null before construction completes.
    Daw::ArrangementRecompileTrigger* getRecompileTrigger() noexcept { return recompileTrigger_.get(); }

    /// Returns the owned EditCommandDispatcher so the host can set the DAW ValueTree
    /// and UndoManager after construction.
    Daw::EditCommandDispatcher* getEditDispatcher() noexcept { return dispatcher_.get(); }

    // PRD-0078: global record control + record playhead.
    //--------------------------------------------------------------------------
    // The three colour-free visual states of the Record button, matching the
    // recording session controller's lifecycle (PRD-0071).
    enum class RecordUiState { Idle, Armed, Recording };

    // Pressed when the DJ clicks the Record button. The host wires this to the
    // recording session controller's arm/stop (PRD-0071). When unset the button
    // is still drawn but inert.
    std::function<void()> onRecordToggle;

    // Polled (message thread) for the current record lifecycle state so the
    // button renders idle / armed / recording. When unset the button stays idle.
    void setRecordStateProvider (std::function<RecordUiState()> provider);

    // Polled for the live record position sample; while armed/recording this
    // drives the single shared playhead (the same cursor playback uses), so the
    // view follows the recording and the record position is always reachable.
    void setRecordPlayheadProvider (std::function<std::int64_t()> provider);

    // Access to the owned transform (PRD-0067+ interaction lives on the panel).
    TimelineTransform&       getTransform()       noexcept { return transform_; }
    const TimelineTransform& getTransform() const noexcept { return transform_; }

    //--------------------------------------------------------------------------
    // PRD-0096: the DAW edit-history undo manager is owned here (the
    // EditCommandDispatcher delegates to it). The SessionController consumes it
    // to reset the undo baseline after Open / New.
    //--------------------------------------------------------------------------
    juce::UndoManager& getUndoManager() noexcept { return undoManager_; }

    //--------------------------------------------------------------------------
    // PRD-0096 view-state capture/restore. The persisted view chrome is the
    // horizontal zoom (samples-per-pixel) and the left-edge scroll sample.
    // captureViewZoomSamplesPerPixel()/captureViewScrollStartSample() read the
    // live transform at save time; restoreViewState() re-applies a persisted
    // pair AFTER the UI rebuild (§1.5.5), falling back to fit-to-width/start
    // when either value is absent or out of the transform's valid range.
    //--------------------------------------------------------------------------
    double       captureViewZoomSamplesPerPixel() const;
    std::int64_t captureViewScrollStartSample()   const noexcept
    {
        return transform_.getLeftEdgeSample();
    }
    void restoreViewState (std::optional<double>       zoomSamplesPerPixel,
                           std::optional<std::int64_t> scrollStartSample);

    //--------------------------------------------------------------------------
    // PRD-0096: the in-DAW session indicator shown in the header
    // ("My Set.soniksession" plus a trailing dot when dirty). Setting it
    // repaints the header.
    //--------------------------------------------------------------------------
    void setSessionTitle (const juce::String& titleWithMarker);

    // PRD-0070: follow-playhead auto-scroll state (testable).
    FollowController&       getFollowController()       noexcept { return followController_; }
    const FollowController& getFollowController() const noexcept { return followController_; }

    //--------------------------------------------------------------------------
    // PRD-0098: external audio-file drag-drop import.
    //
    // The panel is the OS file-drop target for the arrangement. While a supported
    // file hovers it draws a monochrome drop-target highlight on the lane under
    // the cursor at the snapped drop sample; an unsupported drag is rejected
    // (no highlight, drop ignored). On drop it resolves the target lane node +
    // snapped timeline sample and forwards them to onFilesDropped.
    //--------------------------------------------------------------------------

    // Validates whether a set of dragged file paths contains importable audio
    // (delegated to AudioFileImporter's format whitelist). When unset, all
    // drags are rejected. Message thread.
    std::function<bool (const juce::StringArray& files)> isImportableFiles;

    // Whether grid-snap is currently enabled for placement (mirrors the clip-
    // drag snap toggle). When unset, snap defaults to ON. The Cmd/Ctrl override
    // is applied on top of this at drop time.
    std::function<bool()> isSnapEnabledForImport;

    // Fired on a valid drop with the target lane node and the snapped timeline
    // start sample. The host runs the import pipeline.
    std::function<void (const juce::Array<juce::File>& files,
                        juce::ValueTree lane,
                        std::int64_t snappedSample)> onFilesDropped;

    // PRD-0098: fired when the lane context menu's "Import Audio File..." is
    // chosen, with the right-clicked lane node + snapped timeline sample. The
    // host opens the native chooser and imports at that position. When unset the
    // context-menu entry is not offered.
    std::function<void (juce::ValueTree lane, std::int64_t snappedSample)> onImportRequestedAtPoint;

    // PRD-0098: lane node + snapped sample for a menu/context import. Returns the
    // lane whose content row is at `panelLocalPoint`, or the first source lane
    // when the point is over no lane (menu-import fallback, §1.5.7). The snapped
    // timeline sample for that point is written to `snappedSampleOut`.
    juce::ValueTree laneTreeAtPanelPoint (juce::Point<int> panelLocalPoint,
                                          std::int64_t& snappedSampleOut) const;

    // PRD-0098: the first source lane of the topmost group (menu-import default
    // target when no lane is focused).
    juce::ValueTree firstLaneTree() const;

    // PRD-0098: snap a raw timeline sample to the grid honouring the current
    // snap toggle. (No Cmd override here — used by the menu/playhead path.)
    std::int64_t snapImportSample (std::int64_t rawSample) const;

    //---- juce::FileDragAndDropTarget --------------------------------------
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove  (const juce::StringArray& files, int x, int y) override;
    void fileDragExit  (const juce::StringArray& files) override;
    void filesDropped  (const juce::StringArray& files, int x, int y) override;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& event, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress& key) override;

    //--------------------------------------------------------------------------
    // PRD-0102: shared snap settings + clip selection (owned here, injected into
    // the clip stack). Exposed for tests and host wiring.
    //--------------------------------------------------------------------------
    SnapSettings&  getSnapSettings()  noexcept { return snap_; }
    ClipSelection& getClipSelection() noexcept { return selection_; }

private:
    void timerCallback() override;
    void rebuildTransform();
    bool gridChanged (const MasterGridService::GridContext& ctx) const;

    // Grouped-tracks mute/solo: a muted/solo flip anywhere in the daw branch
    // changes which lanes the compiler admits, so the published snapshot must
    // be rebuilt (the lane dimming itself is the ChannelGroupStack's own
    // observation; this listener only drives playback).
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override
    {
        if ((property == DawIDs::muted || property == DawIDs::solo)
            && recompileTrigger_ != nullptr)
            recompileTrigger_->requestRecompile();
    }
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void layoutBody();

    // PRD-0098 drag-drop helpers.
    void updateDropHighlight (int panelX, int panelY); // recompute highlight rect
    void clearDropHighlight();                          // hide highlight + repaint

    // PRD-0070 helpers.
    int  contentLeftGutter() const noexcept;       // px before the content axis
    void afterTransformChanged();                  // re-layout + repaint
    void updateNowLine();                          // reposition the shared playhead
    void applyFollowIfNeeded();                     // auto-scroll when following
    void layoutPlayhead();

    // The single, shared timeline playhead sample. Recording owns the cursor
    // while a session is armed/recording; otherwise it follows the DAW transport
    // (playback). A scrub preview overrides both. Returns -1 when there is no
    // live position (idle, stopped), which hides the playhead.
    std::int64_t activePlayheadSample() const;

    // LCD helpers: bar.beat for the current playhead + the live tempo string.
    juce::String computeLcdPosition() const;
    juce::String computeLcdTempo() const;
    void         refreshLcd();                 // repaint the LCD cell on change

    // PRD-0102 ruler scrubbing helpers.
    bool         isInRulerBand (juce::Point<int> panelPoint) const noexcept;
    std::int64_t timelineSampleAtPanelX (int panelX, bool bypass) const; // snapped
    void         finalizeScrub();                  // commit the parked seek

    // PRD-0102: true when a panel-local point lands on a ClipBlock (used by the
    // interaction overlay's hitTest so clip clicks fall through to the clip).
    bool         isPointOverClip (juce::Point<int> panelPoint);

    // Transparent input surface over the content area (ruler + body): it forwards
    // pan/zoom/pinch gestures to the panel so they work even above the body
    // viewport, while leaving the now-line (drawn above it) non-interactive.
    class InteractionLayer final : public juce::Component
    {
    public:
        InteractionLayer() { setInterceptsMouseClicks (true, true); }

        std::function<void (const juce::MouseEvent&)> onDown, onDrag, onUp;
        std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;
        std::function<void (const juce::MouseEvent&, float)> onMagnify;

        // PRD-0102: return true at points where the click should fall THROUGH to
        // the component beneath (a ClipBlock), so clip drag/trim/context work.
        // Empty lane areas and the ruler stay opaque so this layer keeps handling
        // pan / zoom / scrub. Without this the overlay shadowed every clip.
        std::function<bool (juce::Point<int>)> shouldPassThrough;

        bool hitTest (int x, int y) override
        {
            if (shouldPassThrough && shouldPassThrough ({ x, y }))
                return false; // transparent here -> clip beneath receives the event
            return true;
        }

        void mouseDown (const juce::MouseEvent& e) override { if (onDown) onDown (e); }
        void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (e); }
        void mouseUp   (const juce::MouseEvent& e) override { if (onUp)   onUp (e); }
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
            { if (onWheel) onWheel (e, w); }
        void mouseMagnify (const juce::MouseEvent& e, float s) override { if (onMagnify) onMagnify (e, s); }
    };

    MasterGridService& gridService_;
    juce::ValueTree    dawBranch_;               // retained copy for EditCommandDispatcher
    juce::UndoManager  undoManager_;             // owned; EditCommandDispatcher delegates here
    TimelineTransform  transform_;

    // PRD-0093: the automation data model over the same daw branch the stack
    // observes (constructed BEFORE the stack so it can be passed in).
    AutomationModel    automationModel_;

    TimeRuler          ruler_;
    juce::Viewport     bodyViewport_;
    ChannelGroupStack  stack_;

    // PRD-0093: master tempo automation lane, revealed beneath the body.
    std::unique_ptr<ContinuousAutomationLaneView> masterTempoLane_;
    bool                 masterAutoRevealed_ { false };
    juce::Rectangle<int> masterAutoBounds_;     // header "M.AUTO" disclosure
    InteractionLayer   interaction_;
    Playhead           playhead_;
    FollowController   followController_;

    std::function<std::int64_t()> nowLineProvider_;
    std::function<RecordUiState()> recordStateProvider_;
    std::function<std::int64_t()>  recordPlayheadProvider_;
    RecordUiState                  lastRecordState_ { RecordUiState::Idle };

    // Metronome (testing aid) on/off, owned here; the host is notified via
    // onMetronomeToggle and renders the active/inactive fill from this flag.
    bool metronomeOn_ { false };

    bool expanded_ { true };
    bool fullSize_ { false };

    // PRD-0096: current session indicator drawn in the header (already carries
    // the trailing dirty dot when applicable). Empty => show "ARRANGEMENT".
    juce::String sessionTitle_;

    juce::Rectangle<int> toggleBounds_;        // collapse / expand fold
    juce::Rectangle<int> fullScreenBounds_;    // full-size view toggle
    juce::Rectangle<int> recordButtonBounds_;  // global record arm/stop
    juce::Rectangle<int> playBounds_;          // PRD-0082: DAW play
    juce::Rectangle<int> pauseBounds_;         // PRD-0082: DAW pause
    juce::Rectangle<int> stopBounds_;          // PRD-0082: DAW stop (to start)
    juce::Rectangle<int> loopBounds_;          // PRD-0082: loop-arm toggle
    juce::Rectangle<int> metroBounds_;         // metronome icon toggle
    juce::Rectangle<int> snapBounds_;          // consolidated snap dropdown
    juce::Rectangle<int> lcdBounds_;           // central LCD (position + tempo)

    // Last LCD strings, so the 30 Hz timer repaints the LCD only on change.
    juce::String lcdPosition_ { "1.1" };
    juce::String lcdTempo_    { "---" };

    // PRD-0082: DawTransport owned here so the buttons work without external wiring.
    std::unique_ptr<Daw::DawTransport>            transport_;

    // PRD-0079/0083: Arrangement compiler/publisher/recompile-trigger owned here.
    // SonikApplication reads the publisher reference to wire the AudioEngine.
    Daw::ArrangementPublisher                     arrangementPublisher_;
    std::unique_ptr<Daw::ArrangementRecompileTrigger> recompileTrigger_;

    // PRD-0083/0084/0085/0086: EditCommandDispatcher owned here.
    std::unique_ptr<Daw::EditCommandDispatcher>   dispatcher_;

    // Drag-to-pan state (content area horizontal pan).
    bool         dragging_       { false };
    int          dragLastX_      { 0 };

    // PRD-0102: grid snap settings + single-clip selection, shared (by pointer)
    // with every ClipBlock through the stack plumbing.
    SnapSettings  snap_;
    ClipSelection selection_;

    // PRD-0102: ruler-scrub state. While scrubbing_ the playhead line follows the
    // cursor live (visual only); the authoritative transport seek is committed on
    // mouse-up (§1.5.1: no per-move audio re-prime while playing).
    bool          scrubbing_   { false };
    std::int64_t  scrubSample_ { 0 };

    // PRD-0098: external-file drop highlight state. dropActive_ is true while a
    // supported file hovers; dropHighlight_ is the lane-row rectangle (panel-
    // local) with a 2px ink marker at the snapped drop sample.
    bool                 dropActive_    { false };
    juce::Rectangle<int> dropHighlight_;            // lane row band (panel-local)
    int                  dropMarkerX_   { -1 };     // snapped drop-sample x (panel-local)

    // Last-seen grid signature for change-gating the timer.
    double       lastBpm_         { -1.0 };
    double       lastSamplesPerBeat_ { -1.0 };
    std::int64_t lastPhaseOrigin_ { -1 };
    bool         lastIsPlaying_   { false };

    // Last now-line sample, so the timer only re-lays-out growing clips while
    // playback is actually advancing the now-line (no idle layout work).
    std::int64_t lastNowLineSample_ { std::numeric_limits<std::int64_t>::min() };

    static inline const juce::Colour kInk           { 0xFF2D2D2D }; // primary
    static inline const juce::Colour kSurface       { 0xFFFDFDFD }; // surface
    static inline const juce::Colour kHeaderBg      { 0xFFE2E2E2 }; // container-highest
    static inline const juce::Colour kCanvasBg      { 0xFFF3F3F4 }; // container-low

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawPanel)
};

} // namespace Daw
