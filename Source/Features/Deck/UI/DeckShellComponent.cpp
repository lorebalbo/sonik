#include "DeckShellComponent.h"

DeckShellComponent::DeckShellComponent (DeckStateManager& deckState,
                                        AudioEngine& engine,
                                        AudioFileLoader& loader,
                                        WaveformManager& waveformMgr,
                                        BeatGridManager& beatGridMgr,
                                        StemSeparationManager& stemMgr,
                                        const juce::String& id)
    : deckStateManager (deckState),
      audioEngine (engine),
      audioFileLoader (loader),
      waveformManager (waveformMgr),
      beatGridManager (beatGridMgr),
      stemSeparationManager (stemMgr),
      deckId (id),
      deckTree (deckState.getDeckState (id)),
      rootState (deckState.getStateTree())
{
    setOpaque (false);

    // -----------------------------------------------------------------------
    // Deck header — always visible
    // -----------------------------------------------------------------------
    trackInfoComponent = std::make_unique<TrackInfoComponent> (
        deckTree, deckStateManager, audioFileLoader, deckId);
    trackInfoComponent->onRemoveRequested = [this]()
    {
        if (onRemoveRequested)
            onRemoveRequested (deckId);
    };
    addAndMakeVisible (*trackInfoComponent);

    // -----------------------------------------------------------------------
    // Stems row
    // -----------------------------------------------------------------------
    stemSeparateButton = std::make_unique<StemSeparateButton> (
        deckTree, stemSeparationManager, audioEngine, deckId);
    addAndMakeVisible (*stemSeparateButton);

    stemVocToggle = std::make_unique<StemToggleComponent> (
        deckTree.getChildWithName (IDs::Stems), "VOCALS",
        std::vector<juce::Identifier> { IDs::vocalsMuted });
    addAndMakeVisible (*stemVocToggle);

    stemInstToggle = std::make_unique<StemToggleComponent> (
        deckTree.getChildWithName (IDs::Stems), "INSTRUMENTAL",
        std::vector<juce::Identifier> { IDs::drumsMuted, IDs::bassMuted, IDs::otherMuted },
        "Mutes drums, bass, and other non-vocal elements");
    addAndMakeVisible (*stemInstToggle);

    // -----------------------------------------------------------------------
    // Time & Pitch sidebar
    // -----------------------------------------------------------------------
    keyLockButton = std::make_unique<KeyLockButton> (deckTree);
    addAndMakeVisible (*keyLockButton);

    keyStepperComponent = std::make_unique<KeyStepperComponent> (deckTree);
    addAndMakeVisible (*keyStepperComponent);

    pitchFaderComponent = std::make_unique<PitchFaderComponent> (deckTree);
    addAndMakeVisible (*pitchFaderComponent);

    // -----------------------------------------------------------------------
    // Control row
    // -----------------------------------------------------------------------
    syncButton = std::make_unique<SyncButtonComponent> (deckTree);
    addAndMakeVisible (*syncButton);

    quantizeButton = std::make_unique<QuantizeButtonComponent> (deckTree);
    addAndMakeVisible (*quantizeButton);

    slipButton = std::make_unique<SlipButtonComponent> (
        deckTree, deckStateManager.getAudioState (deckId),
        audioEngine, deckId);
    addAndMakeVisible (*slipButton);

    // -----------------------------------------------------------------------
    // Controller widget subcomponents
    // -----------------------------------------------------------------------
    hotCueManager = std::make_unique<HotCueManager> (
        deckTree, audioEngine, deckId, deckStateManager.getDatabase());
    hotCueManager->setAudioState (deckStateManager.getAudioState (deckId));
    hotCueManager->addListener (this);

    hotCuePadComponent = std::make_unique<HotCuePadComponent> (deckTree);
    hotCuePadComponent->onSetCue      = [this] (int pad) { hotCueManager->setCue (pad); };
    hotCuePadComponent->onTriggerCue  = [this] (int pad) { hotCueManager->triggerCue (pad); };
    hotCuePadComponent->onDeleteCue   = [this] (int pad) { hotCueManager->deleteCue (pad); };
    hotCuePadComponent->onUndoDelete  = [this] ()        { hotCueManager->undoDelete(); };
    hotCuePadComponent->onColorChange = [this] (int pad, int color) { hotCueManager->setColor (pad, color); };
    hotCuePadComponent->onLabelChange = [this] (int pad, const juce::String& lbl) { hotCueManager->setLabel (pad, lbl); };

    loopEngine = std::make_unique<LoopEngine> (
        deckTree, audioEngine, deckId, deckStateManager.getDatabase());
    loopEngine->setAudioState (deckStateManager.getAudioState (deckId));
    loopEngine->addListener (this);

    loopControlComponent = std::make_unique<LoopControlComponent> (deckTree);
    loopControlComponent->onAutoLoop   = [this] (float beats) { loopEngine->autoLoop (beats); };
    loopControlComponent->onLoopIn     = [this] ()             { loopEngine->setLoopIn(); };
    loopControlComponent->onLoopOut    = [this] ()             { loopEngine->setLoopOut(); };
    loopControlComponent->onToggleLoop = [this] ()             { loopEngine->toggleLoop(); };
    loopControlComponent->onReLoop     = [this] ()             { loopEngine->reLoop(); };
    loopControlComponent->onLoopHalve  = [this] ()             { loopEngine->loopHalve(); };
    loopControlComponent->onLoopDouble = [this] ()             { loopEngine->loopDouble(); };

    beatJumpEngine = std::make_unique<BeatJumpEngine> (deckTree, audioEngine, deckId);
    beatJumpEngine->setAudioState (deckStateManager.getAudioState (deckId));
    beatJumpEngine->setLoopEngine (loopEngine.get());

    beatJumpComponent = std::make_unique<BeatJumpComponent> (deckTree);
    beatJumpComponent->onJumpForward  = [this] ()        { beatJumpEngine->jumpForward(); };
    beatJumpComponent->onJumpBackward = [this] ()        { beatJumpEngine->jumpBackward(); };

    // ControllerWidget owns layout but not lifetime of subcomponents
    controllerWidget = std::make_unique<ControllerWidget> (
        deckTree,
        loopControlComponent.get(),
        hotCuePadComponent.get(),
        beatJumpComponent.get());

    // Wire transport callbacks for the JUMP tab
    controllerWidget->onCuePress  = [this] { handleCuePress(); };
    controllerWidget->onStopPress = [this] { handleStopPress(); };
    controllerWidget->onPlayPress = [this] { handlePlayPress(); };

    // Wire beatgrid callbacks for the GRID tab
    controllerWidget->onGridSet    = [this] { handleGridSet(); };
    controllerWidget->onGridDelete = [this] { handleGridDelete(); };
    controllerWidget->onGridNudge  = [this] (int d) { handleGridNudge (d); };
    controllerWidget->getBpmString = [this]() -> juce::String
    {
        auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
        if (! beatTree.isValid()) return "--";
        double bpm = static_cast<double> (beatTree.getProperty (IDs::bpm, 0.0));
        return (bpm > 0.0) ? juce::String (bpm, 1) : "--";
    };

    addAndMakeVisible (*controllerWidget);

    // -----------------------------------------------------------------------
    // State listeners
    // -----------------------------------------------------------------------
    deckTree.addListener (this);
    rootState.addListener (this);
}

DeckShellComponent::~DeckShellComponent()
{
    if (loopEngine    != nullptr) loopEngine->removeListener (this);
    if (hotCueManager != nullptr) hotCueManager->removeListener (this);

    rootState.removeListener (this);
    if (deckTree.isValid())
        deckTree.removeListener (this);
}

// --- Active state ---

void DeckShellComponent::updateActiveState()
{
    repaint();
}

bool DeckShellComponent::isActive() const
{
    return deckStateManager.getActiveDeckId() == deckId;
}

bool DeckShellComponent::isTrackLoaded() const
{
    if (! deckTree.isValid())
        return false;
    auto status = deckTree.getProperty (IDs::playbackStatus).toString();
    return status != "empty";
}

bool DeckShellComponent::isPlaying() const
{
    if (! deckTree.isValid())
        return false;
    return deckTree.getProperty (IDs::playbackStatus).toString() == "playing";
}

// --- Playback helpers (JUMP tab transport) ---

void DeckShellComponent::handleCuePress()
{
    // If temp cue is set, seek there; otherwise set temp cue at current position
    auto* audioState = deckStateManager.getAudioState (deckId);
    if (audioState != nullptr)
    {
        int64_t cuePos = audioState->tempCuePosition.load (std::memory_order_relaxed);
        if (cuePos >= 0)
            audioEngine.seekDeck (deckId, cuePos);
        else
            audioEngine.seekDeck (deckId, 0); // fallback to start
    }
}

void DeckShellComponent::handleStopPress()
{
    deckStateManager.setPlaybackStatus (deckId, "paused");
    // Return to temp cue
    auto* audioState = deckStateManager.getAudioState (deckId);
    if (audioState != nullptr)
    {
        int64_t cuePos = audioState->tempCuePosition.load (std::memory_order_relaxed);
        if (cuePos >= 0)
            audioEngine.seekDeck (deckId, cuePos);
    }
}

void DeckShellComponent::handlePlayPress()
{
    if (isPlaying())
        deckStateManager.setPlaybackStatus (deckId, "paused");
    else
        deckStateManager.setPlaybackStatus (deckId, "playing");
}

// --- Beatgrid helpers (GRID tab) ---

void DeckShellComponent::handleGridSet()
{
    auto* audioState = deckStateManager.getAudioState (deckId);
    if (audioState == nullptr) return;

    int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
    auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
    if (beatTree.isValid())
    {
        beatTree.setProperty (IDs::anchorSample, pos, nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted, true, nullptr);
    }
}

void DeckShellComponent::handleGridDelete()
{
    auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
    if (beatTree.isValid())
    {
        beatTree.setProperty (IDs::manuallyAdjusted, false, nullptr);
        beatTree.setProperty (IDs::anchorSample, 0, nullptr);
    }
}

void DeckShellComponent::handleGridNudge (int delta)
{
    auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
    if (! beatTree.isValid()) return;

    int64_t anchor = static_cast<int64_t> (
        beatTree.getProperty (IDs::anchorSample, 0));
    int64_t interval = static_cast<int64_t> (
        beatTree.getProperty (IDs::beatIntervalSamples, 0));

    if (interval > 0)
    {
        int64_t nudgeAmount = (std::abs (delta) > 1) ? interval : interval / 8;
        if (delta < 0) nudgeAmount = -nudgeAmount;
        beatTree.setProperty (IDs::anchorSample,
                              juce::jmax<int64_t> (0, anchor + nudgeAmount), nullptr);
        beatTree.setProperty (IDs::manuallyAdjusted, true, nullptr);
    }
}

// --- Paint ---

void DeckShellComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Deck background — #E5E5E5 matching Figma
    g.setColour (juce::Colour (0xFFE5E5E5));
    g.fillRect (bounds);

    // Outer border — 2px #2D2D2D
    g.setColour (juce::Colour (0xFF2D2D2D));
    g.drawRect (bounds, 2);

    // Active deck: bright accent on left edge (kept from original design)
    if (isActive())
    {
        g.setColour (juce::Colour (0xFF000000));
        g.fillRect (0, 0, 4, getHeight());
    }

    // Empty state hint overlaid on waveform area when no track loaded.
    // Use the same dynamic panel width as the loaded waveform so all rows stay
    // the same width regardless of whether a track is present.
    if (! isTrackLoaded())
    {
        const int panelW = getPanelW();
        auto contentBounds = getLocalBounds().reduced (kPad);
        auto waveArea = contentBounds.withTrimmedTop (kHeaderH + kGap + kStemsH + kGap)
                                     .withWidth (panelW)
                                     .withHeight (kMainH);
        paintEmptyState (g, waveArea);
    }

    // Drag overlay
    if (isDragOver)
    {
        g.setColour (juce::Colour (0x20000000));
        g.fillRect (getLocalBounds());
    }
}

void DeckShellComponent::paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto emptyBox = area.reduced (12);

    // Background for empty waveform areas
    g.setColour (juce::Colour (0xFFF9F9F9));
    g.fillRect (area);

    g.setColour (juce::Colour (0xFF2D2D2D));
    g.drawRect (area, 2);

    g.setColour (juce::Colour (0x80000000));
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText ("Drop a track here or browse your library",
                emptyBox, juce::Justification::centred);
}

// --- Layout ---

void DeckShellComponent::resized()
{
    // Compute the actual panel width from the component's real size.
    // When the deck is exactly 592 px wide this equals kPanelW (474);
    // on wider decks it expands so every row stretches to the same width.
    const int panelW = getPanelW();

    auto bounds = getLocalBounds().reduced (kPad);

    // --- Row 1: Deck Header (always visible) ---
    auto headerRow = bounds.removeFromTop (kHeaderH);
    if (trackInfoComponent != nullptr)
        trackInfoComponent->setBounds (headerRow);

    bounds.removeFromTop (kGap);

    // --- Row 2: Stems section (3 buttons with 10px gaps, matching SYNC/Q/SLIP row) ---
    auto stemsRow = bounds.removeFromTop (kStemsH);
    {
        const int stemsGap = 10;
        const int bw       = (panelW - 2 * stemsGap) / 3;
        const int lastW    = panelW - 2 * (bw + stemsGap); // absorbs rounding
        const int x0 = stemsRow.getX();
        const int y0 = stemsRow.getY();

        if (stemSeparateButton != nullptr)
            stemSeparateButton->setBounds (x0, y0, bw, kStemsH);

        if (stemVocToggle != nullptr)
            stemVocToggle->setBounds (x0 + bw + stemsGap, y0, bw, kStemsH);

        if (stemInstToggle != nullptr)
            stemInstToggle->setBounds (x0 + 2 * (bw + stemsGap), y0, lastW, kStemsH);
    }

    bounds.removeFromTop (kGap);

    // --- Row 3: Waveform (474px) + Time&Pitch sidebar (70px) ---
    auto mainRow = bounds.removeFromTop (kMainH);
    {
        auto sidebar = mainRow.removeFromRight (kPitchSidebarW);
        mainRow.removeFromRight (kSidebarGap);

        // Waveform fills left portion
        if (waveformComponent != nullptr && isTrackLoaded())
            waveformComponent->setBounds (mainRow);

        // Time & Pitch sidebar layout
        // Heights: KEY(23) + gap(12) + Stepper(23) + gap(12) + Fader(fill) + gap(12) + CycleBtn area
        // We give the fader everything between the stepper and a bottom strip
        const int btnH    = 23;
        const int sideGap = 12;

        if (keyLockButton != nullptr)
            keyLockButton->setBounds (sidebar.removeFromTop (btnH));

        sidebar.removeFromTop (sideGap);

        if (keyStepperComponent != nullptr)
            keyStepperComponent->setBounds (sidebar.removeFromTop (btnH));

        sidebar.removeFromTop (sideGap);

        // Pitch fader fills all remaining space; the ±% range button is rendered
        // inside pitchFaderComponent at its bottom 23px (req 3, 4).
        if (pitchFaderComponent != nullptr)
            pitchFaderComponent->setBounds (sidebar);
    }

    bounds.removeFromTop (kGap);

    // --- Row 4: Control row (SYNC / QUANT / SLIP) — same width as waveform ---
    auto controlRow = bounds.removeFromTop (kControlRowH);
    controlRow = controlRow.withWidth (panelW);
    {
        // Divide the full panelW equally across 3 buttons with 2 gaps of 10px.
        const int ctrlGap = 10;
        const int availW  = controlRow.getWidth() - 2 * ctrlGap;
        const int syncW_  = availW / 3;
        const int quantW_ = availW / 3;
        const int slipW_  = availW - syncW_ - quantW_; // absorbs rounding

        if (syncButton != nullptr)
            syncButton->setBounds (controlRow.removeFromLeft (syncW_));
        controlRow.removeFromLeft (ctrlGap);

        if (quantizeButton != nullptr)
            quantizeButton->setBounds (controlRow.removeFromLeft (quantW_));
        controlRow.removeFromLeft (ctrlGap);

        if (slipButton != nullptr)
            slipButton->setBounds (controlRow.removeFromLeft (slipW_));
    }

    bounds.removeFromTop (kGap);

    // --- Row 5: Controller Widget (474px panel + 8px gap + 70px tabs) ---
    auto controllerRow = bounds.removeFromTop (kControllerH);
    if (controllerWidget != nullptr)
        controllerWidget->setBounds (controllerRow.withWidth (panelW + kSidebarGap + kPitchSidebarW));
}

// --- Mouse ---

void DeckShellComponent::mouseDown (const juce::MouseEvent&)
{
    deckStateManager.setActiveDeck (deckId);
}

// --- File drag and drop ---

bool DeckShellComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        juce::File file (f);
        if (AudioFileLoader::isSupportedExtension (file.getFileExtension()))
            return true;
    }
    return false;
}

void DeckShellComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    isDragOver = true;
    repaint();
}

void DeckShellComponent::fileDragExit (const juce::StringArray&)
{
    isDragOver = false;
    repaint();
}

void DeckShellComponent::filesDropped (const juce::StringArray& files, int, int)
{
    isDragOver = false;
    repaint();

    for (const auto& f : files)
    {
        juce::File file (f);
        if (AudioFileLoader::isSupportedExtension (file.getFileExtension()))
        {
            deckStateManager.setActiveDeck (deckId);
            audioFileLoader.loadFile (deckId, file);
            break;
        }
    }
}

// --- ValueTree::Listener ---

void DeckShellComponent::valueTreePropertyChanged (juce::ValueTree& tree,
                                                    const juce::Identifier& property)
{
    // Root state: active deck changed
    if (tree == rootState && property == IDs::activeDeckId)
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
            {
                safeThis->updateActiveState();
                safeThis->resized();
            }
        });
        return;
    }

    // Deck tree: playback/loading status changed
    if (tree == deckTree)
    {
        if (property == IDs::playbackStatus || property == IDs::loadingStatus)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis == nullptr)
                    return;

                if (! safeThis->isTrackLoaded())
                {
                    // Track ejected — remove waveform
                    if (safeThis->waveformComponent != nullptr)
                    {
                        safeThis->removeChildComponent (safeThis->waveformComponent.get());
                        safeThis->waveformComponent.reset();
                    }
                }

                safeThis->resized();
                safeThis->repaint();
            });
        }
    }

    // Waveform analysis done
    if (tree.hasType (IDs::Waveform) && property == IDs::analysisStatus)
    {
        if (tree.getParent() == deckTree)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis == nullptr)
                    return;

                auto status = safeThis->deckTree.getChildWithName (IDs::Waveform)
                                  .getProperty (IDs::analysisStatus).toString();

                if (status == "done")
                {
                    auto data = safeThis->waveformManager.getWaveformData (safeThis->deckId);
                    if (data != nullptr)
                    {
                        if (safeThis->waveformComponent == nullptr)
                        {
                            safeThis->waveformComponent = std::make_unique<WaveformComponent>();
                            safeThis->addAndMakeVisible (*safeThis->waveformComponent);

                            safeThis->waveformComponent->onSeek = [safeThis] (int64_t pos)
                            {
                                if (safeThis == nullptr)
                                    return;

                                // Deactivate loop if seeking outside active loop region
                                if (safeThis->loopEngine != nullptr)
                                {
                                    auto loopInfo = safeThis->loopEngine->getLoopInfo();
                                    if (loopInfo.active && loopInfo.inSamples >= 0
                                        && loopInfo.outSamples > loopInfo.inSamples)
                                    {
                                        bool inside = (pos >= loopInfo.inSamples
                                                       && pos < loopInfo.outSamples);
                                        if (! inside)
                                            safeThis->loopEngine->toggleLoop();
                                    }
                                }

                                safeThis->audioEngine.seekDeck (safeThis->deckId, pos);
                            };
                        }

                        safeThis->waveformComponent->setWaveformData (data);
                        safeThis->waveformComponent->setAudioState (
                            safeThis->deckStateManager.getAudioState (safeThis->deckId));

                        // BeatGrid may already be ready
                        auto bgStatus = safeThis->deckTree.getChildWithName (IDs::BeatGrid)
                                            .getProperty (IDs::analysisStatus).toString();
                        if (bgStatus == "done")
                        {
                            auto bgData = safeThis->beatGridManager.getBeatGridData (safeThis->deckId);
                            if (bgData != nullptr)
                            {
                                safeThis->waveformComponent->setBeatGridData (bgData);
                                if (safeThis->hotCueManager != nullptr)
                                    safeThis->hotCueManager->setBeatGridData (bgData);
                                if (safeThis->loopEngine != nullptr)
                                    safeThis->loopEngine->setBeatGridData (bgData);
                            }
                        }

                        safeThis->updateWaveformHotCues();
                        safeThis->resized();
                        safeThis->repaint();
                    }
                }
            });
        }
    }

    // BeatGrid analysis done
    if (tree.hasType (IDs::BeatGrid) && property == IDs::analysisStatus)
    {
        if (tree.getParent() == deckTree)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis == nullptr)
                    return;

                auto status = safeThis->deckTree.getChildWithName (IDs::BeatGrid)
                                  .getProperty (IDs::analysisStatus).toString();
                if (status == "done" && safeThis->waveformComponent != nullptr)
                {
                    auto data = safeThis->beatGridManager.getBeatGridData (safeThis->deckId);
                    if (data != nullptr)
                    {
                        safeThis->waveformComponent->setBeatGridData (data);
                        if (safeThis->hotCueManager != nullptr)
                            safeThis->hotCueManager->setBeatGridData (data);
                        if (safeThis->loopEngine != nullptr)
                            safeThis->loopEngine->setBeatGridData (data);
                    }
                }
            });
        }
    }
}

// --- HotCueManager::Listener ---

void DeckShellComponent::hotCuesChanged()
{
    updateWaveformHotCues();
}

void DeckShellComponent::updateWaveformHotCues()
{
    if (waveformComponent != nullptr && hotCueManager != nullptr)
        waveformComponent->setHotCues (hotCueManager->getHotCues());
}

// --- LoopEngine::Listener ---

void DeckShellComponent::loopStateChanged()
{
    updateLoopControlState();
}

void DeckShellComponent::updateLoopControlState()
{
    if (loopControlComponent != nullptr && loopEngine != nullptr)
    {
        auto info = loopEngine->getLoopInfo();
        loopControlComponent->setActiveAutoLoopBeats (info.activeAutoBeats);
        loopControlComponent->setPendingLoopIn (info.pendingIn);
    }
}
