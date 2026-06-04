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
    interaction_.onUp      = [this] (const juce::MouseEvent& e) { dragging_ = false; juce::ignoreUnused (e); };
    interaction_.onWheel   = [this] (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
                             { mouseWheelMove (e.getEventRelativeTo (this), w); };
    interaction_.onMagnify = [this] (const juce::MouseEvent& e, float s)
                             { mouseMagnify (e.getEventRelativeTo (this), s); };
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

void DawPanel::updateNowLine()
{
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

    // PRD-0082: Transport buttons to the left of the Record button.
    // STOP | PAUSE | PLAY | LOOP (each 44px wide)
    const int transportW = 44;
    const int transportH = toggleSize;
    const int transportGap = 4;

    loopBounds_  = juce::Rectangle<int> (recordButtonBounds_.getX() - transportW - 6,
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

    // Title, left-aligned (Space Mono, ink on the light header).
    g.setColour (kInk);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText ("ARRANGEMENT",
                header.withTrimmedLeft (8).withTrimmedRight (kHeaderHeight + 8),
                juce::Justification::centredLeft, false);

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

        // PRD-0093: master tempo automation disclosure (fill inversion).
        drawTransportBtn (masterAutoBounds_, "M.AUTO", masterAutoRevealed_);
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

    // PRD-0093: master tempo automation disclosure.
    if (masterAutoBounds_.contains (event.getPosition()))
    {
        setMasterAutomationRevealed (! masterAutoRevealed_);
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

    dragging_  = true;
    dragLastX_ = event.getPosition().x;
}

void DawPanel::mouseDrag (const juce::MouseEvent& event)
{
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
