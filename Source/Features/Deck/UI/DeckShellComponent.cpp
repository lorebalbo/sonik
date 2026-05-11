#include "DeckShellComponent.h"
#include <sqlite3.h>

DeckShellComponent::DeckShellComponent (DeckStateManager& deckState,
                                        AudioEngine& engine,
                                        AudioFileLoader& loader,
                                        WaveformManager& waveformMgr,
                                        BeatGridManager& beatGridMgr,
                                        StemSeparationManager& stemMgr,
                                        MasterClockManager& clockMgr,
                                        const juce::String& id)
    : deckStateManager (deckState),
      audioEngine (engine),
      audioFileLoader (loader),
      waveformManager (waveformMgr),
      beatGridManager (beatGridMgr),
      stemSeparationManager (stemMgr),
      masterClockManager (clockMgr),
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
    // Compute deck index (A=0, B=1, C=2, D=3) for MasterButton
    const int deckIdx = (deckId == "A") ? 0 : (deckId == "B") ? 1 : (deckId == "C") ? 2 : 3;

    masterButton = std::make_unique<MasterButton> (deckTree, masterClockManager, deckIdx);
    addAndMakeVisible (*masterButton);

    syncButton = std::make_unique<SyncButton> (deckTree);
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
    controllerWidget->onBpmSave    = [this] (double bpm) { handleBpmSave (bpm); };
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

    // Use the track's analysis sample rate for a tempo-independent ms offset.
    auto bgData = beatGridManager.getBeatGridData (deckId);
    double sr = 44100.0;
    if (bgData != nullptr)
    {
        sr = bgData->analysisSampleRate;
    }
    else
    {
        auto meta = deckTree.getChildWithName (IDs::TrackMetadata);
        sr = static_cast<double> (meta.getProperty (IDs::sampleRate, 44100.0));
    }

    int64_t anchor   = static_cast<int64_t> (
        static_cast<double> (beatTree.getProperty (IDs::anchorSample, 0)));
    int64_t interval = static_cast<int64_t> (
        static_cast<double> (beatTree.getProperty (IDs::beatIntervalSamples, 0)));

    if (interval <= 0) return;

    // fine (|delta|==1) → ±10 ms,  coarse (|delta|==2) → ±50 ms
    bool    isFine        = (std::abs (delta) == 1);
    int64_t offsetSamples = isFine
        ? static_cast<int64_t> (std::round (sr * 0.010))
        : static_cast<int64_t> (std::round (sr * 0.050));

    if (delta < 0) offsetSamples = -offsetSamples;

    // Wrap anchorSample into [0, beatIntervalSamples)
    int64_t newAnchor = ((anchor + offsetSamples) % interval + interval) % interval;

    // Update ValueTree (message thread)
    beatTree.setProperty (IDs::anchorSample,     static_cast<double> (newAnchor), nullptr);
    beatTree.setProperty (IDs::manuallyAdjusted, true,                            nullptr);

    // Propagate to in-memory BeatGridData so engines see the change immediately
    if (bgData != nullptr)
    {
        bgData->anchorSample     = newAnchor;
        bgData->manuallyAdjusted = true;
    }

    // Refresh the waveform beatgrid overlay
    if (waveformComponent != nullptr && bgData != nullptr)
        waveformComponent->setBeatGridData (bgData);

    // Persist new anchor to SQLite
    persistBeatGridToDb();
}

void DeckShellComponent::handleBpmSave (double newBpm)
{
    // Validate range
    if (newBpm < 20.0 || newBpm > 300.0) return;

    auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
    if (! beatTree.isValid()) return;

    // Determine the analysis sample rate (preserved in BeatGridData)
    auto bgData = beatGridManager.getBeatGridData (deckId);
    double sr = 44100.0;
    if (bgData != nullptr)
        sr = bgData->analysisSampleRate;
    else
    {
        auto meta = deckTree.getChildWithName (IDs::TrackMetadata);
        sr = static_cast<double> (meta.getProperty (IDs::sampleRate, 44100.0));
    }

    double newInterval = sr * 60.0 / newBpm;

    // Update ValueTree (message thread — mandatory per AGENTS.md)
    beatTree.setProperty (IDs::bpm,                 newBpm,      nullptr);
    beatTree.setProperty (IDs::beatIntervalSamples, newInterval, nullptr);
    beatTree.setProperty (IDs::manuallyAdjusted,    true,        nullptr);

    // Propagate to in-memory BeatGridData so all beat-aware engines see the change
    if (bgData != nullptr)
    {
        bgData->bpm                 = newBpm;
        bgData->beatIntervalSamples = newInterval;
        bgData->manuallyAdjusted    = true;
    }

    // Re-render waveform beatgrid overlay immediately
    if (waveformComponent != nullptr && bgData != nullptr)
        waveformComponent->setBeatGridData (bgData);

    // Persist to SQLite
    persistBeatGridToDb();
}

void DeckShellComponent::persistBeatGridToDb()
{
    auto meta        = deckTree.getChildWithName (IDs::TrackMetadata);
    auto filePath    = meta.getProperty (IDs::filePath,    "").toString();
    auto contentHash = meta.getProperty (IDs::contentHash, "").toString();
    if (filePath.isEmpty() || contentHash.isEmpty()) return;

    // Build beatgrid JSON from in-memory data when available; fall back to ValueTree
    juce::String beatgridJson;
    auto bgData = beatGridManager.getBeatGridData (deckId);
    if (bgData != nullptr)
    {
        beatgridJson = bgData->toJson();
    }
    else
    {
        auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
        if (beatTree.isValid())
        {
            double sr = static_cast<double> (meta.getProperty (IDs::sampleRate, 44100.0));
            auto   obj = std::make_unique<juce::DynamicObject>();
            obj->setProperty ("bpm",                static_cast<double> (beatTree.getProperty (IDs::bpm, 0.0)));
            obj->setProperty ("anchorSample",       static_cast<double> (beatTree.getProperty (IDs::anchorSample, 0)));
            obj->setProperty ("beatIntervalSamples",static_cast<double> (beatTree.getProperty (IDs::beatIntervalSamples, 0.0)));
            obj->setProperty ("confidence",         static_cast<double> (beatTree.getProperty (IDs::confidence, 0.0)));
            obj->setProperty ("manuallyAdjusted",   static_cast<bool>   (beatTree.getProperty (IDs::manuallyAdjusted, false)));
            obj->setProperty ("sampleRate",         sr);
            beatgridJson = juce::JSON::toString (juce::var (obj.release()));
        }
    }
    if (beatgridJson.isEmpty()) return;

    auto& db = deckStateManager.getDatabase();

    // Load existing record to preserve cue points and key-detection data
    auto existing     = db.loadTrackData (filePath, contentHash);
    juce::String cuePointsJson;
    int   keyIndex  = -1;
    float keyConf   = 0.0f;
    bool  keyManual = false;
    if (existing.has_value())
    {
        cuePointsJson = existing->cuePointsJson;
        keyIndex      = existing->keyIndex;
        keyConf       = existing->keyConfidence;
        keyManual     = existing->keyManuallyAdjusted;
    }

    db.saveTrackData (filePath, contentHash, cuePointsJson, beatgridJson,
                      keyIndex, keyConf, keyManual);
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

    // Drag overlay — binary-invert the header region only (AC-02 / DESIGN.md §2.1)
    if (isDragOver)
    {
        const auto headerBounds = getLocalBounds().reduced (kPad).withHeight (kHeaderH);
        g.setColour (juce::Colour (0xFF000000));
        g.fillRect (headerBounds);
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

    // --- Row 4: Control row (MASTER / SYNC / QUANT / SLIP) ---
    auto controlRow = bounds.removeFromTop (kControlRowH);
    controlRow = controlRow.withWidth (panelW);
    {
        // Four buttons with three gaps of 10 px
        const int ctrlGap  = 10;
        const int availW   = controlRow.getWidth() - 3 * ctrlGap;
        const int masterW_ = availW / 4;
        const int syncW_   = availW / 4;
        const int quantW_  = availW / 4;
        const int slipW_   = availW - masterW_ - syncW_ - quantW_;

        if (masterButton != nullptr)
            masterButton->setBounds (controlRow.removeFromLeft (masterW_));
        controlRow.removeFromLeft (ctrlGap);

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

    // Route through pendingLoadPath so format/existence checks happen in one place.
    for (const auto& f : files)
    {
        juce::File file (f);
        if (AudioFileLoader::isSupportedExtension (file.getFileExtension()))
        {
            deckTree.setProperty (IDs::pendingLoadPath, f, nullptr);
            break;
        }
    }
}

// --- In-app DragAndDropTarget (library table) ---

bool DeckShellComponent::isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details)
{
    return details.description.toString().isNotEmpty();
}

void DeckShellComponent::itemDragEnter (const juce::DragAndDropTarget::SourceDetails&)
{
    isDragOver = true;
    repaint();
}

void DeckShellComponent::itemDragExit (const juce::DragAndDropTarget::SourceDetails&)
{
    isDragOver = false;
    repaint();
}

void DeckShellComponent::itemDropped (const juce::DragAndDropTarget::SourceDetails& details)
{
    isDragOver = false;
    repaint();

    const auto filePath = details.description.toString()
                              .upToFirstOccurrenceOf ("\n", false, false)
                              .trim();
    if (filePath.isNotEmpty())
        deckTree.setProperty (IDs::pendingLoadPath, filePath, nullptr);
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
        if (property == IDs::pendingLoadPath)
        {
            const auto path = tree.getProperty (IDs::pendingLoadPath).toString();
            if (path.isEmpty()) return;
            // Clear immediately to prevent double-fire before the async work runs.
            tree.setProperty (IDs::pendingLoadPath, "", nullptr);
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this), path]()
            {
                if (safeThis != nullptr)
                    safeThis->handlePendingLoad (path);
            });
            return;
        }

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

// --- Track loading via pendingLoadPath (PRD-0034) ---

void DeckShellComponent::handlePendingLoad (const juce::String& path)
{
    juce::File file (path);

    if (! file.existsAsFile())
    {
        // Show Relocate / Remove dialog (AC-10)
        auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::WarningIcon,
            "File Not Found",
            "The track file could not be found:\n\n" + path,
            "Relocate",
            "Remove from Library");

        juce::AlertWindow::showAsync (
            opts,
            [safeThis = juce::Component::SafePointer (this), path] (int result)
            {
                if (safeThis == nullptr) return;
                if (result == 1)
                    safeThis->showRelocateDialog (path);
                else
                    safeThis->removeTrackFromLibrary (path);
            });
        return;
    }

    // Format check — reject unsupported extensions (AC-13)
    if (! AudioFileLoader::isSupportedExtension (file.getFileExtension()))
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Format Not Supported",
            "The file format \"" + file.getFileExtension() + "\" is not supported.",
            "OK",
            this);
        return;
    }

    // Auto-disengage SYNC if the deck is currently playing (AC-15)
    if (deckTree.getProperty (IDs::playbackStatus).toString() == "playing")
        deckTree.setProperty (IDs::syncEnabled, false, nullptr);

    // Make this deck active
    deckStateManager.setActiveDeck (deckId);

    // Kick off the async decode
    audioFileLoader.loadFile (deckId, file);

    // Record the loaded path so LibraryComponent can track play-count and
    // loadFocusedDeckTrack can detect empty decks (AC-08, AC-16).
    deckTree.setProperty (IDs::loadedFilePath, path, nullptr);
}

void DeckShellComponent::showRelocateDialog (const juce::String& originalPath)
{
    auto fc = std::make_shared<juce::FileChooser> (
        "Relocate Track",
        juce::File{},
        "*.mp3;*.flac;*.wav;*.aiff;*.aif;*.ogg;*.m4a");

    fc->launchAsync (
        juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer (this), originalPath, fc] (const juce::FileChooser& ch)
        {
            const auto result = ch.getResult();
            if (! result.existsAsFile() || safeThis == nullptr)
                return;

            const juce::String newPath = result.getFullPathName();

            // Persist new path to DB (AC-11)
            auto* handle = safeThis->deckStateManager.getDatabase().getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2 (handle,
                    "UPDATE library_tracks SET file_path=?, is_missing=0 WHERE file_path=?",
                    -1, &stmt, nullptr) == SQLITE_OK)
            {
                const std::string newPs  = newPath.toStdString();
                const std::string origPs = originalPath.toStdString();
                sqlite3_bind_text (stmt, 1, newPs.c_str(),  -1, SQLITE_TRANSIENT);
                sqlite3_bind_text (stmt, 2, origPs.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step      (stmt);
                sqlite3_finalize  (stmt);
            }

            // Proceed with the load using the new path
            safeThis->handlePendingLoad (newPath);
        });
}

void DeckShellComponent::removeTrackFromLibrary (const juce::String& filePath)
{
    // AC-12: delete from library_tracks and do not load anything
    auto* handle = deckStateManager.getDatabase().getDbHandle();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
            "DELETE FROM library_tracks WHERE file_path=?",
            -1, &stmt, nullptr) == SQLITE_OK)
    {
        const std::string ps = filePath.toStdString();
        sqlite3_bind_text (stmt, 1, ps.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step      (stmt);
        sqlite3_finalize  (stmt);
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
