#include "SonikApplication.h"

#include "Features/Daw/Session/Ui/SessionDialogs.h"
#include "Features/Daw/Session/Ui/UnresolvedSourcesDialog.h"
#include "Features/Daw/Session/SessionSchema.h"
#include "Features/Daw/Export/Ui/ExportDialog.h"
#include "Features/Daw/Export/ExportJobBuilder.h"

namespace
{
struct AsyncCompletion final
{
    juce::WaitableEvent event;
    std::atomic<bool> finished { false };
    std::atomic<bool> success  { false };
};

bool waitForAsyncCompletion (const LibraryAnalysisQueue::JobContext& ctx,
                             const std::shared_ptr<AsyncCompletion>& completion)
{
    while (! completion->finished.load (std::memory_order_acquire))
    {
        if (ctx.shouldExit && ctx.shouldExit())
        {
            if (ctx.sharedCancel != nullptr)
                ctx.sharedCancel->store (true, std::memory_order_release);
            return false;
        }

        completion->event.wait (100);
    }

    return completion->success.load (std::memory_order_acquire);
}

LibraryAnalysisQueue::JobExecutor makeAnalysisExecutor (LibraryAnalysisService& analysisService)
{
    return [&analysisService] (const LibraryAnalysisQueue::JobContext& ctx)
    {
        auto completion = std::make_shared<AsyncCompletion>();
        auto progressFn = ctx.progress;

        analysisService.analyzeTrack (ctx.filePath, ctx.contentHash,
            [completion] (const juce::String&, bool succeeded)
            {
                completion->success.store (succeeded, std::memory_order_release);
                completion->finished.store (true, std::memory_order_release);
                completion->event.signal();
            },
            ctx.sharedCancel,
            [onProgress = std::move (progressFn)] (int percent) mutable
            {
                if (onProgress)
                    onProgress (percent);
            });

        return waitForAsyncCompletion (ctx, completion);
    };
}

LibraryAnalysisQueue::JobExecutor makeStemExecutor (StemSeparationManager& stemManager)
{
    return [&stemManager] (const LibraryAnalysisQueue::JobContext& ctx)
    {
        auto completion = std::make_shared<AsyncCompletion>();

        stemManager.startSeparationForFile (ctx.filePath, ctx.contentHash, ctx.sharedCancel,
            [completion] (bool success)
            {
                completion->success.store (success, std::memory_order_release);
                completion->finished.store (true, std::memory_order_release);
                completion->event.signal();
            });

        return waitForAsyncCompletion (ctx, completion);
    };
}
} // namespace

void SonikApplication::initialise (const juce::String& /*commandLine*/)
{
    // Install the DESIGN.md LookAndFeel before any UI exists so every popup
    // menu and combo box in the app inherits the monochrome 1-bit chrome.
    lookAndFeel = std::make_unique<sonik::ui::SonikLookAndFeel>();
    juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

    // One desktop-level tooltip window for the whole app (700 ms hover delay).
    tooltipWindow = std::make_unique<juce::TooltipWindow> (nullptr, 700);

    auto dbPath = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Application Support")
                      .getChildFile ("Sonik")
                      .getChildFile ("sonik.db");

    trackDatabase    = std::make_unique<TrackDatabase> (dbPath);

    // MIDI I/O subsystem (PRD-0040) — constructed before any feature that
    // may later subscribe to MIDI events. Enumeration + hot-plug only;
    // no devices are opened automatically at this stage.
    midiHost          = std::make_unique<sonik::midi::JuceMidiHost>();
    midiDeviceManager = std::make_unique<sonik::midi::MidiDeviceManager> (*midiHost);
    midiDeviceManager->initialise();

    // Attach diagnostic logger so manual hot-plug testing is observable.
    midiDiagnosticLogger = std::make_unique<MidiDiagnosticLogger>();
    midiDiagnosticLogger->manager = midiDeviceManager.get();
    midiDeviceManager->addDeviceListChangeListener (midiDiagnosticLogger.get());
    {
        const auto initial = midiDeviceManager->getDevices();
        DBG ("[MIDI] Startup enumeration: " << (int) initial.size() << " device(s)");
        for (const auto& d : initial)
            DBG ("[MIDI]   id=" << juce::String::toHexString ((juce::int64) d.deviceId)
                 << "  " << (d.isInput ? "IN " : "OUT")
                 << "  mfg='"   << d.manufacturer
                 << "' name='" << d.productName
                 << "' ordinal=" << d.ordinal
                 << " connected=" << (d.isConnected ? 1 : 0));
    }

    deckStateManager = std::make_unique<DeckStateManager> (*trackDatabase);

    deckStateManager->restoreSession();

    // Ensure at least 2 decks exist after restore
    while (deckStateManager->getDeckCount() < 2)
        deckStateManager->addDeck();

    // PRD-0052: Mixer state schema — adds the "Mixer" sub-tree to the root
    // state tree and populates it with documented defaults.
    mixerStateSchema    = std::make_unique<MixerStateSchema> (deckStateManager->getStateTree());
    mixerAtomicSnapshot = std::make_unique<MixerAtomicSnapshot>();
    mixerMeterSnapshot  = std::make_unique<MixerMeterSnapshot>();
    mixerStateBridge    = std::make_unique<MixerStateBridge> (*mixerStateSchema, *mixerAtomicSnapshot);

    // PRD-0063: DAW state schema — attaches the empty `daw` branch (parallel to
    // decks/mixer) to the root state tree. Message-thread only; tracks are
    // created lazily later (PRD-0069). Idempotent get-or-create, like the mixer.
    dawStateTree = DawState::getOrCreateDawBranch (deckStateManager->getStateTree());

    // Create the master clock publisher and manager (PRD-0026).
    // Manager must be created after decks exist so its initial listener state is correct.
    masterClockPublisher = std::make_unique<MasterClockPublisher>();
    masterClockManager   = std::make_unique<MasterClockManager> (
        deckStateManager->getStateTree(), *masterClockPublisher);

    // Create and start the audio engine
    audioEngine = std::make_unique<AudioEngine> (deckStateManager->getStateTree());

    // PRD-0064/0066: master grid service feeding the DAW panel. Reads the master
    // clock SeqLock and the live audio device sample rate (message thread only).
    masterGridService = std::make_unique<Daw::MasterGridService> (
        *masterClockPublisher,
        [this]() { return audioEngine != nullptr ? audioEngine->getSampleRate() : 0.0; });

    // PRD-0041: RT-safe MIDI message bridge. Constructed after the device
    // manager (producers exist) and before the audio engine begins
    // processing (the audio thread will drain the FIFO each callback once
    // PRD-0044 wires producers to it).
    midiMessageBridge = std::make_unique<sonik::midi::MidiMessageBridge>();
    audioEngine->setMidiMessageBridge (midiMessageBridge.get());

    // PRD-0043: Mapping storage. Loads bundled profiles synchronously and
    // enumerates user profiles from
    // ~/Library/Application Support/Sonik/MidiMappings on a worker thread.
    // Constructor creates the user directory if it does not already exist.
    mappingStore = std::make_unique<sonik::midi::MappingStore> (*midiDeviceManager);

    // PRD-0045: Soft-takeover (pickup mode) tracker. Listens to the root
    // state tree for non-MIDI parameter changes (track load, mouse moves) so
    // the next hardware sample must cross the new value before it engages.
    softTakeoverManager = std::make_unique<sonik::midi::SoftTakeoverManager> (
        deckStateManager->getStateTree(), *mappingStore);

    // PRD-0044: Inbound MIDI command dispatch. The router subscribes to
    // MidiDeviceManager + MappingStore and forwards to the composite handler
    // on the Message thread (audio-thread paths go via the bridge FIFO).
    deckMidiHandler      = std::make_unique<DeckMidiHandler> (*deckStateManager, *softTakeoverManager);
    mixerMidiHandler     = std::make_unique<MixerMidiHandler> (softTakeoverManager.get());
    mixerMidiHandler->setStateTree (deckStateManager->getStateTree());
    mixerMidiHandler->setMixerStateSchema (mixerStateSchema.get());

    // PRD-0053: Wire the mixer atomic snapshot into the audio engine so the
    // audio thread can read channel-strip and master parameters each block.
    audioEngine->setMixerAtomicSnapshot (mixerAtomicSnapshot.get());

    // PRD-0058: Wire the metering snapshot so the audio thread can publish
    // per-channel + master peak/peakHold/RMS/clip every block.
    audioEngine->setMixerMeterSnapshot (mixerMeterSnapshot.get());
    libraryMidiHandler   = std::make_unique<LibraryMidiHandler>();
    compositeMidiHandler = std::make_unique<CompositeMidiCommandHandler> (
        *deckMidiHandler, *mixerMidiHandler, *libraryMidiHandler);
    midiInboundRouter = std::make_unique<sonik::midi::MidiInboundRouter> (
        *midiDeviceManager, *midiMessageBridge, *mappingStore, *compositeMidiHandler);

    // TODO: per-device opt-in auto-open policy belongs to a follow-up PRD.
    // Until then, open every enumerated endpoint so inbound events flow and
    // outbound LED feedback can reach hardware.
    for (const auto& dev : midiDeviceManager->getDevices())
    {
        if (dev.isInput)
            midiDeviceManager->openInput (dev.deviceId);
        else
            midiDeviceManager->openOutput (dev.deviceId);
    }

    // PRD-0047: MIDI output and LED feedback engine.
    midiFeedbackEngine = std::make_unique<sonik::midi::MidiFeedbackEngine> (
        deckStateManager->getStateTree(),
        *midiDeviceManager,
        *mappingStore,
        *softTakeoverManager);

    // Inject the master clock publisher into every deck slot (PRD-0026).
    audioEngine->setMasterClockPublisher (masterClockPublisher.get());

    auto decksNode = deckStateManager->getStateTree().getChildWithName (IDs::Decks);
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto deckTree = decksNode.getChild (i);
        auto deckId   = deckTree.getProperty (IDs::id).toString();
        auto* state   = deckStateManager->getAudioState (deckId);

        if (state != nullptr)
            audioEngine->registerDeck (deckId, state);
    }

    audioEngine->start();

    // Create the file loader (uses engine's sample rate)
    audioFileLoader = std::make_unique<AudioFileLoader> (
        *deckStateManager, *audioEngine, audioEngine->getSampleRate());

    // Create the waveform manager (PRD-0006)
    waveformManager = std::make_unique<WaveformManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the beat grid manager (PRD-0008)
    beatGridManager = std::make_unique<BeatGridManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the key detection manager (PRD-0009)
    keyDetectionManager = std::make_unique<KeyDetectionManager> (
        *deckStateManager, *trackDatabase, *audioEngine);

    // Create the model manager for stem separation (PRD-0019)
    // Validates model file and discovers Python on a background thread.
    modelManager = std::make_unique<ModelManager> (
        deckStateManager->getStateTree());

    // Create the stem separation manager (PRD-0020)
    stemSeparationManager = std::make_unique<StemSeparationManager> (
        *deckStateManager, *trackDatabase,
        *modelManager, *audioEngine);

    // Analyzes separated stems into waveform peaks so each DAW stem clip can draw
    // its own audio content (instrumental vs vocal) instead of the original track.
    stemWaveformAnalyzer = std::make_unique<WaveformAnalyzer> (*trackDatabase);

    // Wire stem-ready callback to deliver stems to audio engine (PRD-0021)
    stemSeparationManager->setStemReadyCallback (
        [this] (const juce::String& deckId, StemData::Ptr stems)
        {
            if (audioEngine != nullptr && stems != nullptr)
            {
                audioEngine->setDeckStemBuffers (
                    deckId,
                    stems->stems[StemData::Vocals],
                    stems->stems[StemData::Drums],
                    stems->stems[StemData::Bass],
                    stems->stems[StemData::Other]);
            }

            // Generate per-stem waveform peaks keyed by the lane-qualified id so a
            // clip on the Instrumental / Vocal lane resolves its own peaks (the
            // ClipBlock requests these same keys). Stored under composite keys; the
            // analyzer no-ops when peaks are already cached (idempotent on reload).
            if (stemWaveformAnalyzer == nullptr || stems == nullptr
                || deckStateManager == nullptr)
                return;

            auto deckTree = deckStateManager->getDeckState (deckId);
            const juce::String hash = deckTree.getChildWithName (IDs::TrackMetadata)
                                              .getProperty (IDs::contentHash).toString();
            if (hash.isEmpty())
                return;

            if (auto vocals = stems->getVocals())
                stemWaveformAnalyzer->analyze (
                    Daw::stemWaveformKey (hash, "Vocal"), vocals, {});

            // Instrumental = drums + bass + other, matching the playback mix.
            std::vector<AudioBufferHolder::Ptr> instrumental {
                stems->getDrums(), stems->getBass(), stems->getOther() };
            stemWaveformAnalyzer->analyzeSum (
                Daw::stemWaveformKey (hash, "Instrumental"),
                std::move (instrumental), {});
        });

    libraryAnalysisService = std::make_unique<LibraryAnalysisService> (*trackDatabase);
    libraryAnalysisQueue = std::make_unique<LibraryAnalysisQueue> (
        makeAnalysisExecutor (*libraryAnalysisService),
        makeStemExecutor (*stemSeparationManager));

    mainWindow = std::make_unique<MainWindow> (
        *audioFileLoader, *deckStateManager, *audioEngine, *waveformManager,
        *beatGridManager, *stemSeparationManager, *masterClockManager,
        *libraryAnalysisQueue, *trackDatabase,
        *mixerStateSchema, *mixerMeterSnapshot, *masterGridService);

    // Create the watch-folder scanner (PRD-0031) and kick off the startup scan.
    // Constructed after trackDatabase is ready; destroyed in shutdown() before
    // trackDatabase is reset. Created AFTER mainWindow so the LibraryComponent
    // exists and can register as a listener.
    watchFolderScanner = std::make_unique<WatchFolderScanner> (*trackDatabase);

    // Wire the scanner to the LibraryComponent listener interface.
    if (auto* content = mainWindow->getContent())
        content->registerScannerWithLibrary (*watchFolderScanner);

    // PRD-0048: wire toolbar MIDI button.
    if (auto* content = mainWindow->getContent())
        content->setOnMidiClicked ([this]() { openMidiSettingsWindow(); });

    // Wire AudioEngine into DeckMidiHandler (needed for TransportCue, PositionSeek).
    if (deckMidiHandler != nullptr && audioEngine != nullptr)
        deckMidiHandler->setAudioEngine (audioEngine.get());

    // Wire MixerMidiHandler to the root state tree.
    if (mixerMidiHandler != nullptr)
        mixerMidiHandler->setStateTree (deckStateManager->getStateTree());

    // Wire DeckLayoutManager → DeckMidiHandler so deck engines are registered.
    // This also retroactively registers the engines for all decks created
    // during mainWindow construction.
    if (auto* content = mainWindow->getContent())
        content->getLayoutManager().setDeckMidiHandler (deckMidiHandler.get());

    // Wire LibraryMidiHandler callbacks to LibraryComponent.
    if (auto* content = mainWindow->getContent())
    {
        if (auto* lib = content->getLibraryComponent())
        {
            libraryMidiHandler->onScrollUp    = [lib]() { lib->scrollLibraryUp(); };
            libraryMidiHandler->onScrollDown  = [lib]() { lib->scrollLibraryDown(); };
            libraryMidiHandler->onFocusSearch = [lib]() { lib->focusSearch(); };
            libraryMidiHandler->onLoadDeck    = [lib] (int idx) { lib->loadSelectedTrackToDeck (idx); };
            libraryMidiHandler->onBrowse      = [lib] (int steps)
            {
                for (int i = 0, n = std::abs (steps); i < n; ++i)
                {
                    if (steps > 0) lib->scrollLibraryDown();
                    else           lib->scrollLibraryUp();
                }
            };
        }
    }

    // EPIC-0012 / PRD-0096: build the session lifecycle (controller + File menu
    // + shortcuts) now that the live `daw` branch and the DAW panel exist.
    buildSessionLifecycle();

    watchFolderScanner->startScan();
}

//==============================================================================
// PRD-0096: session lifecycle wiring. Constructs the shared ApplicationProperties
// (recents storage), the serializer, the SessionController with a fully-
// implemented Host, and the File-menu command surface.
//==============================================================================
void SonikApplication::buildSessionLifecycle()
{
    if (mainWindow == nullptr)
        return;

    // Shared application properties: the SAME store/pattern the Library uses
    // (juce::ApplicationProperties keyed to the "Sonik" app), so recents live
    // alongside other app preferences rather than in a bespoke file (§1.5.3).
    appProperties = std::make_unique<juce::ApplicationProperties>();
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Sonik";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Sonik";
        opts.osxLibrarySubFolder = "Application Support";
        appProperties->setStorageParameters (opts);
    }

    sessionSerializer = std::make_unique<Daw::Session::SessionSerializer>();

    // The live `daw` branch (single source of truth) the controller serialises
    // and swaps into; the DAW UndoManager owned by the panel (PRD-0083).
    auto& dawPanel = mainWindow->getContent()->getDawPanel();

    // PRD-0097: the source-id resolution integration. It binds the resolver's
    // strategies to the real library DB, filesystem, and stem manager, runs the
    // resolution pass at session open (the SEAM below), writes the per-clip
    // `missingSource` flag into the live daw tree (driving the Glitch treatment
    // and snapshot exclusion), and exposes the play/export gating queries.
    sourceResolution = std::make_unique<Daw::Session::SessionSourceResolution> (
        DawState::getOrCreateDawBranch (deckStateManager->getStateTree()),
        *trackDatabase,
        *stemSeparationManager);
    sourceResolution->setRecompileTrigger (dawPanel.getRecompileTrigger());

    Daw::Session::SessionController::Host host;

    //--- captureMetadata: gather the master grid/tempo reference, the project
    // sample rate, and the live view state (zoom + scroll) at save time. ------
    host.captureMetadata = [this, &dawPanel]() -> Daw::Session::SessionMetadata
    {
        Daw::Session::SessionMetadata md;
        md.projectSampleRate = DawState::kProjectSampleRate;
        md.appVersion        = getApplicationVersion();

        if (masterGridService != nullptr)
        {
            const auto grid = masterGridService->snapshotGrid();
            md.masterGrid.bpm                = grid.bpm;
            md.masterGrid.downbeatSample     = grid.phaseOriginSample;
            md.masterGrid.timeSigNumerator   = DawState::kBeatsPerBar;
            md.masterGrid.timeSigDenominator = 4;
        }

        // View chrome (§1.5.5): persisted resolution-independently.
        md.viewState.zoomSamplesPerPixel = dawPanel.captureViewZoomSamplesPerPixel();
        md.viewState.scrollStartSample   = dawPanel.captureViewScrollStartSample();

        // PRD-0097: SOURCE_REFS relocation hints — one per distinct sourceFileId,
        // carrying source kind + last-known path so a reopened (or relocated-
        // machine) session can resolve each source through its kind-appropriate
        // strategy even before a library scan completes.
        if (sourceResolution != nullptr)
            md.sourceRefs = sourceResolution->buildSourceRefs();

        // PRD-0098: enrich the SOURCE_REFS table with the imported sources'
        // last-known path + display name. The session resolver classifies an
        // "import:" id as External but cannot recover its path (the DB does not
        // know it), so overlay the registry's richer hint for every imported id
        // that is still referenced by a clip in the table. This is what lets a
        // reopened session re-resolve / relocate the external file (PRD-0097).
        if (importRegistry != nullptr && md.sourceRefs.isValid())
        {
            const auto importRefs = importRegistry->toSourceRefs();
            for (int i = 0; i < md.sourceRefs.getNumChildren(); ++i)
            {
                auto ref = md.sourceRefs.getChild (i);
                const auto id = ref.getProperty (
                    Daw::Session::IDs::sourceFileId).toString();

                for (int j = 0; j < importRefs.getNumChildren(); ++j)
                {
                    auto src = importRefs.getChild (j);
                    if (src.getProperty (Daw::Session::IDs::sourceFileId).toString() == id)
                    {
                        ref.setProperty (Daw::Session::IDs::lastKnownPath,
                                         src.getProperty (Daw::Session::IDs::lastKnownPath), nullptr);
                        ref.setProperty (Daw::Session::IDs::displayName,
                                         src.getProperty (Daw::Session::IDs::displayName), nullptr);
                        ref.setProperty (Daw::Session::IDs::sourceKind,
                                         Daw::Session::SourceKindStrings::kExternal, nullptr);
                        break;
                    }
                }
            }
        }

        return md;
    };

    //--- onSessionOpened: the controller has already swapped the loaded daw tree
    // into the live model (in place, preserving node identity, so the DAW UI's
    // ValueTree listeners rebuild automatically). Here we restore the persisted
    // view state AFTER layout has settled so the scroll targets laid-out
    // components (§1.5.5).
    //
    // PRD-0097 SEAM: missing-source resolution is invoked HERE but owned by
    // PRD-0097. The marked block below is the single call site where the source
    // resolver will be run over `doc.sourceRefs` before/while the view restores;
    // it is intentionally left as a no-op hook for that PRD.
    host.onSessionOpened = [this, &dawPanel] (const Daw::Session::SessionDocument& doc)
    {
        //========================== PRD-0097 SEAM ==========================
        // Run the missing-source resolution pass over the (already-swapped) live
        // daw branch using the loaded SOURCE_REFS hint table. This:
        //   - resolves each DISTINCT sourceFileId once through the ordered
        //     strategy (library DB id -> stored path -> content hash; stems via
        //     the stem cache),
        //   - writes the per-clip `missingSource` flag into the daw tree
        //     (Missing-source clips render with the Glitch treatment), and
        //   - requests a recompile so the EPIC-0010 snapshot is built for
        //     Resolved-source clips ONLY (the engine never sees a Missing source).
        // This runs BEFORE the arrangement-snapshot recompile that view-state
        // restore would otherwise race, because the recompile it requests is the
        // authoritative resolved-only compile.
        if (sourceResolution != nullptr)
        {
            sourceResolution->runResolutionPass (doc.sourceRefs);

            // Partial open (§1.5.4): the session is fully displayed; if any source
            // is Missing, surface the session-scoped "Unresolved Sources" batch
            // step so the DJ can relocate/re-derive in one focused pass.
            if (! sourceResolution->areAllSourcesResolved())
            {
                juce::Component::SafePointer<juce::Component> safeContent (
                    mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr);
                juce::MessageManager::callAsync ([this, safeContent]
                {
                    if (sourceResolution != nullptr && ! sourceResolution->areAllSourcesResolved())
                        Daw::Session::Ui::showUnresolvedSourcesStep (
                            safeContent.getComponent(), *sourceResolution);
                });
            }
        }
        //===================================================================

        // Restore view state after the UI rebuild + layout completes.
        const auto vs = doc.viewState;
        juce::Component::SafePointer<Daw::DawPanel> safePanel (&dawPanel);
        juce::MessageManager::callAsync ([safePanel, vs]
        {
            if (safePanel != nullptr)
                safePanel->restoreViewState (vs.zoomSamplesPerPixel, vs.scrollStartSample);
        });
    };

    //--- onNewSession: the controller already installed a fresh empty daw model,
    // cleared undo, and set the title to "Untitled". Reset the view to the
    // canonical fit/home default after the UI rebuilds.
    host.onNewSession = [this, &dawPanel]()
    {
        // PRD-0097: a fresh empty model has no sources; reset the resolution
        // state so gating reports all-resolved and no stale flags remain.
        if (sourceResolution != nullptr)
            sourceResolution->clearAndReresolve();

        juce::Component::SafePointer<Daw::DawPanel> safePanel (&dawPanel);
        juce::MessageManager::callAsync ([safePanel]
        {
            if (safePanel != nullptr)
                safePanel->restoreViewState (std::nullopt, std::nullopt);
        });
    };

    //--- chooseFile: a real native FileChooser filtered to *.soniksession,
    // seeded per §1.5.4 (the controller supplies the directory + suggested name).
    host.chooseFile = [this] (Daw::Session::ChooserPurpose purpose,
                              juce::File suggestedDirectory,
                              juce::String suggestedName,
                              std::function<void (juce::File)> onChosen)
    {
        const bool saving = (purpose == Daw::Session::ChooserPurpose::SaveAs);
        const juce::String title = saving ? "Save Session As" : "Open Session";

        juce::File startLocation = suggestedDirectory;
        if (saving && suggestedName.isNotEmpty())
            startLocation = suggestedDirectory.getChildFile (suggestedName);

        sessionFileChooser = std::make_unique<juce::FileChooser> (
            title, startLocation, "*" + Daw::Session::kSessionFileExtension);

        const int flags = saving
            ? (juce::FileBrowserComponent::saveMode
               | juce::FileBrowserComponent::warnAboutOverwriting)
            : (juce::FileBrowserComponent::openMode
               | juce::FileBrowserComponent::canSelectFiles);

        sessionFileChooser->launchAsync (flags,
            [onChosen] (const juce::FileChooser& fc)
            {
                if (onChosen)
                    onChosen (fc.getResult()); // invalid File on cancel
            });
    };

    //--- promptUnsavedChanges: the DESIGN.md monochrome Save/Don't Save/Cancel
    // modal (2px ink borders, Space Mono, zero radius, dithered shadow).
    host.promptUnsavedChanges = [this] (std::function<void (Daw::Session::UnsavedChoice)> onChoice)
    {
        juce::String title = "Untitled";
        if (sessionController != nullptr)
            title = sessionController->displayTitle();

        Daw::Session::Ui::showUnsavedChangesPrompt (
            mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr,
            title, std::move (onChoice));
    };

    //--- showError: the DESIGN.md monochrome error notice. The live model is
    // left untouched by the controller on a failed open (it deserialises into a
    // staging tree and swaps only on full success, §1.5.7).
    host.showError = [this] (const juce::String& title, const juce::String& message)
    {
        Daw::Session::Ui::showSessionError (
            mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr,
            title, message);
    };

    //--- onTitleChanged: update the in-DAW session indicator AND the OS window
    // title (both carry the trailing dirty dot).
    host.onTitleChanged = [this] (const juce::String& displayTitle, bool dirty)
    {
        updateSessionTitleUi (displayTitle, dirty);
    };

    sessionController = std::make_unique<Daw::Session::SessionController> (
        DawState::getOrCreateDawBranch (deckStateManager->getStateTree()),
        *sessionSerializer,
        dawPanel.getUndoManager(),
        *appProperties->getUserSettings(),
        std::move (host));

    // File menu + shortcuts (§1.5.8). The command manager is the single binding
    // table the menu and keystrokes share.
    sessionMenu = std::make_unique<Daw::Session::Ui::SessionMenu> (*sessionController);

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (sessionMenu.get());
   #endif

    // Route the four shortcuts: register the command manager as a key listener
    // on the main window so Cmd/Ctrl+N/O/S and Shift+Cmd/Ctrl+S fire. Deck /
    // transport KeyPress handlers in MainContentComponent consume their keys
    // first, so the session commands never shadow them (§1.5.8).
    mainWindow->addKeyListener (sessionMenu->getCommandManager().getKeyMappings());

    // PRD-0097: play gating (§1.5.7). Wrap the DAW transport Play callback so an
    // attempt to play while any referenced source is Missing is BLOCKED, the
    // affected clips are already highlighted by the Glitch treatment, and the
    // batch resolution step is offered. Once every source is Resolved, play
    // proceeds with no further prompts. (Export gating for PRD-0099-0101 consults
    // the SAME areAllSourcesResolved() query — see the seam comment below.)
    {
        auto innerPlay = dawPanel.onTransportPlay; // the panel's real play action
        juce::Component::SafePointer<juce::Component> safeContent (
            mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr);

        dawPanel.onTransportPlay = [this, innerPlay, safeContent]()
        {
            if (sourceResolution != nullptr && ! sourceResolution->areAllSourcesResolved())
            {
                Daw::Session::Ui::showUnresolvedSourcesStep (
                    safeContent.getComponent(), *sourceResolution);
                return; // blocked: do not start playback with a Missing source
            }

            if (innerPlay)
                innerPlay();
        };
    }

    //========================= PRD-0101 EXPORT SEAM =========================
    // The "Export..." command (Cmd/Ctrl+E) opens the monochrome ExportDialog over
    // the current arrangement. The dialog runs the PRD-0099 render + PRD-0100
    // encode on its own background thread (the live audio device is untouched),
    // gates on the SAME areAllSourcesResolved() query as play (never exporting a
    // Missing source), persists last-used options app-wide via the shared
    // PropertiesFile, and restores the transport on cancel. All render/encode
    // logic is reused — this wiring only supplies the dialog its context.
    {
        sessionMenu->onExport = [this, &dawPanel]()
        {
            if (mainWindow == nullptr || trackDatabase == nullptr)
                return;

            Daw::Export::Ui::ExportContext ctx;

            ctx.projectSampleRate = (audioEngine != nullptr && audioEngine->getSampleRate() > 0.0)
                                        ? audioEngine->getSampleRate()
                                        : DawState::kProjectSampleRate;

            // buildJob: compile the live daw branch into a snapshot at the chosen
            // export rate + a per-clip ClipSourceResolver-backed ReaderProvider.
            ctx.buildJob = [this] (const Daw::Export::ExportOptions& options)
            {
                Daw::Export::ExportJobBuilder builder (
                    DawState::getOrCreateDawBranch (deckStateManager->getStateTree()),
                    *trackDatabase,
                    importPublisher.get());
                return builder.buildJob (options);
            };

            // Selected-region availability + bounds come from the DAW transport's
            // loop region (the only persisted selection source). A region exists
            // when the loop is armed and non-empty.
            auto& transport = dawPanel.getDawTransport();
            ctx.hasSelectedRegion = [&transport]()
            {
                // A selectable region exists when the loop is armed and non-empty.
                return transport.isLoopEnabled()
                    && transport.getLoopEnd() > transport.getLoopStart();
            };
            ctx.selectedRegion = [&transport] (double exportSampleRate) -> Daw::Export::ExportRange
            {
                // Loop bounds are kept at the project rate; scale to the export rate
                // so the range matches the compiled snapshot positions.
                const double scale = exportSampleRate / DawState::kProjectSampleRate;
                Daw::Export::ExportRange r;
                r.startSample = (juce::int64) std::llround ((double) transport.getLoopStart() * scale);
                r.endSample   = (juce::int64) std::llround ((double) transport.getLoopEnd()   * scale);
                return r;
            };

            // PRD-0097 export gate (the SAME query play consults).
            juce::Component::SafePointer<juce::Component> safeContent (
                mainWindow->getContentComponent());
            ctx.areAllSourcesResolved = [this]()
            {
                return sourceResolution == nullptr
                    || sourceResolution->areAllSourcesResolved();
            };
            ctx.describeMissingSources = [this]() -> juce::String
            {
                if (sourceResolution == nullptr)
                    return {};
                juce::StringArray names;
                for (const auto& s : sourceResolution->missingSources())
                    names.add (s.displayName.isNotEmpty() ? s.displayName : s.sourceFileId);
                return names.joinIntoString ("\n");
            };
            ctx.showUnresolvedSourcesStep = [this, safeContent]()
            {
                if (sourceResolution != nullptr)
                    Daw::Session::Ui::showUnresolvedSourcesStep (
                        safeContent.getComponent(), *sourceResolution);
            };

            // Last-used persistence store (PRD-0096 PropertiesFile; output path
            // excluded by the dialog).
            ctx.properties = (appProperties != nullptr)
                                 ? appProperties->getUserSettings()
                                 : nullptr;

            // Transport capture/restore for cancel (§1.5.2): snapshot the playhead
            // before export; restore it after (the offline driver may move it).
            ctx.captureTransport = [&transport]() -> juce::int64
            {
                return transport.getPlayheadSample();
            };
            ctx.restoreTransport = [&transport] (juce::int64 token)
            {
                transport.seek (token);
            };

            Daw::Export::Ui::showExportDialog (
                mainWindow->getContentComponent(), std::move (ctx));
        };
    }
    //=======================================================================

    //==========================================================================
    // PRD-0098: external audio-file import pipeline.
    //
    // Construct the ref-counted registry, the atomic publisher, the clip placer
    // (over the live daw branch + the DAW UndoManager so an import is one undo
    // transaction), and the importer with real callbacks. Inject the publisher
    // into the playback controller's resolver so imported clips are audible, and
    // wire the DawPanel drop + the File-menu command to the importer.
    //==========================================================================
    importRegistry  = std::make_unique<Daw::Import::ImportSourceRegistry>();
    importPublisher = std::make_unique<Daw::Import::ImportSourcePublisher>();
    importPlacer    = std::make_unique<Daw::Import::ImportClipPlacer> (
        DawState::getOrCreateDawBranch (deckStateManager->getStateTree()),
        dawPanel.getUndoManager(),
        *importRegistry);

    // Inject the publisher into the engine's clip-source resolver: an
    // "import:<hash>" id now resolves to the published atomic buffer (silence
    // until published; never disk I/O on the audio thread).
    if (auto* content = mainWindow->getContent())
        if (auto* pc = content->getPlaybackController())
            pc->setImportPublisher (importPublisher.get());

    {
        Daw::Import::AudioFileImporter::Callbacks cb;

        cb.getSessionSampleRate = [this]() -> double
        {
            return audioEngine != nullptr ? audioEngine->getSampleRate()
                                          : DawState::kProjectSampleRate;
        };

        // The "decoding..." placeholder begin/end are progressive affordances;
        // the lane already shows a placeholder waveform for the clip the instant
        // it is placed, so the in-flight token is a no-op hook here (kept for a
        // future explicit decoding-placeholder overlay).
        cb.onDecodeBegan = [] (juce::ValueTree, std::int64_t, int) {};
        cb.onDecodeEnded = [] (int) {};

        // Transient, monochrome error notice (DESIGN.md): no clip is created.
        cb.onError = [this] (const juce::String& message)
        {
            Daw::Session::Ui::showSessionError (
                mainWindow != nullptr ? mainWindow->getContentComponent() : nullptr,
                "Import Failed", message);
        };

        // After placement (and on each waveform completion) force a recompile so
        // the engine picks up the freshly published source + new clip nodes.
        cb.onClipsPlaced = [this]()
        {
            if (mainWindow == nullptr)
                return;
            if (auto* content = mainWindow->getContent())
                if (auto* trigger = content->getDawPanel().getRecompileTrigger())
                    trigger->compileNow();
        };

        audioFileImporter = std::make_unique<Daw::Import::AudioFileImporter> (
            *importRegistry, *importPublisher, *importPlacer, *trackDatabase, std::move (cb));
    }

    // DawPanel drag-drop: validate against the importer's format whitelist, and
    // run the import on drop (target lane + snapped sample come from the panel).
    dawPanel.isImportableFiles = [] (const juce::StringArray& files)
    {
        for (const auto& path : files)
            if (Daw::Import::AudioFileImporter::isSupportedExtension (
                    juce::File (path).getFileExtension()))
                return true;
        return false;
    };

    dawPanel.onFilesDropped = [this] (const juce::Array<juce::File>& files,
                                      juce::ValueTree lane,
                                      std::int64_t snappedSample)
    {
        if (audioFileImporter == nullptr || ! lane.isValid())
            return;

        // Keep only the importer-supported files, preserving OS order (§1.5.7).
        juce::Array<juce::File> audio;
        for (const auto& f : files)
            if (Daw::Import::AudioFileImporter::isSupportedExtension (f.getFileExtension()))
                audio.add (f);

        if (audio.isEmpty())
            return;

        Daw::Import::AudioFileImporter::Target target;
        target.lane               = lane;
        target.dropTimelineSample = snappedSample;
        // The panel already snapped the drop sample; do not re-snap.
        target.snap               = nullptr;

        if (audio.size() == 1)
            audioFileImporter->importFile (audio.getFirst(), target);
        else
            audioFileImporter->importFiles (audio, target);
    };

    // Shared chooser+import helper for the menu/context-menu entry points: opens
    // the native chooser filtered to the supported wildcard and imports the
    // chosen file onto `lane` at the already-snapped `dropSample` (§1.5.7).
    auto chooseAndImport = [this] (juce::ValueTree lane, std::int64_t dropSample)
    {
        if (audioFileImporter == nullptr || ! lane.isValid())
            return;

        importFileChooser = std::make_unique<juce::FileChooser> (
            "Import Audio File",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            Daw::Import::AudioFileImporter::supportedFormatsWildcard());

        importFileChooser->launchAsync (
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
            [this, lane, dropSample] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (! file.existsAsFile() || audioFileImporter == nullptr)
                    return;

                Daw::Import::AudioFileImporter::Target target;
                target.lane               = lane;
                target.dropTimelineSample = dropSample;
                target.snap               = nullptr; // already snapped at call site
                audioFileImporter->importFile (file, target);
            });
    };

    // File-menu "Import Audio File..." (§1.5.7): place at the playhead on the
    // first/focused lane.
    sessionMenu->onImportAudioFile = [this, chooseAndImport]()
    {
        if (audioFileImporter == nullptr || mainWindow == nullptr)
            return;

        auto* content = mainWindow->getContent();
        if (content == nullptr)
            return;

        auto& panel = content->getDawPanel();
        auto  lane  = panel.firstLaneTree();
        if (! lane.isValid())
        {
            Daw::Session::Ui::showSessionError (
                mainWindow->getContentComponent(),
                "Import Failed",
                "No lane is available to receive the imported file.");
            return;
        }

        // Place at the playhead, snapped per the active grid toggle.
        const std::int64_t playhead   = panel.getDawTransport().getPlayheadSample();
        const std::int64_t dropSample = panel.snapImportSample (playhead);
        chooseAndImport (lane, dropSample);
    };

    // Lane context-menu "Import Audio File..." (§1.5.7): place at the snapped
    // right-click position on the clicked lane.
    dawPanel.onImportRequestedAtPoint = [chooseAndImport] (juce::ValueTree lane,
                                                           std::int64_t snappedSample)
    {
        chooseAndImport (lane, snappedSample);
    };

    // Initial title.
    updateSessionTitleUi (sessionController->displayTitle(), sessionController->isDirty());
}

void SonikApplication::updateSessionTitleUi (const juce::String& displayTitle, bool dirty)
{
    const juce::String dot = juce::String::fromUTF8 ("\xe2\x80\xa2"); // bullet
    const juce::String marked = dirty ? displayTitle + " " + dot : displayTitle;

    // In-DAW indicator.
    if (mainWindow != nullptr)
        if (auto* content = mainWindow->getContent())
            content->getDawPanel().setSessionTitle (marked);

    // OS window title mirrors the same string. The em-dash separator must be
    // built as explicit UTF-8 (a raw literal mojibakes through juce::String).
    if (mainWindow != nullptr)
        mainWindow->setName (juce::String::fromUTF8 ("Sonik \xe2\x80\x94 ") + marked);

    // Keep the File-menu enablement (Save / Clear Recent) in step.
    if (sessionMenu != nullptr)
        sessionMenu->refresh();
}

void SonikApplication::shutdown()
{
    if (libraryAnalysisQueue != nullptr)
        libraryAnalysisQueue->cancelAllJobs();

    if (deckStateManager != nullptr)
        deckStateManager->saveSession();

    // PRD-0096: tear down the session lifecycle BEFORE the window/state. On
    // macOS detach the main menu first; the menu references the controller, the
    // controller references the panel's undo manager + the app properties +
    // the live daw branch, so destroy in dependency order.
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    if (mainWindow != nullptr && sessionMenu != nullptr)
        mainWindow->removeKeyListener (sessionMenu->getCommandManager().getKeyMappings());
    sessionMenu.reset();
    sessionController.reset();
    // PRD-0097: resolution references the daw branch + DB + stem manager + the
    // panel's recompile trigger; drop it before the window/state tear-down.
    sourceResolution.reset();
    sessionFileChooser.reset();
    sessionSerializer.reset();
    if (appProperties != nullptr)
        appProperties->saveIfNeeded();
    appProperties.reset();

    // PRD-0098: tear down the import pipeline before the window/state. Stop the
    // importer (joins its decode thread pool) first, then detach the publisher
    // from the still-alive playback controller's resolver so a dangling pointer
    // is never read, then drop the placer/publisher/registry in dependency order.
    importFileChooser.reset();
    audioFileImporter.reset();
    if (mainWindow != nullptr)
        if (auto* content = mainWindow->getContent())
            if (auto* pc = content->getPlaybackController())
                pc->setImportPublisher (nullptr);
    importPlacer.reset();
    importPublisher.reset();
    importRegistry.reset();

    midiSettingsWindow.reset();  // PRD-0048
    mainWindow.reset();

    // PRD-0064/0066: grid service references the clock publisher + audio engine;
    // destroy it (after the UI that uses it) before those dependencies go away.
    masterGridService.reset();

    // Stop the watch-folder scanner before the database is torn down.
    if (watchFolderScanner != nullptr)
        watchFolderScanner->cancelScan();
    watchFolderScanner.reset();

    // Stop file loader before engine
    audioFileLoader.reset();

    // Stop waveform manager before engine
    waveformManager.reset();

    // Join the stem waveform analyzer's thread pool before the database it writes
    // peaks into is torn down.
    stemWaveformAnalyzer.reset();

    // Stop beat grid manager before engine
    beatGridManager.reset();

    libraryAnalysisQueue.reset();
    libraryAnalysisService.reset();

    // Stop stem separation manager before model manager
    stemSeparationManager.reset();

    // Stop model manager before engine
    modelManager.reset();

    // Stop audio engine BEFORE destroying DeckStateManager
    audioEngine.reset();

    // Master clock manager must be destroyed before deckStateManager
    // (it holds a ValueTree reference and listens to rootState).
    masterClockManager.reset();
    masterClockPublisher.reset();

    deckStateManager.reset();

    // Tear down MIDI before the database (PRD-0040). Destruction stops the
    // hot-plug timer and closes every open MIDI input/output on the Message
    // thread.
    if (midiDeviceManager != nullptr && midiDiagnosticLogger != nullptr)
        midiDeviceManager->removeDeviceListChangeListener (midiDiagnosticLogger.get());
    midiDiagnosticLogger.reset();
    midiInboundRouter.reset();   // PRD-0044 (unsubscribes from device manager, store, bridge)
    compositeMidiHandler.reset();
    libraryMidiHandler.reset();
    mixerMidiHandler.reset();
    deckMidiHandler.reset();
    midiFeedbackEngine.reset(); // PRD-0047 (depends on mappingStore + device manager + state tree)
    softTakeoverManager.reset(); // PRD-0045 (depends on mappingStore + deckStateManager)
    mappingStore.reset();        // PRD-0043
    midiMessageBridge.reset();   // PRD-0041
    midiDeviceManager.reset();
    midiHost.reset();

    trackDatabase.reset();

    // The tooltip window is a desktop component styled by the LookAndFeel,
    // so it must go before the LookAndFeel does.
    tooltipWindow.reset();

    // Every component is gone by now; restore the stock default before the
    // LookAndFeel instance itself is destroyed.
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    lookAndFeel.reset();
}

void SonikApplication::systemRequestedQuit()
{
    if (quitSaveActive || quitSessionActive)
        return;

    // PRD-0096: the session dirty-guard fires FIRST. If the live `daw` model has
    // unsaved changes, confirmQuit presents the monochrome Save / Don't Save /
    // Cancel prompt; Cancel vetoes the quit (mayQuit == false). Only when the
    // session permits the quit do we fall through to the existing preparation-
    // list guard and, finally, quit().
    if (sessionController != nullptr)
    {
        quitSessionActive = true;
        sessionController->confirmQuit (
            [this] (bool mayQuit)
            {
                quitSessionActive = false;
                if (mayQuit)
                    proceedWithPreparationListQuit();
                // else: Cancel — application stays open.
            });
        return;
    }

    proceedWithPreparationListQuit();
}

void SonikApplication::proceedWithPreparationListQuit()
{
    if (quitSaveActive)
        return;

    if (mainWindow != nullptr)
    {
        if (auto* content = mainWindow->getContent())
        {
            quitSaveActive = true;
            content->savePreparationListBeforeQuit (
                [this] (bool shouldQuit)
                {
                    quitSaveActive = false;
                    if (shouldQuit)
                        quit();
                });
            return;
        }
    }

    quit();
}

//==============================================================================
// PRD-0040 diagnostic logger implementation
namespace
{
    static juce::String midiDeviceDescription (sonik::midi::MidiDeviceManager& mgr, std::uint64_t id)
    {
        for (const auto& d : mgr.getDevices())
            if (d.deviceId == id)
                return juce::String (d.isInput ? "IN  " : "OUT ")
                       + "'" + d.productName + "' (ordinal=" + juce::String (d.ordinal) + ")";
        return juce::String ("<unknown>");
    }
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceAdded (std::uint64_t deviceId)
{
    if (manager != nullptr)
        DBG ("[MIDI] +ADDED   id=" << juce::String::toHexString ((juce::int64) deviceId)
             << "  " << midiDeviceDescription (*manager, deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceRemoved (std::uint64_t deviceId)
{
    if (manager != nullptr)
        DBG ("[MIDI] -REMOVED id=" << juce::String::toHexString ((juce::int64) deviceId)
             << "  " << midiDeviceDescription (*manager, deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceOpened (std::uint64_t deviceId)
{
    DBG ("[MIDI]  OPENED  id=" << juce::String::toHexString ((juce::int64) deviceId));
}

void SonikApplication::MidiDiagnosticLogger::midiDeviceClosed (std::uint64_t deviceId)
{
    DBG ("[MIDI]  CLOSED  id=" << juce::String::toHexString ((juce::int64) deviceId));
}

//==============================================================================
// PRD-0048: MIDI Settings window lifecycle.
void SonikApplication::openMidiSettingsWindow()
{
    if (midiSettingsWindow != nullptr)
    {
        midiSettingsWindow->toFront (true);
        return;
    }

    if (mappingStore == nullptr || midiDeviceManager == nullptr
        || midiInboundRouter == nullptr || softTakeoverManager == nullptr)
        return;

    midiSettingsWindow = std::make_unique<sonik::midi::MidiSettingsWindow> (
        *mappingStore, *midiDeviceManager, *midiInboundRouter, *softTakeoverManager);

    midiSettingsWindow->onClose = [this]()
    {
        midiSettingsWindow.reset();
    };
}

