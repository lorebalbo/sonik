#include "DeckShellComponent.h"

DeckShellComponent::DeckShellComponent (DeckStateManager& deckState,
                                        AudioEngine& engine,
                                        AudioFileLoader& loader,
                                        WaveformManager& waveformMgr,
                                        BeatGridManager& beatGridMgr,
                                        const juce::String& id)
    : deckStateManager (deckState),
      audioEngine (engine),
      audioFileLoader (loader),
      waveformManager (waveformMgr),
      beatGridManager (beatGridMgr),
      deckId (id),
      deckTree (deckState.getDeckState (id)),
      rootState (deckState.getStateTree())
{
    setOpaque (false);

    removeButton.setButtonText (juce::String::charToString (0x00D7)); // multiplication sign as X
    removeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    removeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    removeButton.onClick = [this]()
    {
        if (onRemoveRequested)
            onRemoveRequested (deckId);
    };
    addAndMakeVisible (removeButton);

    // Create pitch fader and gain knob (always visible)
    pitchFaderComponent = std::make_unique<PitchFaderComponent> (deckTree);
    addAndMakeVisible (*pitchFaderComponent);

    gainKnobComponent = std::make_unique<GainKnobComponent> (deckTree);
    addAndMakeVisible (*gainKnobComponent);

    // Create key lock button
    keyLockButton = std::make_unique<KeyLockButton> (deckTree);
    addAndMakeVisible (*keyLockButton);

    // Create quantize button (PRD-0013)
    quantizeButton = std::make_unique<QuantizeButtonComponent> (deckTree);
    addAndMakeVisible (*quantizeButton);

    // Create hot cue manager and pad component (PRD-0012)
    hotCueManager = std::make_unique<HotCueManager> (
        deckTree, audioEngine, deckId, deckStateManager.getDatabase());
    hotCueManager->setAudioState (deckStateManager.getAudioState (deckId));
    hotCueManager->addListener (this);

    hotCuePadComponent = std::make_unique<HotCuePadComponent> (deckTree);
    hotCuePadComponent->onSetCue     = [this] (int pad) { hotCueManager->setCue (pad); };
    hotCuePadComponent->onTriggerCue = [this] (int pad) { hotCueManager->triggerCue (pad); };
    hotCuePadComponent->onDeleteCue  = [this] (int pad) { hotCueManager->deleteCue (pad); };
    hotCuePadComponent->onUndoDelete = [this] ()        { hotCueManager->undoDelete(); };
    hotCuePadComponent->onColorChange = [this] (int pad, int color) { hotCueManager->setColor (pad, color); };
    hotCuePadComponent->onLabelChange = [this] (int pad, const juce::String& label) { hotCueManager->setLabel (pad, label); };
    addAndMakeVisible (*hotCuePadComponent);

    // Create loop engine and control component (PRD-0014)
    loopEngine = std::make_unique<LoopEngine> (
        deckTree, audioEngine, deckId, deckStateManager.getDatabase());
    loopEngine->setAudioState (deckStateManager.getAudioState (deckId));
    loopEngine->addListener (this);

    loopControlComponent = std::make_unique<LoopControlComponent> (deckTree);
    loopControlComponent->onAutoLoop  = [this] (float beats) { loopEngine->autoLoop (beats); };
    loopControlComponent->onLoopIn    = [this] ()             { loopEngine->setLoopIn(); };
    loopControlComponent->onLoopOut   = [this] ()             { loopEngine->setLoopOut(); };
    loopControlComponent->onToggleLoop = [this] ()            { loopEngine->toggleLoop(); };
    loopControlComponent->onReLoop    = [this] ()             { loopEngine->reLoop(); };
    loopControlComponent->onLoopHalve = [this] ()             { loopEngine->loopHalve(); };
    loopControlComponent->onLoopDouble = [this] ()            { loopEngine->loopDouble(); };
    addAndMakeVisible (*loopControlComponent);

    // Create beat jump engine and component (PRD-0015)
    beatJumpEngine = std::make_unique<BeatJumpEngine> (
        deckTree, audioEngine, deckId);
    beatJumpEngine->setAudioState (deckStateManager.getAudioState (deckId));
    beatJumpEngine->setLoopEngine (loopEngine.get());

    beatJumpComponent = std::make_unique<BeatJumpComponent> (deckTree);
    beatJumpComponent->onJumpForward  = [this] () { beatJumpEngine->jumpForward(); };
    beatJumpComponent->onJumpBackward = [this] () { beatJumpEngine->jumpBackward(); };
    beatJumpComponent->onCycleSize    = [this] (bool fwd) { beatJumpEngine->cycleJumpSize (fwd); };
    addAndMakeVisible (*beatJumpComponent);

    // Listen to deck tree and root state for property changes
    deckTree.addListener (this);
    rootState.addListener (this);
}

DeckShellComponent::~DeckShellComponent()
{
    if (loopEngine != nullptr)
        loopEngine->removeListener (this);

    if (hotCueManager != nullptr)
        hotCueManager->removeListener (this);

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

// --- Colours ---

juce::Colour DeckShellComponent::getDeckAccentColour (const juce::String& id)
{
    if (id == "A") return juce::Colour (0xFF3B82F6);
    if (id == "B") return juce::Colour (0xFFF97316);
    if (id == "C") return juce::Colour (0xFF22C55E);
    if (id == "D") return juce::Colour (0xFFA855F7);
    return juce::Colours::white;
}

// --- Paint ---

void DeckShellComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.setColour (juce::Colour (0xFFF3F3F4)); // surface-container-low
    g.fillRect (bounds);

    // Active indicator
    if (isActive())
        paintActiveIndicator (g);

    // Content area offset for active indicator
    auto contentBounds = bounds.withTrimmedLeft (activeIndicatorWidth);

    // Header
    auto headerArea = contentBounds.removeFromTop (headerHeight);
    paintHeader (g, headerArea);

    // Content
    if (! isTrackLoaded() || waveformComponent == nullptr)
        paintEmptyState (g, contentBounds);

    // Drag overlay
    if (isDragOver)
    {
        g.setColour (juce::Colour (0x20000000));
        g.fillRect (getLocalBounds());
    }
}

void DeckShellComponent::paintActiveIndicator (juce::Graphics& g)
{
    auto colour = getDeckAccentColour (deckId);
    g.setColour (colour);
    g.fillRect (0, 0, activeIndicatorWidth, getHeight());
}

void DeckShellComponent::paintHeader (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Header background
    g.setColour (juce::Colour (0xFFE2E2E2)); // surface-container-highest
    g.fillRect (area);

    // Deck label
    g.setColour (juce::Colours::black);
    g.setFont (juce::FontOptions (14.0f).withStyle ("Bold"));
    g.drawText ("DECK " + deckId,
                area.withTrimmedLeft (8).withTrimmedRight (headerHeight),
                juce::Justification::centredLeft);
}

void DeckShellComponent::paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto emptyBox = area.reduced (24);

    // Dashed border (simulate with line segments)
    g.setColour (juce::Colour (0x60000000));
    const float dashLen = 6.0f;
    const float gapLen  = 4.0f;

    auto fb = emptyBox.toFloat();

    // Top edge
    for (float x = fb.getX(); x < fb.getRight(); x += dashLen + gapLen)
        g.drawHorizontalLine ((int) fb.getY(), x, juce::jmin (x + dashLen, fb.getRight()));

    // Bottom edge
    for (float x = fb.getX(); x < fb.getRight(); x += dashLen + gapLen)
        g.drawHorizontalLine ((int) fb.getBottom(), x, juce::jmin (x + dashLen, fb.getRight()));

    // Left edge
    for (float y = fb.getY(); y < fb.getBottom(); y += dashLen + gapLen)
        g.drawVerticalLine ((int) fb.getX(), y, juce::jmin (y + dashLen, fb.getBottom()));

    // Right edge
    for (float y = fb.getY(); y < fb.getBottom(); y += dashLen + gapLen)
        g.drawVerticalLine ((int) fb.getRight(), y, juce::jmin (y + dashLen, fb.getBottom()));

    // Text
    g.setColour (juce::Colour (0x80000000));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Drop a track here or browse your library",
                emptyBox, juce::Justification::centred);
}

// --- Layout ---

void DeckShellComponent::resized()
{
    auto bounds = getLocalBounds().withTrimmedLeft (activeIndicatorWidth);
    auto headerArea = bounds.removeFromTop (headerHeight);

    // Remove button in header, right-aligned
    auto btnSize = headerHeight - 4;
    removeButton.setBounds (headerArea.removeFromRight (btnSize + 4)
                                .withSizeKeepingCentre (btnSize, btnSize));

    // Update remove button enabled state
    bool canRemove = deckStateManager.canRemoveDeck (deckId);
    removeButton.setEnabled (canRemove);

    if (! canRemove)
    {
        if (deckStateManager.getDeckCount() <= 1)
            removeButton.setTooltip ("At least one deck is required");
        else if (isPlaying())
            removeButton.setTooltip ("Stop playback to remove this deck");
    }
    else
    {
        removeButton.setTooltip ({});
    }

    // Control strip on the right side
    auto controlStrip = bounds.removeFromRight (controlStripWidth);

    // Gain knob at top of control strip
    if (gainKnobComponent != nullptr)
        gainKnobComponent->setBounds (controlStrip.removeFromTop (100));

    // Key lock button between gain and pitch fader
    if (keyLockButton != nullptr)
        keyLockButton->setBounds (controlStrip.removeFromTop (24).reduced (12, 2));

    // Quantize button below key lock (PRD-0013)
    if (quantizeButton != nullptr)
        quantizeButton->setBounds (controlStrip.removeFromTop (24).reduced (12, 2));

    // Pitch fader fills rest of control strip
    if (pitchFaderComponent != nullptr)
        pitchFaderComponent->setBounds (controlStrip);

    // Track info component above waveform
    if (trackInfoComponent != nullptr && isTrackLoaded())
        trackInfoComponent->setBounds (bounds.removeFromTop (trackInfoHeight));

    // Hot cue pad strip below waveform
    if (hotCuePadComponent != nullptr && isTrackLoaded())
    {
        auto padArea = bounds.removeFromBottom (hotCuePadHeight);
        hotCuePadComponent->setBounds (padArea);
        hotCuePadComponent->setVisible (true);
    }
    else if (hotCuePadComponent != nullptr)
    {
        hotCuePadComponent->setVisible (false);
    }

    // Loop control strip below hot cue pads
    if (loopControlComponent != nullptr && isTrackLoaded())
    {
        auto loopArea = bounds.removeFromBottom (loopControlHeight);
        loopControlComponent->setBounds (loopArea);
        loopControlComponent->setVisible (true);
    }
    else if (loopControlComponent != nullptr)
    {
        loopControlComponent->setVisible (false);
    }

    // Beat jump strip below loop controls (PRD-0015)
    if (beatJumpComponent != nullptr && isTrackLoaded())
    {
        auto beatJumpArea = bounds.removeFromBottom (beatJumpHeight);
        beatJumpComponent->setBounds (beatJumpArea);
        beatJumpComponent->setVisible (true);
    }
    else if (beatJumpComponent != nullptr)
    {
        beatJumpComponent->setVisible (false);
    }

    // Waveform component in remaining content area
    if (waveformComponent != nullptr && isTrackLoaded())
        waveformComponent->setBounds (bounds);
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
            // Set this deck as active and load
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
    // Root state property changed (e.g., activeDeckId)
    if (tree == rootState && property == IDs::activeDeckId)
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
            {
                safeThis->updateActiveState();
                safeThis->resized(); // update remove button state
            }
        });
        return;
    }

    // Deck tree property changed
    if (tree == deckTree)
    {
        if (property == IDs::playbackStatus || property == IDs::loadingStatus)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                {
                    if (safeThis->isTrackLoaded())
                    {
                        // Create TrackInfoComponent if not present
                        if (safeThis->trackInfoComponent == nullptr)
                        {
                            safeThis->trackInfoComponent = std::make_unique<TrackInfoComponent> (
                                safeThis->deckTree,
                                safeThis->deckStateManager,
                                safeThis->audioFileLoader,
                                safeThis->deckId);
                            safeThis->addAndMakeVisible (*safeThis->trackInfoComponent);
                        }
                    }
                    else
                    {
                        // Remove waveform and track info if track was ejected
                        if (safeThis->waveformComponent != nullptr)
                        {
                            safeThis->removeChildComponent (safeThis->waveformComponent.get());
                            safeThis->waveformComponent.reset();
                        }
                        if (safeThis->trackInfoComponent != nullptr)
                        {
                            safeThis->removeChildComponent (safeThis->trackInfoComponent.get());
                            safeThis->trackInfoComponent.reset();
                        }
                    }

                    safeThis->resized(); // update remove button state
                    safeThis->repaint();
                }
            });
        }
    }

    // Waveform analysis status changed
    if (tree.hasType (IDs::Waveform) && property == IDs::analysisStatus)
    {
        // Check if this waveform node belongs to our deck tree
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
                                if (safeThis != nullptr)
                                    safeThis->audioEngine.seekDeck (safeThis->deckId, pos);
                            };
                        }

                        safeThis->waveformComponent->setWaveformData (data);
                        safeThis->waveformComponent->setAudioState (
                            safeThis->deckStateManager.getAudioState (safeThis->deckId));
                        safeThis->waveformComponent->setDeckAccentColour (
                            getDeckAccentColour (safeThis->deckId));

                        // If BeatGrid analysis already completed, forward data now
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

                        // Forward existing hot cues to waveform
                        safeThis->updateWaveformHotCues();

                        safeThis->resized();
                        safeThis->repaint();
                    }
                }
            });
        }
    }

    // BeatGrid analysis status changed
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

                        // Forward beatgrid to hot cue manager for quantize snap
                        if (safeThis->hotCueManager != nullptr)
                            safeThis->hotCueManager->setBeatGridData (data);

                        // Forward beatgrid to loop engine for beat-accurate loops
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
