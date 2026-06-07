//==============================================================================
// PRD-0066: DawPanel organism implementation.
//==============================================================================

#include "DawPanel.h"

#include <cmath>
#include <cstdint>

namespace Daw
{

DawPanel::DawPanel (MasterGridService& gridService,
                    juce::ValueTree dawBranch,
                    ChannelGroupStack::DeckResolver deckResolver,
                    ClipBlock::WaveformSource waveformSource)
    : gridService_ (gridService),
      dawBranch_   (dawBranch),  // retained copy (ValueTree ref-counts internally)
      transform_ (TimelineTransform::GridSnapshot{}, kDefaultPixelsPerBeat,
                  /*leftEdgeSample*/ 0, /*viewportWidthPx*/ 0.0),
      automationModel_ (dawBranch), // same daw branch the stack/ruler observe
      ruler_ (gridService, transform_),
      stack_ (dawBranch, transform_, std::move (deckResolver),
              std::move (waveformSource), &automationModel_)
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

    // The now-line overlay sits above the ruler + body and never eats input.
    addAndMakeVisible (playhead_);

    // The record playhead band sits above the now-line; also input-transparent.
    addAndMakeVisible (recordPlayhead_);

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
    onTransportStop  = [this]() { transport_->stop();  repaint(); };
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
    repaint();
}

void DawPanel::setRecordPlayheadProvider (std::function<std::int64_t()> provider)
{
    recordPlayheadProvider_ = std::move (provider);
    updateRecordPlayhead();
}

int DawPanel::contentLeftGutter() const noexcept
{
    return DawLayout::kTrackHeaderWidth;
}

bool DawPanel::gridChanged (const MasterGridService::GridContext& ctx) const
{
    return ctx.bpm               != lastBpm_
        || ctx.samplesPerBeat    != lastSamplesPerBeat_
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

    // The arrangement extends a few bars past the live now-line so there is room
    // to scroll ahead of playback.
    const std::int64_t nowLine =
        nowLineProvider_ ? nowLineProvider_() : grid.phaseOriginSample;
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

void DawPanel::updateNowLine()
{
    // PRD-0102: while the DJ is scrubbing the ruler, the playhead line follows
    // the cursor live (scrubSample_) regardless of the transport position — the
    // authoritative seek is committed on mouse-up (§1.5.1).
    if (scrubbing_)
    {
        const double x = transform_.sampleToX (scrubSample_);
        playhead_.setLineX ((int) std::llround (TimelineTransform::alignToPixelGrid (x)));
        return;
    }

    if (! nowLineProvider_)
    {
        playhead_.setLineX (-1);
        return;
    }

    const std::int64_t sample = nowLineProvider_();
    const double x = transform_.sampleToX (sample);
    playhead_.setLineX ((int) std::llround (TimelineTransform::alignToPixelGrid (x)));
}

void DawPanel::updateRecordPlayhead()
{
    // Hidden unless a session is armed/recording and a position is available.
    const bool armed = recordStateProvider_ && recordStateProvider_() != RecordUiState::Idle;
    if (! armed || ! recordPlayheadProvider_)
    {
        recordPlayhead_.setLineX (-1);
        return;
    }

    const std::int64_t sample = recordPlayheadProvider_();
    const double x = transform_.sampleToX (sample);
    recordPlayhead_.setLineX ((int) std::llround (TimelineTransform::alignToPixelGrid (x)));
}

void DawPanel::applyFollowIfNeeded()
{
    if (! nowLineProvider_ || ! followController_.isEnabled())
        return;

    const double viewportWidth = transform_.getViewportWidth();
    const double nowLineX = transform_.sampleToX (nowLineProvider_());

    if (! followController_.shouldFollow (nowLineX, viewportWidth))
        return;

    // Re-anchor the now-line to the re-anchor fraction of the viewport.
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
    return false;
}

void DawPanel::timerCallback()
{
    const auto ctx = gridService_.snapshotGrid();
    if (gridChanged (ctx))
    {
        rebuildTransform();
        lastNowLineSample_ = nowLineProvider_ ? nowLineProvider_() : 0;
    }
    else if (nowLineProvider_)
    {
        const std::int64_t nowLine = nowLineProvider_();

        // Keep the content-end at least one full viewport ahead of the live
        // now-line so the follow re-anchor always has room and long clips have
        // somewhere to grow into.
        const auto grid = transform_.grid();
        const std::int64_t barMargin =
            static_cast<std::int64_t> (grid.samplesPerBeat * DawState::kBeatsPerBar * 4);
        const std::int64_t viewportSamples =
            static_cast<std::int64_t> (transform_.getViewportWidth()
                                       * grid.samplesPerBeat
                                       / juce::jmax (1.0, transform_.getPixelsPerBeat()));
        transform_.setContentEndSample (nowLine + juce::jmax (barMargin, viewportSamples));

        // The clip blocks resize themselves as their crop end grows (ClipBlock
        // listens to its own ValueTree), so here we only need to keep the
        // now-line in step with playback — no full relayout/repaint per tick.
        lastNowLineSample_ = nowLine;
        updateNowLine();
    }
    else
    {
        updateNowLine();
    }

    // Keep the record playhead in step and repaint the header when its state
    // changes so the Record button reflects idle / armed / recording.
    updateRecordPlayhead();
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

    // Always-visible collapse/expand toggle: a square button on the right.
    const int toggleSize = kHeaderHeight - 8;
    toggleBounds_ = juce::Rectangle<int> (header.getRight() - toggleSize - 4,
                                          header.getY() + 4,
                                          toggleSize, toggleSize);

    // Follow-playhead toggle: a labelled button to the left of the collapse one.
    const int followWidth = 64;
    followToggleBounds_ = juce::Rectangle<int> (toggleBounds_.getX() - followWidth - 6,
                                                header.getY() + 4,
                                                followWidth, toggleSize);

    // Global Record button: a labelled square to the left of the follow toggle.
    const int recordWidth = 64;
    recordButtonBounds_ = juce::Rectangle<int> (followToggleBounds_.getX() - recordWidth - 6,
                                                header.getY() + 4,
                                                recordWidth, toggleSize);

    // Metronome (testing aid) toggle: a labelled button between LOOP and REC.
    const int metroW = 52;
    metroBounds_ = juce::Rectangle<int> (recordButtonBounds_.getX() - metroW - 6,
                                         header.getY() + 4, metroW, toggleSize);

    // PRD-0082: Transport buttons to the left of the Metronome button.
    // STOP | PAUSE | PLAY | LOOP (each 44px wide)
    const int transportW = 44;
    const int transportH = toggleSize;
    const int transportGap = 4;

    loopBounds_  = juce::Rectangle<int> (metroBounds_.getX() - transportW - 6,
                                          header.getY() + 4, transportW, transportH);
    stopBounds_  = juce::Rectangle<int> (loopBounds_.getX()  - transportW - transportGap,
                                          header.getY() + 4, transportW, transportH);
    pauseBounds_ = juce::Rectangle<int> (stopBounds_.getX()  - transportW - transportGap,
                                          header.getY() + 4, transportW, transportH);
    playBounds_  = juce::Rectangle<int> (pauseBounds_.getX() - transportW - transportGap,
                                          header.getY() + 4, transportW, transportH);

    // PRD-0093: master tempo automation disclosure ("M.AUTO"), left of PLAY.
    const int masterAutoW = 60;
    masterAutoBounds_ = juce::Rectangle<int> (playBounds_.getX() - masterAutoW - 6,
                                              header.getY() + 4, masterAutoW, transportH);

    // PRD-0102: grid-snap toggle + granularity selector, left of M.AUTO.
    const int snapW = 52;
    const int granW = 52;
    snapToggleBounds_ = juce::Rectangle<int> (masterAutoBounds_.getX() - snapW - 6,
                                              header.getY() + 4, snapW, transportH);
    snapGranBounds_   = juce::Rectangle<int> (snapToggleBounds_.getX() - granW - transportGap,
                                              header.getY() + 4, granW, transportH);

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
        recordPlayhead_.setVisible (true);
        layoutPlayhead();
    }
    else
    {
        ruler_.setVisible (false);
        bodyViewport_.setVisible (false);
        interaction_.setVisible (false);
        playhead_.setVisible (false);
        recordPlayhead_.setVisible (false);
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

    recordPlayhead_.setBounds (gutter, top,
                               juce::jmax (0, getWidth() - gutter),
                               juce::jmax (0, bottom - top));
    recordPlayhead_.toFront (false);

    updateNowLine();
    updateRecordPlayhead();
}

void DawPanel::layoutBody()
{
    stack_.layoutToContentHeight (juce::jmax (1, bodyViewport_.getMaximumVisibleWidth()));
}

void DawPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Header strip.
    auto header = bounds.removeFromTop (kHeaderHeight);
    g.setColour (kHeaderBg);
    g.fillRect (header);

    // Title, left-aligned (Space Mono, ink on the light header). PRD-0096: when
    // a session is active the indicator (file name + trailing dirty dot) is
    // shown after the "ARRANGEMENT" label so the DJ always knows which file
    // they are editing and whether it has unsaved changes.
    g.setColour (kInk);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    auto titleArea = header.withTrimmedLeft (8).withTrimmedRight (kHeaderHeight + 8);
    g.drawText ("ARRANGEMENT", titleArea, juce::Justification::centredLeft, false);

    if (sessionTitle_.isNotEmpty())
    {
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
        g.drawText (sessionTitle_,
                    titleArea.withTrimmedLeft (104),
                    juce::Justification::centredLeft, true);
    }

    // Collapse/expand toggle: 2-px ink border, surface fill, glyph centred.
    g.setColour (kSurface);
    g.fillRect (toggleBounds_);
    g.setColour (kInk);
    g.drawRect (toggleBounds_, 2);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold));
    g.drawText (expanded_ ? juce::String ("-") : juce::String ("+"),
                toggleBounds_, juce::Justification::centred, false);

    // Follow-playhead toggle: inverted active/inactive fill (DESIGN.md).
    const bool following = followController_.isEnabled();
    g.setColour (following ? kInk : kSurface);
    g.fillRect (followToggleBounds_);
    g.setColour (kInk);
    g.drawRect (followToggleBounds_, 2);
    g.setColour (following ? kSurface : kInk);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.drawText ("FOLLOW", followToggleBounds_, juce::Justification::centred, false);

    // Global Record button
    {
        const RecordUiState rec = recordStateProvider_ ? recordStateProvider_()
                                                        : RecordUiState::Idle;
        const bool active = (rec != RecordUiState::Idle);

        g.setColour (active ? kInk : kSurface);
        g.fillRect (recordButtonBounds_);

        g.setColour (kInk);
        g.drawRect (recordButtonBounds_, 2);
        g.setColour (active ? kSurface : kInk);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold));
        g.drawText ("REC", recordButtonBounds_, juce::Justification::centred, false);
    }

    // PRD-0082: Transport buttons — PLAY, PAUSE, STOP, LOOP.
    {
        const bool playing    = isTransportPlaying    ? isTransportPlaying()    : false;
        const bool paused     = isTransportPaused     ? isTransportPaused()     : false;
        const bool loopArmed  = isTransportLoopEnabled ? isTransportLoopEnabled() : false;

        auto drawTransportBtn = [&] (const juce::Rectangle<int>& r,
                                     const juce::String& label, bool active)
        {
            g.setColour (active ? kInk : kSurface);
            g.fillRect (r);
            g.setColour (kInk);
            g.drawRect (r, 2);
            g.setColour (active ? kSurface : kInk);
            g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::bold));
            g.drawText (label, r, juce::Justification::centred, false);
        };

        drawTransportBtn (playBounds_,  "PLAY",  playing && !paused);
        drawTransportBtn (pauseBounds_, "PAUS",  paused);
        drawTransportBtn (stopBounds_,  "STOP",  !playing && !paused);
        drawTransportBtn (loopBounds_,  "LOOP",  loopArmed);

        // Metronome (testing aid) toggle: active/inactive fill inversion.
        drawTransportBtn (metroBounds_, "METRO", metronomeOn_);

        // PRD-0093: master tempo automation disclosure (fill inversion).
        drawTransportBtn (masterAutoBounds_, "M.AUTO", masterAutoRevealed_);

        // PRD-0102: grid-snap toggle (active/inactive fill inversion) + the
        // granularity selector (a plain button showing the current subdivision).
        drawTransportBtn (snapToggleBounds_, "SNAP", snap_.enabled);
        drawTransportBtn (snapGranBounds_, labelForGranularity (snap_.granularity), false);
    }

    // Blank gutter corner above the lane headers, beside the ruler (when expanded).
    if (expanded_)
    {
        auto rulerRow = bounds.removeFromTop (TimeRuler::kRulerHeight);
        auto corner   = rulerRow.withWidth (DawLayout::kTrackHeaderWidth);
        g.setColour (kHeaderBg);
        g.fillRect (corner);
        g.setColour (kInk);
        g.drawRect (corner, 2);
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
        setExpanded (! expanded_);
        return;
    }

    if (followToggleBounds_.contains (event.getPosition()))
    {
        followController_.toggle();
        // Re-engaging follow snaps the now-line back into view immediately.
        if (followController_.isEnabled())
            applyFollowIfNeeded();
        repaint();
        return;
    }

    if (recordButtonBounds_.contains (event.getPosition()))
    {
        if (onRecordToggle)
            onRecordToggle();
        repaint();
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

    // PRD-0102: grid-snap toggle + granularity cycle. Both only repaint the
    // header; the new state is read at the start of the next edit/scrub gesture.
    if (snapToggleBounds_.contains (event.getPosition()))
    {
        snap_.enabled = ! snap_.enabled;
        repaint();
        return;
    }
    if (snapGranBounds_.contains (event.getPosition()))
    {
        snap_.granularity = nextGranularity (snap_.granularity);
        repaint();
        return;
    }
}

void DawPanel::mouseDown (const juce::MouseEvent& event)
{
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
                juce::PopupMenu::Options().withTargetComponent (this),
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
