//==============================================================================
// PRD-0066: DawPanel organism implementation.
//==============================================================================

#include "DawPanel.h"

#include "../Atoms/PixelIcons.h"

#include <cmath>
#include <cstdint>

namespace Daw
{

DawPanel::DawPanel (MasterGridService& gridService,
                    juce::ValueTree dawBranch,
                    ChannelGroupStack::DeckResolver deckResolver,
                    ClipBlock::WaveformSource waveformSource,
                    ClipBlock::NameSource clipNameSource)
    : gridService_ (gridService),
      dawBranch_   (dawBranch),  // retained copy (ValueTree ref-counts internally)
      transform_ (TimelineTransform::GridSnapshot{}, kDefaultPixelsPerBeat,
                  /*leftEdgeSample*/ 0, /*viewportWidthPx*/ 0.0),
      automationModel_ (dawBranch), // same daw branch the stack/ruler observe
      ruler_ (gridService, transform_),
      stack_ (dawBranch, transform_, std::move (deckResolver),
              std::move (waveformSource), &automationModel_,
              std::move (clipNameSource))
{
    addAndMakeVisible (ruler_);

    // PRD-0093: master tempo automation lane (owner "master"), hidden by default.
    auto tempoLaneNode = automationModel_.getOrCreateContinuousLane ("master", "tempo").getState();
    masterTempoLane_ = std::make_unique<ContinuousAutomationLaneView> (
        tempoLaneNode, transform_, &automationModel_, "tempo");
    masterTempoLane_->setVisible (false);
    addChildComponent (*masterTempoLane_);

    bodyViewport_.setViewedComponent (&stack_, false);
    bodyViewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (bodyViewport_);

    // Transparent gesture surface over the content area; forwards to the panel
    // handlers in panel coordinates so header toggles still work normally.
    interaction_.onDown    = [this] (const juce::MouseEvent& e) { mouseDown (e.getEventRelativeTo (this)); };
    interaction_.onDrag    = [this] (const juce::MouseEvent& e) { mouseDrag (e.getEventRelativeTo (this)); };
    interaction_.onUp      = [this] (const juce::MouseEvent& e) { dragging_ = false; finalizeScrub(); juce::ignoreUnused (e); };
    interaction_.onWheel   = [this] (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
                             { mouseWheelMove (e.getEventRelativeTo (this), w); };
    interaction_.onMagnify = [this] (const juce::MouseEvent& e, float s)
                             { mouseMagnify (e.getEventRelativeTo (this), s); };
    // PRD-0102: let clicks fall through to a clip when the cursor is over one, so
    // clip drag/trim/context menu work; the overlay still owns empty-area pan and
    // ruler scrub. The point arrives in interaction-local space.
    interaction_.shouldPassThrough = [this] (juce::Point<int> p)
    {
        return isPointOverClip (getLocalPoint (&interaction_, p));
    };
    addAndMakeVisible (interaction_);

    // The single shared playhead overlay sits above the ruler + body and never
    // eats input. It serves both playback (now-line) and recording.
    addAndMakeVisible (playhead_);

    stack_.onContentHeightChanged = [this]() { layoutBody(); };

    rebuildTransform();

    // PRD-0082: Create the owned DawTransport and wire transport button callbacks
    // so buttons work immediately without external wiring.
    transport_ = std::make_unique<Daw::DawTransport>();
    transport_->onStateChanged = [this] (Daw::DawTransport::State) { repaint(); };

    isTransportPlaying = [this]() -> bool {
        return transport_->getState() == Daw::DawTransport::State::Playing;
    };
    isTransportPaused = [this]() -> bool {
        return transport_->getState() == Daw::DawTransport::State::Paused;
    };
    isTransportLoopEnabled = [this]() -> bool {
        return transport_->isLoopEnabled();
    };

    onTransportPlay  = [this]() { transport_->play();  repaint(); };
    onTransportPause = [this]() { transport_->pause(); repaint(); };

    // Stop returns the playhead to the beginning (Logic-style): the transport
    // resets its playhead, and the view scrolls back to the arrangement start so
    // the DJ is looking at where the next Play will land.
    onTransportStop  = [this]()
    {
        transport_->stop();
        transform_.setLeftEdgeSample (transform_.minLeftEdgeSample());
        followController_.setEnabled (true);
        afterTransformChanged();
    };
    onTransportLoopToggle = [this]() { transport_->toggleLoop(); repaint(); };

    // PRD-0079/0083: Create the arrangement compiler / recompile trigger / dispatcher.
    // The compiler uses a null handle resolver for now (clips reference files by
    // sourceFileId; the resolver is wired in by SonikApplication when audio loads).
    recompileTrigger_ = std::make_unique<Daw::ArrangementRecompileTrigger> (
        dawBranch_,
        Daw::ArrangementCompiler (nullptr /*handleResolver*/),
        arrangementPublisher_);

    // EPIC-0010: when playback starts or the user seeks, re-prime the clip
    // streamers so they align to the (new) playhead. The playback resolver
    // computes per-clip source offsets from the transport's current position,
    // so a recompile is the single unified path that realigns priming.
    transport_->onStateChanged = [this] (Daw::DawTransport::State s)
    {
        if (s == Daw::DawTransport::State::Playing && recompileTrigger_ != nullptr)
            recompileTrigger_->requestRecompile();

        // Logic-style "catch on play": starting playback re-engages the default
        // follow behaviour even after a manual scroll disengaged it.
        if (s == Daw::DawTransport::State::Playing)
        {
            followController_.setEnabled (true);
            applyFollowIfNeeded();
        }
        repaint();
    };
    transport_->onSeeked = [this] (std::int64_t)
    {
        if (recompileTrigger_ != nullptr)
            recompileTrigger_->requestRecompile();
        repaint();
    };

    dispatcher_ = std::make_unique<Daw::EditCommandDispatcher> (
        dawBranch_, undoManager_, *recompileTrigger_);

    // Wire the dispatcher into all existing lane views.
    stack_.setEditDispatcher (dispatcher_.get());

    // PRD-0102: share the snap settings + selection model with every clip, and
    // route file-import placement through the same snap toggle/granularity.
    stack_.setClipInteraction (&snap_, &selection_);
    isSnapEnabledForImport = [this]() { return snap_.enabled; };

    // PRD-0102: accept keyboard focus so a Delete keystroke after a ruler click
    // (which parks the playhead but leaves the clip selected, §1.5.5) still
    // deletes the selected clip via keyPressed().
    setWantsKeyboardFocus (true);

    // PRD-0094: the master tempo automation lane edits through the same shared
    // command layer, so it needs the dispatcher as well.
    if (masterTempoLane_ != nullptr)
        masterTempoLane_->setEditDispatcher (dispatcher_.get());

    // ~30 Hz: cheap enough for a smooth now-line, still change-gated for the
    // transform rebuild so an idle panel does no layout work.
    startTimerHz (30);
}

DawPanel::~DawPanel()
{
    stopTimer();
}

void DawPanel::setExpanded (bool shouldBeExpanded)
{
    if (expanded_ == shouldBeExpanded)
        return;

    expanded_ = shouldBeExpanded;
    resized();
    repaint();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void DawPanel::setFullSize (bool shouldBeFullSize)
{
    if (fullSize_ == shouldBeFullSize)
        return;

    fullSize_ = shouldBeFullSize;

    // Full-size only makes sense with the timeline visible.
    if (fullSize_ && ! expanded_)
    {
        expanded_ = true;
        resized();
    }

    repaint();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void DawPanel::setMasterAutomationRevealed (bool shouldBeRevealed)
{
    if (masterAutoRevealed_ == shouldBeRevealed)
        return;

    masterAutoRevealed_ = shouldBeRevealed;
    resized();
    repaint();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void DawPanel::setNowLineProvider (std::function<std::int64_t()> provider)
{
    nowLineProvider_ = std::move (provider);

    // PRD-0093: the automation read-only playhead indicator rides the same
    // now-line sample. Suppressed (-1) when there is no provider.
    auto playheadProvider = [this]() -> std::int64_t
    {
        return nowLineProvider_ ? nowLineProvider_() : (std::int64_t) -1;
    };
    stack_.setAutomationPlayheadProvider (playheadProvider);
    if (masterTempoLane_ != nullptr)
        masterTempoLane_->setPlayheadProvider (playheadProvider);

    updateNowLine();
}

void DawPanel::setRecordStateProvider (std::function<RecordUiState()> provider)
{
    recordStateProvider_ = std::move (provider);
    updateNowLine();
    repaint();
}

void DawPanel::setRecordPlayheadProvider (std::function<std::int64_t()> provider)
{
    recordPlayheadProvider_ = std::move (provider);
    updateNowLine();
}

int DawPanel::contentLeftGutter() const noexcept
{
    return DawLayout::kTrackHeaderWidth;
}

bool DawPanel::gridChanged (const MasterGridService::GridContext& ctx) const
{
    return ! juce::exactlyEqual (ctx.bpm, lastBpm_)
        || ! juce::exactlyEqual (ctx.samplesPerBeat, lastSamplesPerBeat_)
        || ctx.phaseOriginSample != lastPhaseOrigin_
        || ctx.isPlaying         != lastIsPlaying_;
}

void DawPanel::rebuildTransform()
{
    const auto ctx  = gridService_.snapshotGrid();
    const auto grid = TimelineTransform::GridSnapshot::fromContext (ctx);

    const double contentWidth =
        static_cast<double> (juce::jmax (0, getWidth() - contentLeftGutter()));

    // PRD-0070: preserve the user's current zoom + scroll across a grid rebuild
    // (a tempo/phase change must not yank the view back to defaults).
    const double ppb      = transform_.getPixelsPerBeat();
    const auto   leftEdge = transform_.getLeftEdgeSample();

    // The arrangement extends a few bars past the live playhead (playback OR
    // recording) so there is room to scroll ahead of it.
    const std::int64_t active = activePlayheadSample();
    const std::int64_t nowLine = active >= 0 ? active : grid.phaseOriginSample;
    const std::int64_t trailingMargin =
        static_cast<std::int64_t> (grid.samplesPerBeat * DawState::kBeatsPerBar * 4);
    const std::int64_t contentEnd = nowLine + trailingMargin;

    transform_ = TimelineTransform (grid, ppb, leftEdge, contentWidth, contentEnd);

    lastBpm_            = ctx.bpm;
    lastSamplesPerBeat_ = ctx.samplesPerBeat;
    lastPhaseOrigin_    = ctx.phaseOriginSample;
    lastIsPlaying_      = ctx.isPlaying;

    afterTransformChanged();
}

void DawPanel::afterTransformChanged()
{
    stack_.refreshClipLayout();
    stack_.refreshAutomationTransform();
    if (masterTempoLane_ != nullptr)
        masterTempoLane_->refreshTransform();
    ruler_.refresh();
    updateNowLine();
    repaint();
}

//==============================================================================
// PRD-0096: view-state capture / restore.
//
// The schema persists the horizontal zoom as samples-per-pixel (resolution-
// independent: it does not depend on the viewport width or the current tempo's
// samples-per-beat the way pixelsPerBeat does). We therefore convert at the
// transform boundary:  samplesPerPixel = samplesPerBeat / pixelsPerBeat.
//==============================================================================
double DawPanel::captureViewZoomSamplesPerPixel() const
{
    const double samplesPerBeat = transform_.grid().samplesPerBeat;
    const double pixelsPerBeat   = transform_.getPixelsPerBeat();
    if (pixelsPerBeat <= 0.0)
        return 0.0;
    return samplesPerBeat / pixelsPerBeat;
}

void DawPanel::restoreViewState (std::optional<double>       zoomSamplesPerPixel,
                                 std::optional<std::int64_t> scrollStartSample)
{
    const double samplesPerBeat = transform_.grid().samplesPerBeat;

    // --- Zoom: convert samples-per-pixel back to pixels-per-beat, then clamp
    // through the transform (which enforces kMin/kMaxPixelsPerBeat). When the
    // persisted value is absent or non-positive, fall back to a fit-to-width
    // default rather than applying garbage (§1.5.5).
    bool zoomApplied = false;
    if (zoomSamplesPerPixel.has_value() && *zoomSamplesPerPixel > 0.0 && samplesPerBeat > 0.0)
    {
        const double targetPpb = samplesPerBeat / *zoomSamplesPerPixel;
        if (std::isfinite (targetPpb) && targetPpb > 0.0)
        {
            transform_.setPixelsPerBeat (targetPpb);
            zoomApplied = true;
        }
    }

    if (! zoomApplied)
        transform_.setPixelsPerBeat (kDefaultPixelsPerBeat); // fit-to-width default

    // --- Scroll: the transform clamps the left edge to its scroll bounds, so an
    // out-of-range persisted value collapses harmlessly to the nearest valid
    // edge; absence falls back to scroll-start (the min left edge).
    if (scrollStartSample.has_value())
        transform_.setLeftEdgeSample (*scrollStartSample);
    else
        transform_.setLeftEdgeSample (transform_.minLeftEdgeSample());

    afterTransformChanged();
}

void DawPanel::setSessionTitle (const juce::String& titleWithMarker)
{
    if (sessionTitle_ == titleWithMarker)
        return;
    sessionTitle_ = titleWithMarker;
    repaint();
}

//==============================================================================
// LCD (position + tempo). The position is the bar.beat of the live playhead —
// the scrub preview while scrubbing, else the transport now-line, else bar 1.
//==============================================================================
juce::String DawPanel::computeLcdPosition() const
{
    const std::int64_t sample = activePlayheadSample();

    const auto&  grid = transform_.grid();
    double beats = 0.0;
    if (sample >= 0 && grid.samplesPerBeat > 0.0)
        beats = static_cast<double> (sample - grid.phaseOriginSample) / grid.samplesPerBeat;

    const int bar  = juce::jmax (1, (int) std::floor (beats / DawState::kBeatsPerBar) + 1);
    const int beat = juce::jmax (1, (int) std::floor (std::fmod (juce::jmax (0.0, beats),
                                                                 (double) DawState::kBeatsPerBar)) + 1);
    return juce::String (bar) + "." + juce::String (beat);
}

juce::String DawPanel::computeLcdTempo() const
{
    const double bpm = transform_.grid().bpm;
    if (bpm <= 0.0)
        return "---";
    return juce::String (bpm, 1);
}

void DawPanel::refreshLcd()
{
    auto position = computeLcdPosition();
    auto tempo    = computeLcdTempo();
    if (position == lcdPosition_ && tempo == lcdTempo_)
        return;

    lcdPosition_ = std::move (position);
    lcdTempo_    = std::move (tempo);
    repaint (lcdBounds_);
}

std::int64_t DawPanel::activePlayheadSample() const
{
    // PRD-0102: while the DJ is scrubbing the ruler, the playhead follows the
    // cursor live (scrubSample_) regardless of transport/recording — the
    // authoritative seek is committed on mouse-up (§1.5.1).
    if (scrubbing_)
        return scrubSample_;

    // Recording owns the single shared playhead while a session is armed or
    // recording, so the same bar marks where capture is writing.
    if (recordStateProvider_ && recordStateProvider_() != RecordUiState::Idle
        && recordPlayheadProvider_)
        return recordPlayheadProvider_();

    // Otherwise the playhead tracks the DAW transport (the now-line provider
    // returns -1 when stopped, which hides the playhead).
    if (nowLineProvider_)
        return nowLineProvider_();

    return (std::int64_t) -1;
}

void DawPanel::updateNowLine()
{
    const std::int64_t sample = activePlayheadSample();
    if (sample < 0)
    {
        playhead_.setLineX (-1);
        return;
    }

    const double x = transform_.sampleToX (sample);
    playhead_.setLineX ((int) std::llround (TimelineTransform::alignToPixelGrid (x)));
}

void DawPanel::applyFollowIfNeeded()
{
    // A ruler scrub is a manual gesture: the DJ drives the cursor, so the view
    // must not auto-chase it (clicking near the right edge would yank the view).
    if (scrubbing_ || ! followController_.isEnabled())
        return;

    // Follow whatever currently drives the shared playhead — playback OR
    // recording. Without a live position there is nothing to chase.
    const std::int64_t sample = activePlayheadSample();
    if (sample < 0)
        return;

    const double viewportWidth = transform_.getViewportWidth();
    const double nowLineX = transform_.sampleToX (sample);

    if (! followController_.shouldFollow (nowLineX, viewportWidth))
        return;

    // Pin the playhead to the re-anchor (4/5) fraction of the viewport. Because
    // trigger == re-anchor, each tick only corrects the small distance playback
    // advanced, giving a smooth, continuous follow.
    const double target = FollowController::reanchorTargetX (viewportWidth);
    transform_.scrollByX (nowLineX - target);
    afterTransformChanged();
}

//==============================================================================
// PRD-0102: ruler scrubbing + selection-delete.
//==============================================================================
bool DawPanel::isInRulerBand (juce::Point<int> p) const noexcept
{
    if (! expanded_)
        return false;
    const int top    = kHeaderHeight;
    const int bottom = kHeaderHeight + TimeRuler::kRulerHeight;
    return p.getY() >= top && p.getY() < bottom && p.getX() >= contentLeftGutter();
}

std::int64_t DawPanel::timelineSampleAtPanelX (int panelX, bool bypass) const
{
    const int contentX = panelX - contentLeftGutter();
    const std::int64_t raw = transform_.xToSample (static_cast<double> (juce::jmax (0, contentX)));
    return snap_.snap (juce::jmax ((std::int64_t) 0, raw), transform_, bypass);
}

void DawPanel::finalizeScrub()
{
    if (! scrubbing_)
        return;

    scrubbing_ = false;
    if (transport_ != nullptr)
        transport_->seek (scrubSample_); // commit the parked / play-from-here position
    updateNowLine();
    repaint();
}

bool DawPanel::isPointOverClip (juce::Point<int> panelPoint)
{
    // The ruler / header is never "over a clip" (scrubbing owns the ruler).
    if (panelPoint.getY() < kHeaderHeight + TimeRuler::kRulerHeight)
        return false;

    const auto stackPoint = stack_.getLocalPoint (this, panelPoint);
    for (auto* hit = stack_.getComponentAt (stackPoint); hit != nullptr; hit = hit->getParentComponent())
    {
        if (dynamic_cast<ClipBlock*> (hit) != nullptr)
            return true;
        if (hit == &stack_)
            break;
    }
    return false;
}

bool DawPanel::keyPressed (const juce::KeyPress& key)
{
    // PRD-0102: Delete / Backspace removes the selected clip (the §1.5.5 path
    // that works even when focus left the clip, e.g. after a ruler scrub).
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        const juce::String id = selection_.selectedId();
        if (id.isNotEmpty() && dispatcher_ != nullptr)
        {
            selection_.clear();
            dispatcher_->deleteClip (id);
            return true;
        }
    }

    //--------------------------------------------------------------------------
    // Contextual DAW transport shortcuts. These fire only while the DAW panel
    // owns keyboard focus (any click in the panel grabs it), so the same keys
    // keep their deck meaning when the DJ is working elsewhere — the deck
    // handler in MainContentComponent stays the fallback.
    //--------------------------------------------------------------------------
    const auto mods = key.getModifiers();
    if (! mods.isCommandDown() && ! mods.isCtrlDown() && ! mods.isAltDown())
    {
        // Space — play/pause toggle.
        if (key == juce::KeyPress::spaceKey)
        {
            const bool playing = isTransportPlaying && isTransportPlaying();
            if (playing) { if (onTransportPause) onTransportPause(); }
            else         { if (onTransportPlay)  onTransportPlay();  }
            repaint();
            return true;
        }

        // Return — stop and send the playhead back to the beginning.
        if (key == juce::KeyPress::returnKey)
        {
            if (onTransportStop)
                onTransportStop();
            repaint();
            return true;
        }

        const auto ch = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

        // R — record arm/stop toggle.
        if (ch == 'r')
        {
            if (onRecordToggle)
                onRecordToggle();
            repaint();
            return true;
        }

        // M — metronome on/off.
        if (ch == 'm')
        {
            metronomeOn_ = ! metronomeOn_;
            if (onMetronomeToggle)
                onMetronomeToggle (metronomeOn_);
            repaint();
            return true;
        }
    }

    return false;
}

void DawPanel::timerCallback()
{
    const auto ctx = gridService_.snapshotGrid();
    if (gridChanged (ctx))
        rebuildTransform();

    // The single shared playhead sample (recording position while recording,
    // else the transport now-line; -1 when there is no live position).
    const std::int64_t active = activePlayheadSample();

    if (active >= 0)
    {
        // Keep the content-end at least one full viewport ahead of the live
        // playhead so the follow re-anchor always has room and long clips have
        // somewhere to grow into. This also keeps the position reachable by a
        // manual scroll/drag while recording.
        const auto grid = transform_.grid();
        const std::int64_t barMargin =
            static_cast<std::int64_t> (grid.samplesPerBeat * DawState::kBeatsPerBar * 4);
        const std::int64_t viewportSamples =
            static_cast<std::int64_t> (transform_.getViewportWidth()
                                       * grid.samplesPerBeat
                                       / juce::jmax (1.0, transform_.getPixelsPerBeat()));
        transform_.setContentEndSample (active + juce::jmax (barMargin, viewportSamples));
    }

    // The clip blocks resize themselves as their crop end grows (ClipBlock
    // listens to its own ValueTree), so here we only need to keep the playhead
    // in step — no full relayout/repaint per tick.
    lastNowLineSample_ = active;
    updateNowLine();

    // Keep the LCD's bar.beat position + tempo readout live (repaints only the
    // LCD cell, and only when the displayed strings actually change).
    refreshLcd();

    // Repaint the header when the record lifecycle changes so the Record button
    // reflects idle / armed / recording.
    if (recordStateProvider_)
    {
        const auto recState = recordStateProvider_();
        if (recState != lastRecordState_)
        {
            lastRecordState_ = recState;
            repaint();
        }
    }

    applyFollowIfNeeded();
}

void DawPanel::resized()
{
    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop (kHeaderHeight);

    //--------------------------------------------------------------------------
    // Control bar, Logic-style: [ title block over the track-header gutter ]
    // [ segmented transport | LCD | cycle+metronome, centred as one group ]
    // [ snap dropdown + tempo-lane disclosure + view toggles on the right ].
    // Transport buttons share their 2-px borders (a segmented cluster) so the
    // bar reads as one engineered unit instead of floating chips.
    //--------------------------------------------------------------------------
    const int btn   = 30;                      // square button side
    const int y     = header.getY() + (kHeaderHeight - btn) / 2;
    const int step  = btn - 2;                 // segmented: borders coincide

    // Right cluster (right -> left): collapse fold + full-size (segmented pair),
    // snap dropdown, master tempo automation disclosure.
    toggleBounds_     = { header.getRight() - btn - 8, y, btn, btn };
    fullScreenBounds_ = { toggleBounds_.getX() - step, y, btn, btn };

    const int snapW = 100;
    snapBounds_       = { fullScreenBounds_.getX() - snapW - 14, y, snapW, btn };

    const int masterAutoW = 58;
    masterAutoBounds_ = { snapBounds_.getX() - masterAutoW + 2, y, masterAutoW, btn };

    // Centre group: [PLAY|PAUSE|STOP|REC]  LCD  [LOOP|METRO].
    const int lcdW       = 200;
    const int transportW = 4 * step + 2;       // 4 segmented buttons
    const int pairW      = 2 * step + 2;       // loop + metronome pair
    const int gapLcd     = 10;
    const int groupW     = transportW + gapLcd + lcdW + gapLcd + pairW;

    const int minLeft = DawLayout::kTrackHeaderWidth + 12;  // clear of the title
    int groupX = juce::jmax (minLeft, (getWidth() - groupW) / 2);
    groupX     = juce::jmin (groupX, masterAutoBounds_.getX() - groupW - 16);

    playBounds_         = { groupX + 0 * step, y, btn, btn };
    pauseBounds_        = { groupX + 1 * step, y, btn, btn };
    stopBounds_         = { groupX + 2 * step, y, btn, btn };
    recordButtonBounds_ = { groupX + 3 * step, y, btn, btn };

    lcdBounds_   = { groupX + transportW + gapLcd, y, lcdW, btn };

    const int pairX = lcdBounds_.getRight() + gapLcd;
    loopBounds_  = { pairX,        y, btn, btn };
    metroBounds_ = { pairX + step, y, btn, btn };

    const int gutter = contentLeftGutter();

    // Keep the transform viewport width (content area, after the gutter) in sync.
    transform_.setViewportWidth (
        static_cast<double> (juce::jmax (0, getWidth() - gutter)));

    if (expanded_)
    {
        auto rulerRow = bounds.removeFromTop (TimeRuler::kRulerHeight);
        ruler_.setVisible (true);
        // The ruler occupies the content area to the right of the gutter.
        ruler_.setBounds (rulerRow.withTrimmedLeft (gutter));
        ruler_.refresh();

        // PRD-0093: reserve the master tempo automation lane row at the BOTTOM of
        // the body region when revealed (beneath the channel-group stack), so the
        // default layout (revealed = false) is unchanged.
        if (masterTempoLane_ != nullptr && masterAutoRevealed_)
        {
            auto masterRow = bounds.removeFromBottom (AutomationLaneMetrics::kAutomationLaneHeight);
            masterTempoLane_->setVisible (true);
            masterTempoLane_->setBounds (masterRow);
        }
        else if (masterTempoLane_ != nullptr)
        {
            masterTempoLane_->setVisible (false);
        }

        bodyViewport_.setVisible (true);
        bodyViewport_.setBounds (bounds);
        layoutBody();

        // The gesture surface + now-line overlay span the ruler + body region.
        const int gutterX = contentLeftGutter();
        const int contentTop = kHeaderHeight;
        interaction_.setVisible (true);
        interaction_.setBounds (gutterX, contentTop,
                                juce::jmax (0, getWidth() - gutterX),
                                juce::jmax (0, getHeight() - contentTop));

        playhead_.setVisible (true);
        layoutPlayhead();
    }
    else
    {
        ruler_.setVisible (false);
        bodyViewport_.setVisible (false);
        interaction_.setVisible (false);
        playhead_.setVisible (false);
        if (masterTempoLane_ != nullptr)
            masterTempoLane_->setVisible (false);
    }
}

void DawPanel::layoutPlayhead()
{
    const int gutter = contentLeftGutter();
    const int top    = kHeaderHeight;
    const int bottom = getHeight();
    playhead_.setBounds (gutter, top,
                         juce::jmax (0, getWidth() - gutter),
                         juce::jmax (0, bottom - top));
    playhead_.toFront (false);

    updateNowLine();
}

void DawPanel::layoutBody()
{
    stack_.layoutToContentHeight (juce::jmax (1, bodyViewport_.getMaximumVisibleWidth()));
}

void DawPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Whole-panel canvas fill first: in full-size view the body can be taller
    // than the channel-group stack, and that spare area must read as DAW canvas
    // (surface-container-low), never as an unpainted hole.
    g.setColour (kCanvasBg);
    g.fillRect (bounds);

    // Header strip.
    auto header = bounds.removeFromTop (kHeaderHeight);
    g.setColour (kHeaderBg);
    g.fillRect (header);

    // The control bar's 2-px ink base line — one continuous edge under the whole
    // bar makes it read as a single engineered band (Logic's control bar edge).
    g.setColour (kInk);
    g.fillRect (header.getX(), header.getBottom() - 2, header.getWidth(), 2);

    // Title block over the track-header gutter (Space Mono, ink on the light
    // header). PRD-0096: the session file name (with its trailing dirty dot)
    // sits beneath the "ARRANGEMENT" label.
    g.setColour (kInk);
    g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
    auto titleArea = header.withTrimmedLeft (12)
                           .withWidth (DawLayout::kTrackHeaderWidth - 12);
    if (sessionTitle_.isNotEmpty())
    {
        g.drawText ("ARRANGEMENT", titleArea.withTrimmedBottom (kHeaderHeight / 2 - 1),
                    juce::Justification::bottomLeft, false);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain)));
        g.drawText (sessionTitle_, titleArea.withTrimmedTop (kHeaderHeight / 2 + 1),
                    juce::Justification::topLeft, true);
    }
    else
    {
        g.drawText ("ARRANGEMENT", titleArea, juce::Justification::centredLeft, false);
    }

    // Shared DESIGN.md button frame: 2-px ink border, instant active/inactive
    // fill inversion, pixel-art glyph in the state's contrast colour.
    auto drawIconBtn = [&] (const juce::Rectangle<int>& r, bool active, auto&& icon)
    {
        g.setColour (active ? kInk : kSurface);
        g.fillRect (r);
        g.setColour (kInk);
        g.drawRect (r, 2);
        g.setColour (active ? kSurface : kInk);
        icon (g, r);
    };

    // Segmented transport cluster — PLAY / PAUSE / STOP / REC share borders.
    {
        const bool playing   = isTransportPlaying     ? isTransportPlaying()     : false;
        const bool paused    = isTransportPaused      ? isTransportPaused()      : false;
        const bool loopArmed = isTransportLoopEnabled ? isTransportLoopEnabled() : false;
        const RecordUiState rec = recordStateProvider_ ? recordStateProvider_()
                                                       : RecordUiState::Idle;

        drawIconBtn (playBounds_,  playing && ! paused,   PixelIcons::drawPlay);
        drawIconBtn (pauseBounds_, paused,                PixelIcons::drawPause);
        drawIconBtn (stopBounds_,  ! playing && ! paused, PixelIcons::drawStop);
        drawIconBtn (recordButtonBounds_, rec != RecordUiState::Idle, PixelIcons::drawRecord);

        drawIconBtn (loopBounds_,  loopArmed,    PixelIcons::drawLoop);
        drawIconBtn (metroBounds_, metronomeOn_, PixelIcons::drawMetronome);
    }

    // Central LCD — Logic's signature element, 1-bit style: an ink inset with
    // surface text, split into POSITION and TEMPO cells.
    {
        g.setColour (kInk);
        g.fillRect (lcdBounds_);

        const auto posCell   = lcdBounds_.withWidth (lcdBounds_.getWidth() / 2);
        const auto tempoCell = lcdBounds_.withTrimmedLeft (lcdBounds_.getWidth() / 2);

        g.setColour (kSurface);
        // Cell divider (1 px, surface) — an instrument-panel separation.
        g.fillRect (posCell.getRight(), lcdBounds_.getY() + 5, 1, lcdBounds_.getHeight() - 10);

        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)));
        g.drawText (lcdPosition_, posCell.withTrimmedBottom (10),
                    juce::Justification::centredBottom, false);
        g.drawText (lcdTempo_, tempoCell.withTrimmedBottom (10),
                    juce::Justification::centredBottom, false);

        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 7.0f, juce::Font::plain)));
        g.drawText ("POSITION", posCell.withTrimmedTop (lcdBounds_.getHeight() - 11),
                    juce::Justification::centredTop, false);
        g.drawText ("TEMPO", tempoCell.withTrimmedTop (lcdBounds_.getHeight() - 11),
                    juce::Justification::centredTop, false);
    }

    // Right cluster — tempo-lane disclosure, snap dropdown, view toggles.
    {
        // PRD-0093: master tempo automation disclosure (fill inversion).
        g.setColour (masterAutoRevealed_ ? kInk : kSurface);
        g.fillRect (masterAutoBounds_);
        g.setColour (kInk);
        g.drawRect (masterAutoBounds_, 2);
        g.setColour (masterAutoRevealed_ ? kSurface : kInk);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::bold)));
        g.drawText ("TEMPO", masterAutoBounds_, juce::Justification::centred, false);

        // Consolidated snap dropdown: one control showing the live snap state
        // ("SNAP:BEAT" / "SNAP:OFF") with a caret; the menu holds Off + all
        // granularities (PRD-0102 toggle + selector merged).
        const juce::String snapLabel = snap_.enabled
            ? "SNAP " + labelForGranularity (snap_.granularity)
            : juce::String ("SNAP OFF");
        g.setColour (snap_.enabled ? kInk : kSurface);
        g.fillRect (snapBounds_);
        g.setColour (kInk);
        g.drawRect (snapBounds_, 2);
        g.setColour (snap_.enabled ? kSurface : kInk);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::bold)));
        g.drawText (snapLabel, snapBounds_.withTrimmedLeft (8).withTrimmedRight (16),
                    juce::Justification::centredLeft, false);
        PixelIcons::drawDropdownCaret (g, snapBounds_.withTrimmedLeft (snapBounds_.getWidth() - 18));

        // Full-size toggle + collapse fold (segmented pair).
        drawIconBtn (fullScreenBounds_, fullSize_,
                     fullSize_ ? PixelIcons::drawShrink : PixelIcons::drawExpand);
        drawIconBtn (toggleBounds_, false,
                     expanded_ ? PixelIcons::drawChevronUp : PixelIcons::drawChevronDown);
    }

    // Gutter corner above the track headers, beside the ruler: flat header tone
    // with a single 2-px ink edge on its right (the track-header column
    // separator) and along the ruler's bottom — no boxed cell.
    if (expanded_)
    {
        auto rulerRow = bounds.removeFromTop (TimeRuler::kRulerHeight);
        auto corner   = rulerRow.withWidth (DawLayout::kTrackHeaderWidth);
        g.setColour (kHeaderBg);
        g.fillRect (corner);
        g.setColour (kInk);
        g.fillRect (corner.getRight() - 2, corner.getY(), 2, corner.getHeight());
        g.fillRect (corner.getX(), corner.getBottom() - 2, corner.getWidth(), 2);
    }
}

//==============================================================================
// PRD-0098: external audio-file drag-drop import.
//==============================================================================
void DawPanel::paintOverChildren (juce::Graphics& g)
{
    if (! dropActive_ || dropHighlight_.isEmpty())
        return;

    // Monochrome drop-target highlight (DESIGN.md): a 2-px ink border around the
    // target lane row over a sparse dithered ink wash, plus a 2-px ink marker at
    // the snapped drop sample. No colour, no radius, no gradients.
    g.setColour (kInk.withAlpha (0.10f));
    g.fillRect (dropHighlight_);

    g.setColour (kInk);
    g.drawRect (dropHighlight_, 2);

    if (dropMarkerX_ >= dropHighlight_.getX() && dropMarkerX_ <= dropHighlight_.getRight())
        g.fillRect (dropMarkerX_, dropHighlight_.getY(), 2, dropHighlight_.getHeight());
}

std::int64_t DawPanel::snapImportSample (std::int64_t rawSample) const
{
    const bool snapOn = isSnapEnabledForImport ? isSnapEnabledForImport() : snap_.enabled;
    const std::int64_t clamped = juce::jmax ((std::int64_t) 0, rawSample);
    if (! snapOn)
        return clamped;
    // PRD-0102: honour the current snap granularity so a file drop lands on the
    // same grid the clip edits use (not always the beat grid).
    const SnapSettings forced { true, snap_.granularity };
    return forced.snap (clamped, transform_);
}

juce::ValueTree DawPanel::firstLaneTree() const
{
    return stack_.firstLaneTree();
}

juce::ValueTree DawPanel::laneTreeAtPanelPoint (juce::Point<int> panelLocalPoint,
                                                std::int64_t& snappedSampleOut) const
{
    // Content-area x = panel x minus the lane-header gutter; the transform maps
    // content px -> timeline sample. Snap honours the live snap toggle.
    const int contentX = panelLocalPoint.getX() - contentLeftGutter();
    const std::int64_t raw = transform_.xToSample ((double) juce::jmax (0, contentX));
    snappedSampleOut = snapImportSample (raw);

    // Map the panel point into the channel-group stack's coordinate space (the
    // stack scrolls inside bodyViewport_), then hit-test for the lane row.
    const auto stackPoint = stack_.getLocalPoint (this, panelLocalPoint);
    auto lane = stack_.laneTreeAt (stackPoint);
    if (! lane.isValid())
        lane = stack_.firstLaneTree();   // menu-import fallback (§1.5.7)
    return lane;
}

void DawPanel::updateDropHighlight (int panelX, int panelY)
{
    // Only highlight when the cursor is over an actual lane row in the body.
    const auto stackPoint = stack_.getLocalPoint (this, juce::Point<int> (panelX, panelY));
    if (! stack_.laneTreeAt (stackPoint).isValid())
    {
        clearDropHighlight();
        return;
    }

    std::int64_t snapped = 0;
    laneTreeAtPanelPoint ({ panelX, panelY }, snapped);

    // The lane content row spans the content area (right of the gutter). Derive a
    // crisp, stable row band by quantising the pointer Y to the lane-height grid
    // measured from the body top (header + ruler), accounting for body scroll.
    const int gutter    = contentLeftGutter();
    const int rowHeight = DawLayout::kLaneHeight;
    const int bodyTop   = kHeaderHeight + TimeRuler::kRulerHeight;
    const int scrollY   = bodyViewport_.getViewPositionY();
    const int relY      = juce::jmax (0, (panelY - bodyTop) + scrollY);
    const int rowTop    = bodyTop - scrollY + (relY / rowHeight) * rowHeight;

    dropHighlight_ = juce::Rectangle<int> (gutter, rowTop,
                                           juce::jmax (0, getWidth() - gutter),
                                           rowHeight)
                         .getIntersection (juce::Rectangle<int> (0, bodyTop,
                                                                 getWidth(),
                                                                 juce::jmax (0, getHeight() - bodyTop)));
    dropMarkerX_   = gutter + (int) std::llround (transform_.sampleToX (snapped));
    dropActive_    = true;
    repaint();
}

void DawPanel::clearDropHighlight()
{
    if (! dropActive_ && dropHighlight_.isEmpty())
        return;
    dropActive_  = false;
    dropHighlight_ = {};
    dropMarkerX_ = -1;
    repaint();
}

bool DawPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    return isImportableFiles ? isImportableFiles (files) : false;
}

void DawPanel::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    if (isInterestedInFileDrag (files))
        updateDropHighlight (x, y);
}

void DawPanel::fileDragMove (const juce::StringArray& files, int x, int y)
{
    if (isInterestedInFileDrag (files))
        updateDropHighlight (x, y);
}

void DawPanel::fileDragExit (const juce::StringArray&)
{
    clearDropHighlight();
}

void DawPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    clearDropHighlight();

    if (! onFilesDropped || ! isInterestedInFileDrag (files))
        return;

    std::int64_t snapped = 0;
    const auto lane = laneTreeAtPanelPoint ({ x, y }, snapped);
    if (! lane.isValid())
        return;

    juce::Array<juce::File> audioFiles;
    for (const auto& path : files)
    {
        const juce::File f (path);
        if (f.existsAsFile())
            audioFiles.add (f);
    }

    if (! audioFiles.isEmpty())
        onFilesDropped (audioFiles, lane, snapped);
}

void DawPanel::mouseUp (const juce::MouseEvent& event)
{
    dragging_ = false;

    if (toggleBounds_.contains (event.getPosition()))
    {
        // Folding away a full-size panel returns to the integrated layout first.
        if (fullSize_)
            setFullSize (false);
        setExpanded (! expanded_);
        return;
    }

    if (fullScreenBounds_.contains (event.getPosition()))
    {
        setFullSize (! fullSize_);
        return;
    }

    if (recordButtonBounds_.contains (event.getPosition()))
    {
        if (onRecordToggle)
            onRecordToggle();
        repaint();
        return;
    }

    // PRD-0082: DAW transport controls.
    if (playBounds_.contains (event.getPosition()))
    {
        if (onTransportPlay) onTransportPlay();
        repaint();
        return;
    }
    if (pauseBounds_.contains (event.getPosition()))
    {
        if (onTransportPause) onTransportPause();
        repaint();
        return;
    }
    if (stopBounds_.contains (event.getPosition()))
    {
        if (onTransportStop) onTransportStop();
        repaint();
        return;
    }
    if (loopBounds_.contains (event.getPosition()))
    {
        if (onTransportLoopToggle) onTransportLoopToggle();
        repaint();
        return;
    }

    // Metronome (testing aid) toggle: flip the owned state and notify the host.
    if (metroBounds_.contains (event.getPosition()))
    {
        metronomeOn_ = ! metronomeOn_;
        if (onMetronomeToggle) onMetronomeToggle (metronomeOn_);
        repaint();
        return;
    }

    // PRD-0093: master tempo automation disclosure.
    if (masterAutoBounds_.contains (event.getPosition()))
    {
        setMasterAutomationRevealed (! masterAutoRevealed_);
        return;
    }

    // PRD-0102 (consolidated): the snap dropdown. One menu owns both the on/off
    // toggle and the granularity; the new state is read at the start of the next
    // edit/scrub gesture.
    if (snapBounds_.contains (event.getPosition()))
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Snap Off", true, ! snap_.enabled);
        menu.addSeparator();
        menu.addItem (2, "Bar",      true, snap_.enabled && snap_.granularity == SnapGranularity::Bar);
        menu.addItem (3, "Beat",     true, snap_.enabled && snap_.granularity == SnapGranularity::Beat);
        menu.addItem (4, "1/2 Beat", true, snap_.enabled && snap_.granularity == SnapGranularity::Half);
        menu.addItem (5, "1/4 Beat", true, snap_.enabled && snap_.granularity == SnapGranularity::Quarter);

        juce::Component::SafePointer<DawPanel> safe (this);
        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withTargetComponent (this)
                .withTargetScreenArea (localAreaToGlobal (snapBounds_)),
            [safe] (int result)
            {
                if (safe == nullptr || result == 0)
                    return;
                if (result == 1)
                    safe->snap_.enabled = false;
                else
                {
                    safe->snap_.enabled = true;
                    safe->snap_.granularity = static_cast<SnapGranularity> (result - 2);
                }
                safe->repaint();
            });
        return;
    }
}

void DawPanel::mouseDown (const juce::MouseEvent& event)
{
    // Any press inside the panel claims keyboard focus so the contextual DAW
    // shortcuts (Space / Return / R / M) target the arrangement, not the decks.
    grabKeyboardFocus();

    // Start a horizontal pan only inside the content area (right of the gutter,
    // below the header). Header clicks are handled in mouseUp (toggles).
    if (! expanded_)
        return;
    if (event.getPosition().y < kHeaderHeight)
        return;
    if (event.getPosition().x < contentLeftGutter())
        return;

    // PRD-0102: a press on the time ruler begins a playhead scrub. A click parks
    // the playhead; a press-drag moves it live (the seek is committed on
    // mouse-up, §1.5.1). Right-clicks on the ruler do nothing (no context menu).
    if (isInRulerBand (event.getPosition()))
    {
        if (event.mods.isPopupMenu())
            return;

        const bool bypass = event.mods.isCommandDown() || event.mods.isCtrlDown();
        scrubbing_   = true;
        scrubSample_ = timelineSampleAtPanelX (event.getPosition().x, bypass);
        // Grab focus so a Delete keystroke still targets the selected clip even
        // though the ruler click does not change the selection (§1.5.5).
        grabKeyboardFocus();
        updateNowLine();
        repaint();
        return;
    }

    // PRD-0098: right-click over a lane row offers an "Import Audio File..."
    // context entry, placing the clip at the (snapped) cursor position on that
    // lane. Left-clicks start a horizontal pan as before.
    if (event.mods.isPopupMenu() && onImportRequestedAtPoint != nullptr)
    {
        std::int64_t snapped = 0;
        const auto lane = laneTreeAtPanelPoint (event.getPosition(), snapped);
        if (lane.isValid())
        {
            juce::PopupMenu menu;
            menu.addItem (1, "Import Audio File...");
            menu.showMenuAsync (
                juce::PopupMenu::Options()
                    .withTargetComponent (this)
                    .withTargetScreenArea (juce::Rectangle<int> (
                        event.getScreenX(), event.getScreenY(), 1, 1)),
                [this, lane, snapped] (int result)
                {
                    if (result == 1 && onImportRequestedAtPoint != nullptr)
                        onImportRequestedAtPoint (lane, snapped);
                });
        }
        return;
    }

    dragging_  = true;
    dragLastX_ = event.getPosition().x;
}

void DawPanel::mouseDrag (const juce::MouseEvent& event)
{
    // PRD-0102: live ruler scrub — move the playhead line with the cursor. The
    // transport seek itself is committed on mouse-up (finalizeScrub).
    if (scrubbing_)
    {
        const bool bypass = event.mods.isCommandDown() || event.mods.isCtrlDown();
        scrubSample_ = timelineSampleAtPanelX (event.getPosition().x, bypass);
        updateNowLine();
        return;
    }

    if (! dragging_)
        return;

    const int x  = event.getPosition().x;
    const int dx = x - dragLastX_;
    dragLastX_   = x;

    if (dx == 0)
        return;

    // Dragging right reveals earlier content -> scroll the view left.
    transform_.scrollByX (static_cast<double> (-dx));
    followController_.notifyManualScroll();
    afterTransformChanged();
    repaint();
}

void DawPanel::mouseWheelMove (const juce::MouseEvent& event,
                               const juce::MouseWheelDetails& wheel)
{
    if (! expanded_)
        return;

    const double focusPx =
        static_cast<double> (event.getPosition().x - contentLeftGutter());

    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
    {
        // Modifier + wheel = focal zoom around the cursor.
        const double zoomFactor = wheel.deltaY > 0.0f ? 1.1 : (1.0 / 1.1);
        transform_.zoomAroundX (focusPx, zoomFactor);
    }
    else if (std::abs (wheel.deltaX) >= std::abs (wheel.deltaY))
    {
        // Horizontal-dominant wheel = scroll the timeline.
        transform_.scrollByX (-wheel.deltaX * 80.0);
    }
    else
    {
        // Vertical-dominant wheel = scroll the channel-group body instead.
        // NOTE: do NOT call bodyViewport_.mouseWheelMove() here. When the body
        // can't scroll, JUCE's Viewport bubbles the wheel event back up to its
        // parent (this panel), which would re-enter mouseWheelMove() and recurse
        // until the stack overflows. Instead move the view position directly.
        auto& vbar = bodyViewport_.getVerticalScrollBar();
        if (vbar.isVisible())
        {
            const int singleStep = juce::jmax (1, bodyViewport_.getViewHeight() / 12);
            const int delta = juce::roundToInt (wheel.deltaY * static_cast<float> (singleStep)
                                                * (wheel.isReversed ? -3.0f : 3.0f));
            bodyViewport_.setViewPosition (bodyViewport_.getViewPositionX(),
                                           bodyViewport_.getViewPositionY() - delta);
        }
        return;
    }

    followController_.notifyManualScroll();
    afterTransformChanged();
}

void DawPanel::mouseMagnify (const juce::MouseEvent& event, float scaleFactor)
{
    if (! expanded_ || scaleFactor <= 0.0f)
        return;

    const double focusPx =
        static_cast<double> (event.getPosition().x - contentLeftGutter());
    transform_.zoomAroundX (focusPx, static_cast<double> (scaleFactor));
    followController_.notifyManualScroll();
    afterTransformChanged();
}

} // namespace Daw
