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
    trackInfoComponent->onBpmEditRequested = [this] (double bpm) { handleBpmSave (bpm); };
    addAndMakeVisible (*trackInfoComponent);

    // -----------------------------------------------------------------------
    // Stems row
    // -----------------------------------------------------------------------
    stemSeparateButton = std::make_unique<StemSeparateButton> (
        deckTree, stemSeparationManager, audioEngine, deckId);
    addAndMakeVisible (*stemSeparateButton);

    // PRD-0062: ORIG/STEMS source-mode selector. Visible alongside the VOC/INST
    // toggles once a ready stem set exists; chooses which source the deck plays.
    stemSourceModeToggle = std::make_unique<SourceModeToggleComponent> (
        deckTree, audioEngine, deckId);
    addAndMakeVisible (*stemSourceModeToggle);

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

    // ControllerWidget owns layout but not lifetime of loopControl.
    // hotCuePadComponent is managed directly by DeckShellComponent (Frame 55 row).
    controllerWidget = std::make_unique<ControllerWidget> (
        deckTree,
        loopControlComponent.get(),
        nullptr,              // hotCuePads placed directly in DeckShellComponent
        beatJumpComponent.get());

    // Wire transport callbacks
    controllerWidget->onCuePress  = [this] { handleCuePress(); };
    controllerWidget->onStopPress = [this] { handleStopPress(); };
    controllerWidget->onPlayPress = [this] { handlePlayPress(); };

    // Wire loop callbacks (LOOP mode size buttons)
    controllerWidget->onAutoLoop    = [this] (float b) { loopEngine->autoLoop (b); };
    controllerWidget->onLoopHalve   = [this]            { loopEngine->loopHalve(); };
    controllerWidget->onLoopDouble  = [this]            { loopEngine->loopDouble(); };

    // Wire jump callbacks (JUMP mode arrows)
    controllerWidget->onJumpForward  = [this] { beatJumpEngine->jumpForward(); };
    controllerWidget->onJumpBackward = [this] { beatJumpEngine->jumpBackward(); };

    // Wire beatgrid callbacks for the GRID mode
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

    // Hot cue pads — Frame 55, managed directly (not inside ControllerWidget)
    addAndMakeVisible (*hotCuePadComponent);

    // Initial stems sidebar overlay visibility
    updateStemsSidebarVisibility();

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

    // --- Update library_tracks.bpm BEFORE writing the ValueTree so that any
    //     listener-triggered query sees the fresh value already in the DB.
    {
        auto meta        = deckTree.getChildWithName (IDs::TrackMetadata);
        auto filePath    = meta.getProperty (IDs::filePath,    "").toString();
        auto contentHash = meta.getProperty (IDs::contentHash, "").toString();
        if (filePath.isNotEmpty() && contentHash.isNotEmpty())
            deckStateManager.getDatabase().updateLibraryTrackBpm (filePath, contentHash, newBpm);
    }

    // Update ValueTree (message thread — mandatory per CLAUDE.md)
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

    // Persist beatgrid JSON to track_data table
    persistBeatGridToDb();

    // Signal LibraryComponent to refresh: set a root-state property that
    // LibraryComponent::valueTreePropertyChanged listens for.
    rootState.setProperty (IDs::trackBpmManuallyChanged,
                           deckTree.getChildWithName (IDs::TrackMetadata)
                                   .getProperty (IDs::filePath, "").toString(),
                           nullptr);
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

// --- Stems sidebar overlay visibility ---

void DeckShellComponent::updateStemsSidebarVisibility()
{
    // When stems are ready, hide the SEPARATE overlay and reveal the
    // VOC/INST toggles beneath it.  In every other status (none, queued,
    // separating, error, model_unavailable, loading_cached) the SEPARATE
    // button takes over the whole sidebar so its state is visible.
    auto stemsTree = deckTree.getChildWithName (IDs::Stems);
    const auto status = stemsTree.isValid()
                            ? stemsTree.getProperty (IDs::status, "none").toString()
                            : juce::String ("none");

    const bool stemsReady = (status == "ready");

    if (stemSeparateButton != nullptr)
        stemSeparateButton->setVisible (! stemsReady);

    if (stemSourceModeToggle != nullptr) stemSourceModeToggle->setVisible (stemsReady);
    if (stemVocToggle  != nullptr) stemVocToggle->setVisible  (stemsReady);
    if (stemInstToggle != nullptr) stemInstToggle->setVisible (stemsReady);
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
    // The waveform sits between the stems sidebar and the pitch sidebar in
    // the main row (after header + control row).
    if (! isTrackLoaded())
    {
        const int waveformW = getWaveformW();
        auto contentBounds = getLocalBounds().reduced (kPad);
        auto waveArea = contentBounds
                            .withTrimmedTop (kHeaderH + kGap + kControlRowH + kGap)
                            .withTrimmedLeft (kStemsSidebarW + kSidebarGap)
                            .withWidth (waveformW)
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

    // CUE label button — active/dark tab at right of Frame 55 row
    if (! cueLabelBounds.isEmpty())
    {
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.fillRect (cueLabelBounds);
        g.drawRect (cueLabelBounds, 2);
        g.setColour (juce::Colour (0xFFF9F9F9));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawText ("CUE", cueLabelBounds, juce::Justification::centred);
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
    // Width helpers — adapt to the actual component width.
    // leftPanelW: MASTER row + (stems sidebar | waveform).  Matches Figma's
    //             474 px panel when the deck is exactly 592 px wide.
    // waveformW : waveform width inside the main row (between sidebars).
    const int leftPanelW = getLeftPanelW();
    const int waveformW  = getWaveformW();

    auto bounds = getLocalBounds().reduced (kPad);

    // --- Row 1: Deck Header (always visible, full content width) ---
    auto headerRow = bounds.removeFromTop (kHeaderH);
    if (trackInfoComponent != nullptr)
        trackInfoComponent->setBounds (headerRow);

    bounds.removeFromTop (kGap);

    // --- Row 2: MASTER / SYNC / QUANT / SLIP  |  KEY  (full content width) ---
    // KEY sits at the right edge in the pitch-sidebar column, directly under
    // the deck-identifier block in the header.
    auto controlRowFull = bounds.removeFromTop (kControlRowH);
    {
        auto keyArea     = controlRowFull.removeFromRight (kPitchSidebarW);
        controlRowFull.removeFromRight (kSidebarGap);
        auto controlRow  = controlRowFull;

        // Four buttons with three 10 px gaps (Figma spec)
        const int ctrlGap = 10;
        const int availW  = controlRow.getWidth() - 3 * ctrlGap;
        const int btnW    = availW / 4;
        const int lastBtnW = controlRow.getWidth() - 3 * (btnW + ctrlGap);

        if (masterButton != nullptr)
            masterButton->setBounds (controlRow.removeFromLeft (btnW));
        controlRow.removeFromLeft (ctrlGap);

        if (syncButton != nullptr)
            syncButton->setBounds (controlRow.removeFromLeft (btnW));
        controlRow.removeFromLeft (ctrlGap);

        if (quantizeButton != nullptr)
            quantizeButton->setBounds (controlRow.removeFromLeft (btnW));
        controlRow.removeFromLeft (ctrlGap);

        if (slipButton != nullptr)
            slipButton->setBounds (controlRow.removeFromLeft (lastBtnW));

        if (keyLockButton != nullptr)
            keyLockButton->setBounds (keyArea);
    }

    bounds.removeFromTop (kGap);

    // --- Row 3: Stems sidebar (23) | Waveform | Pitch sidebar (70) ---
    auto mainRow = bounds.removeFromTop (kMainH);
    {
        // Left: vertical stems sidebar (VOC top, INST bottom — rotated text)
        auto stemsSidebar = mainRow.removeFromLeft (kStemsSidebarW);
        mainRow.removeFromLeft (kSidebarGap);

        {
            // SeparateButton acts as a full-sidebar overlay; it lives in the
            // same bounds as the VOC/INST stack and self-renders based on
            // stem status (showing progress/state).  When stems are ready the
            // sidebar is split vertically into three equal cells: the
            // ORIG/STEMS source-mode toggle on top (PRD-0062), then VOC, then
            // INST, separated by 8 px gaps.
            static constexpr int kStemToggleGap = 8;
            const int totalH = stemsSidebar.getHeight();
            const int cellH  = (totalH - 2 * kStemToggleGap) / 3;
            const int instH  = totalH - 2 * (cellH + kStemToggleGap);

            if (stemSourceModeToggle != nullptr)
                stemSourceModeToggle->setBounds (stemsSidebar.getX(),
                                                 stemsSidebar.getY(),
                                                 kStemsSidebarW, cellH);

            if (stemVocToggle != nullptr)
                stemVocToggle->setBounds (stemsSidebar.getX(),
                                          stemsSidebar.getY() + cellH + kStemToggleGap,
                                          kStemsSidebarW, cellH);

            if (stemInstToggle != nullptr)
                stemInstToggle->setBounds (stemsSidebar.getX(),
                                           stemsSidebar.getY() + 2 * (cellH + kStemToggleGap),
                                           kStemsSidebarW,
                                           instH);

            // The Separate button overlays the sidebar.  When stems are ready
            // it visually renders as "STEMS READY" (dark fill); while
            // separating it shows a progress bar.  When not yet started it
            // shows the prompt and is the only interactive element here.
            if (stemSeparateButton != nullptr)
                stemSeparateButton->setBounds (stemsSidebar);
        }

        // Right: pitch sidebar
        auto pitchSidebar = mainRow.removeFromRight (kPitchSidebarW);
        mainRow.removeFromRight (kSidebarGap);

        // Middle: Waveform fills what's left
        if (waveformComponent != nullptr && isTrackLoaded())
            waveformComponent->setBounds (mainRow);

        // Time & Pitch sidebar layout — KEY now lives in the control row
        // above, so the sidebar only contains: stepper at top, then pitch
        // fader filling all remaining space (with ±% button rendered inside
        // pitchFaderComponent at its bottom 23 px).
        const int btnH    = 23;
        const int sideGap = 8;

        if (keyStepperComponent != nullptr)
            keyStepperComponent->setBounds (pitchSidebar.removeFromTop (btnH));

        pitchSidebar.removeFromTop (sideGap);

        if (pitchFaderComponent != nullptr)
            pitchFaderComponent->setBounds (pitchSidebar);

        juce::ignoreUnused (waveformW);  // already used implicitly via mainRow
    }

    bounds.removeFromTop (kBelowFrameGap);

    // --- Row 4: Controller Widget — LOOP strip, full content width ---
    auto controllerRow = bounds.removeFromTop (kControllerH);
    if (controllerWidget != nullptr)
        controllerWidget->setBounds (controllerRow.withWidth (leftPanelW + kSidebarGap + kPitchSidebarW));

    bounds.removeFromTop (kBelowFrameGap);

    // --- Row 5: Hot Cue Pads (1-9) | CUE label (Frame 55) ---
    auto cuePadsRow = bounds.removeFromTop (kCuePadsH);
    {
        auto cueTab = cuePadsRow.removeFromRight (kPitchSidebarW);
        cuePadsRow.removeFromRight (kSidebarGap);

        if (hotCuePadComponent != nullptr)
            hotCuePadComponent->setBounds (cuePadsRow);

        cueLabelBounds = cueTab;
    }
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

    // Stems status changed → toggle SEPARATE overlay vs VOC/INST toggles
    if (tree.hasType (IDs::Stems) && property == IDs::status)
    {
        if (tree.getParent() == deckTree)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->updateStemsSidebarVisibility();
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

                                auto* audioState =
                                    safeThis->deckStateManager.getAudioState (safeThis->deckId);

                                if (safeThis->scratchGestureActive && audioState != nullptr)
                                {
                                    // Velocity-based scratch (PRD-0016 fix):
                                    // Compute samples/sample velocity from the position
                                    // delta and elapsed time since the last drag event.
                                    // This lets the audio thread play CONTINUOUSLY at the
                                    // published velocity rather than teleporting to an
                                    // absolute target in one block and then going silent
                                    // until the next drag event fires.
                                    const int64_t nowMs = juce::Time::currentTimeMillis();
                                    const int64_t dtMs  = nowMs - safeThis->scratchPrevEventTimeMs;

                                    if (dtMs > 0)
                                    {
                                        const double sr = safeThis->audioEngine.getSampleRate();
                                        const double dtSamples =
                                            (static_cast<double> (dtMs) / 1000.0) * sr;
                                        const double delta =
                                            static_cast<double> (pos)
                                            - static_cast<double> (safeThis->scratchPrevTargetSample);
                                        const float velocity =
                                            static_cast<float> (delta / dtSamples);
                                        audioState->scratchVelocityPerSample.store (
                                            velocity, std::memory_order_relaxed);
                                        safeThis->scratchPrevEventTimeMs = nowMs;
                                    }
                                    // If dtMs == 0 (two events in the same millisecond),
                                    // skip the velocity update so we keep the previous
                                    // value; only advance the position anchor.

                                    safeThis->scratchPrevTargetSample = pos;
                                    audioState->scratchTargetSample.store (
                                        pos, std::memory_order_relaxed);
                                }
                                else
                                    safeThis->audioEngine.seekDeck (safeThis->deckId, pos);
                            };

                            // PRD-0016: Vinyl/CDJ-style scratch lifecycle on
                            // the detail waveform. We capture the prior
                            // playback state at press, then only apply
                            // "touch-stops-platter" when the deck was already
                            // playing. Releasing restores the prior state:
                            //   playing -> hold/drag -> playing (resumes)
                            //   paused  -> hold/drag -> paused  (never starts)
                            //   stopped -> hold/drag -> stopped (never starts)
                            safeThis->waveformComponent->onScratchBegin = [safeThis]
                            {
                                if (safeThis == nullptr)
                                    return;

                                auto* audioState =
                                    safeThis->deckStateManager.getAudioState (safeThis->deckId);

                                safeThis->scratchGestureActive = true;
                                safeThis->scratchPriorStatus =
                                    safeThis->deckTree
                                        .getProperty (IDs::playbackStatus, "paused")
                                        .toString();

                                if (audioState != nullptr)
                                {
                                    const auto currentSample =
                                        audioState->playheadPosition.load (
                                            std::memory_order_relaxed);
                                    audioState->scratchTargetSample.store (
                                        currentSample, std::memory_order_relaxed);
                                    // Initialise velocity to zero (stationary press = silence)
                                    // and seed the timing anchor for the first drag event.
                                    audioState->scratchVelocityPerSample.store (
                                        0.0f, std::memory_order_relaxed);
                                    safeThis->scratchPrevTargetSample = currentSample;
                                    safeThis->scratchPrevEventTimeMs  =
                                        juce::Time::currentTimeMillis();

                                    audioState->scratchActive.store (
                                        true, std::memory_order_release);
                                }

                                // Pointer-down is equivalent to touching a
                                // spinning vinyl: only a currently playing
                                // deck is stopped while held.
                                if (safeThis->scratchPriorStatus == "playing")
                                {
                                    safeThis->deckStateManager.setPlaybackStatus (
                                        safeThis->deckId, "paused");
                                }
                            };
                            safeThis->waveformComponent->onScratchEnd = [safeThis]
                            {
                                if (safeThis == nullptr)
                                    return;

                                if (auto* audioState =
                                        safeThis->deckStateManager.getAudioState (safeThis->deckId);
                                    audioState != nullptr)
                                {
                                    audioState->scratchActive.store (
                                        false, std::memory_order_release);
                                }

                                safeThis->scratchGestureActive = false;
                                const juce::String restoreTo =
                                    safeThis->scratchPriorStatus.isNotEmpty()
                                        ? safeThis->scratchPriorStatus
                                        : juce::String ("paused");
                                safeThis->deckStateManager.setPlaybackStatus (
                                    safeThis->deckId, restoreTo);
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

    // PRD-0039 AC-11: the file is present on disk. If the DB still says it's
    // missing (e.g. external drive reconnected during the session), reset the
    // flag and proceed with dispatch. The UPDATE is a no-op when is_missing=0.
    if (auto* dbHandle = deckStateManager.getDatabase().getDbHandle())
    {
        sqlite3_stmt* upd = nullptr;
        if (sqlite3_prepare_v2 (dbHandle,
                "UPDATE library_tracks SET is_missing=0 WHERE file_path=? AND is_missing=1;",
                -1, &upd, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (upd, 1, path.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_step (upd);
            sqlite3_finalize (upd);
        }
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

    // Auto-disengage SYNC and force-stop a currently playing deck (AC-15).
    // DeckStateManager::loadTrack refuses to write metadata while a deck is in
    // the "playing" state, so without this stop the new title/artist/key/BPM
    // never reach the ValueTree and the deck header keeps displaying the
    // previous track's info.
    if (deckTree.getProperty (IDs::playbackStatus).toString() == "playing")
    {
        deckTree.setProperty (IDs::syncEnabled, false, nullptr);
        deckStateManager.setPlaybackStatus (deckId, "stopped");
    }

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
    // PRD-0039 AC-15: no file format filter — AudioFileLoader validates at
    // dispatch time.
    auto fc = std::make_shared<juce::FileChooser> (
        "Choose replacement file",
        juce::File{},
        juce::String());

    fc->launchAsync (
        juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer (this), originalPath, fc] (const juce::FileChooser& ch)
        {
            const auto result = ch.getResult();
            if (! result.existsAsFile() || safeThis == nullptr)
                return;

            const juce::String newPath = result.getFullPathName();
            auto* handle = safeThis->deckStateManager.getDatabase().getDbHandle();

            // PRD-0039 AC-16 / AC-33: deduplication check against other rows.
            {
                sqlite3_stmt* dup = nullptr;
                bool isDuplicate = false;
                if (sqlite3_prepare_v2 (handle,
                        "SELECT 1 FROM library_tracks WHERE file_path=? AND file_path<>? LIMIT 1;",
                        -1, &dup, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text (dup, 1, newPath.toRawUTF8(),       -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text (dup, 2, originalPath.toRawUTF8(),  -1, SQLITE_TRANSIENT);
                    isDuplicate = (sqlite3_step (dup) == SQLITE_ROW);
                    sqlite3_finalize (dup);
                }

                if (isDuplicate)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Relocate Track",
                        "This file is already in your library.",
                        "OK",
                        safeThis);
                    return;
                }
            }

            // Persist new path to DB (AC-11)
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
    // PRD-0039 AC-21: nested confirmation before destructive delete.
    auto options = juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::WarningIcon)
        .withTitle ("Remove from Library")
        .withMessage ("This will also remove the track from all playlists. "
                      "This action cannot be undone.")
        .withButton ("Confirm Remove")
        .withButton ("Cancel")
        .withAssociatedComponent (this);

    juce::AlertWindow::showAsync (options,
        [safeThis = juce::Component::SafePointer (this), filePath] (int result)
        {
            if (safeThis == nullptr || result != 1)
                return;

            // PRD-0039 AC-23: atomic transaction — delete playlist_tracks first
            // then library_tracks. Both commit together or neither does.
            auto* handle = safeThis->deckStateManager.getDatabase().getDbHandle();
            const std::string ps = filePath.toStdString();

            bool ok = sqlite3_exec (handle, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;

            if (ok)
            {
                sqlite3_stmt* del = nullptr;
                if (sqlite3_prepare_v2 (handle,
                        "DELETE FROM playlist_tracks WHERE track_id IN "
                        "(SELECT id FROM library_tracks WHERE file_path=?);",
                        -1, &del, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text (del, 1, ps.c_str(), -1, SQLITE_TRANSIENT);
                    ok = (sqlite3_step (del) == SQLITE_DONE);
                    sqlite3_finalize (del);
                }
                else
                {
                    ok = false;
                }
            }

            if (ok)
            {
                sqlite3_stmt* del = nullptr;
                if (sqlite3_prepare_v2 (handle,
                        "DELETE FROM library_tracks WHERE file_path=?;",
                        -1, &del, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text (del, 1, ps.c_str(), -1, SQLITE_TRANSIENT);
                    ok = (sqlite3_step (del) == SQLITE_DONE);
                    sqlite3_finalize (del);
                }
                else
                {
                    ok = false;
                }
            }

            if (ok)
                sqlite3_exec (handle, "COMMIT;", nullptr, nullptr, nullptr);
            else
                sqlite3_exec (handle, "ROLLBACK;", nullptr, nullptr, nullptr);
        });
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
        if (controllerWidget != nullptr)
            controllerWidget->setActiveAutoLoopBeats (info.activeAutoBeats);
    }
}
