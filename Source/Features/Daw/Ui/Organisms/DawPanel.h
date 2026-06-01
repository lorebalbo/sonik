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

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Molecules/TimeRuler.h"
#include "../Atoms/Playhead.h"
#include "../Atoms/RecordPlayhead.h"
#include "../FollowController.h"
#include "../DawLayoutMetrics.h"
#include "ChannelGroupStack.h"
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
                       private juce::Timer
{
public:
    //--------------------------------------------------------------------------
    // Layout metrics. The expanded panel is header + ruler + a scrollable
    // channel-group body sized to show one full channel group plus a peek of
    // the next (the body scrolls vertically when groups overflow, PRD-0067).
    //--------------------------------------------------------------------------
    static constexpr int kHeaderHeight = 28;
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
              ClipBlock::WaveformSource waveformSource = {});
    ~DawPanel() override;

    //--------------------------------------------------------------------------
    // Collapse / expand state.
    //--------------------------------------------------------------------------
    bool isExpanded() const noexcept { return expanded_; }
    void setExpanded (bool shouldBeExpanded);

    // The height the parent should give this panel in the current state.
    int  getPreferredHeight() const noexcept
    {
        return expanded_ ? kExpandedHeight : kCollapsedHeight;
    }

    // Invoked when the preferred height changes so the parent can reflow.
    std::function<void()> onPreferredHeightChanged;

    // PRD-0070: source of the live now-line sample (the bridge's now-line).
    // When unset the now-line is hidden.
    void setNowLineProvider (std::function<std::int64_t()> provider);

    //--------------------------------------------------------------------------
    // PRD-0082: DAW transport control callbacks.
    // Wire these to a DawTransport instance (owned by the SonikApplication or AudioEngine).
    std::function<void()> onTransportPlay;
    std::function<void()> onTransportPause;
    std::function<void()> onTransportStop;
    std::function<void()> onTransportLoopToggle;

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

    // Polled for the live record playhead sample; the record playhead band is
    // drawn at this position while armed/recording and hidden when idle.
    void setRecordPlayheadProvider (std::function<std::int64_t()> provider);

    // Access to the owned transform (PRD-0067+ interaction lives on the panel).
    TimelineTransform&       getTransform()       noexcept { return transform_; }
    const TimelineTransform& getTransform() const noexcept { return transform_; }

    // PRD-0070: follow-playhead auto-scroll state (testable).
    FollowController&       getFollowController()       noexcept { return followController_; }
    const FollowController& getFollowController() const noexcept { return followController_; }

    void resized() override;
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& event, float scaleFactor) override;

private:
    void timerCallback() override;
    void rebuildTransform();
    bool gridChanged (const MasterGridService::GridContext& ctx) const;

    void layoutBody();

    // PRD-0070 helpers.
    int  contentLeftGutter() const noexcept;       // px before the content axis
    void afterTransformChanged();                  // re-layout + repaint
    void updateNowLine();                          // reposition the now-line
    void updateRecordPlayhead();                   // reposition the record playhead
    void applyFollowIfNeeded();                     // auto-scroll when following
    void layoutPlayhead();

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
    TimeRuler          ruler_;
    juce::Viewport     bodyViewport_;
    ChannelGroupStack  stack_;
    InteractionLayer   interaction_;
    Playhead           playhead_;
    RecordPlayhead     recordPlayhead_;
    FollowController   followController_;

    std::function<std::int64_t()> nowLineProvider_;
    std::function<RecordUiState()> recordStateProvider_;
    std::function<std::int64_t()>  recordPlayheadProvider_;
    RecordUiState                  lastRecordState_ { RecordUiState::Idle };

    bool expanded_ { true };

    juce::Rectangle<int> toggleBounds_;        // collapse / expand
    juce::Rectangle<int> followToggleBounds_;  // follow-playhead
    juce::Rectangle<int> recordButtonBounds_;  // global record arm/stop
    juce::Rectangle<int> playBounds_;          // PRD-0082: DAW play
    juce::Rectangle<int> pauseBounds_;         // PRD-0082: DAW pause
    juce::Rectangle<int> stopBounds_;          // PRD-0082: DAW stop
    juce::Rectangle<int> loopBounds_;          // PRD-0082: loop-arm toggle

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
