#pragma once

#include <cmath>
#include <cstdint>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include "../../AudioEngine/AudioFileLoader.h"
#include "../../Waveform/WaveformManager.h"
#include "../../Waveform/WaveformData.h"
#include "../../Waveform/WaveformCache.h"
#include "../Database/TrackDatabase.h"
#include "../../BeatGrid/BeatGridManager.h"
#include "../../Sync/MasterClockManager.h"
#include "GlobalToolbar.h"
#include "DeckLayoutManager.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/Ui/Organisms/DawPanel.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Projection/LiveProjectionTimer.h"
#include "Features/Daw/Projection/DeckManagerProjectionSource.h"
#include "Features/Daw/Recording/RecordingSessionController.h"
#include "Features/Daw/Playback/DawPlaybackController.h"
#include "Features/Daw/Automation/AutomationModel.h"
#include "Features/Daw/Automation/AutomationCaptureTaps.h"
#include "Features/Daw/Automation/ChannelContinuousAutomationCapture.h"
#include "Features/Daw/Automation/ChannelBooleanAutomationCapture.h"
#include "Features/Daw/Automation/MasterTempoAutomationCapture.h"
#include "Features/Daw/Automation/AutomationApplier.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Library/UI/LibraryComponent.h"
#include "Features/Library/WatchFolderScanner.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerMeterSnapshot.h"
#include "Features/Mixer/Ui/Organisms/MixerComponent.h"
#include "Features/Deck/DeckIdentifiers.h"
#include <functional>
#include <memory>

class StemSeparationManager;

class MainContentComponent final : public juce::Component,
                                    public juce::DragAndDropContainer,
                                    private juce::ValueTree::Listener,
                                    private juce::Timer
{
public:
    MainContentComponent (DeckStateManager& deckState,
                          AudioEngine& engine,
                          AudioFileLoader& loader,
                          WaveformManager& waveformMgr,
                          BeatGridManager& beatGridMgr,
                          StemSeparationManager& stemMgr,
                          MasterClockManager& clockMgr,
                          LibraryAnalysisQueue& analysisQueue,
                          TrackDatabase& trackDb,
                          MixerStateSchema& mixerSchema,
                          MixerMeterSnapshot& mixerMeters,
                          Daw::MasterGridService& gridService)
        : deckStateManager (deckState),
          audioEngine (engine),
          audioFileLoader (loader),
          trackDatabase (trackDb),
          masterClockManager (clockMgr),
          gridService_ (gridService),
          rootState (deckState.getStateTree()),
          toolbar (deckState, mixerSchema, mixerMeters),
          layoutManager (deckState, engine, loader, waveformMgr, beatGridMgr, stemMgr, clockMgr),
          mixerComponent (mixerSchema, mixerMeters,
                          rootState.getChildWithName (IDs::Decks)),
          dawPanel (gridService,
                    DawState::getOrCreateDawBranch (rootState),
                    [rootTree = rootState](int deckIndex) -> juce::ValueTree
                    {
                        auto decks = rootTree.getChildWithName (IDs::Decks);
                        int count = 0;
                        for (int i = 0; i < decks.getNumChildren(); ++i)
                        {
                            auto deck = decks.getChild (i);
                            if (deck.hasType (IDs::Deck))
                            {
                                if (count == deckIndex)
                                    return deck;
                                ++count;
                            }
                        }
                        return {};
                    },
                    // PRD-0068: resolve a clip's sourceFileId to its WaveformData.
                    // Backed by a SHARED in-memory WaveformCache so each source's
                    // multi-megabyte mipmap is read from the PRD-0006 SQLite cache
                    // and deserialized exactly ONCE, then handed to every clip (the
                    // "one sample, many clip references" model). Without this, every
                    // ClipBlock::paint hit SQLite + deserialized per frame, which is
                    // the root cause of the recording-mode lag. This std::function
                    // is copied down to every ClipBlock; the shared_ptr capture keeps
                    // them all pointing at the one cache. A miss returns nullptr so
                    // the clip draws a placeholder; it is never cached (so a clip
                    // whose analysis lands later still picks it up) and never
                    // triggers a fresh analysis.
                    [cache = std::make_shared<Daw::WaveformCache> (
                         [&trackDb](const juce::String& id) -> WaveformData::Ptr
                         {
                             juce::MemoryBlock block;
                             if (! trackDb.loadWaveformData (id, block))
                                 return nullptr;
                             return WaveformData::deserialize (block, id);
                         })]
                    (const juce::String& sourceFileId) -> WaveformData::Ptr
                    {
                        return cache->get (sourceFileId);
                    }),
          libraryComponent (std::make_unique<LibraryComponent> (rootState, trackDb, analysisQueue))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        toolbar.onAddDeckClicked = [this]()
        {
            handleAddDeck();
        };

        dawPanel.onPreferredHeightChanged = [this]() { resized(); };

        addAndMakeVisible (toolbar);
        addAndMakeVisible (dawPanel);
        addAndMakeVisible (layoutManager);
        addAndMakeVisible (mixerComponent);
        addAndMakeVisible (*libraryComponent);

        rootState.addListener (this);

        // PRD-0069: start the live-deck projection bridge. It reads each deck's
        // published atomics on the message thread and grows live clips on the
        // matching DAW lane(s). The DawPanel's now-line (PRD-0070) follows the
        // bridge's monotonic now-line sample.
        projectionSource = std::make_unique<Daw::DeckManagerProjectionSource> (deckStateManager);
        liveProjection   = std::make_unique<Daw::LiveProjectionTimer> (
                               *projectionSource,
                               DawState::getOrCreateDawBranch (rootState),
                               gridService);
        // The now-line (DAW playback cursor) must only appear during arrangement
        // playback (EPIC-0010). During normal DJing and recording the timeline
        // cursor role belongs to the record playhead exclusively. Deliberately
        // not calling dawPanel.setNowLineProvider here; the panel will keep the
        // now-line hidden (lineX = -1) until playback is wired by EPIC-0010.
        // The recording clock still consumes liveProjection->getNowLineSample()
        // internally via recordingClock->setMasterTimelineProvider.
        // Gate clip writing: DAW lanes only grow when the Record button is
        // active (Armed or Recording). nowLineSample_ always advances.
        liveProjection->setCapturingProvider ([this]() {
            return recordingController != nullptr
                   && recordingController->state() != Daw::RecordingState::Stopped;
        });
        liveProjection->start();

        // PRD-0071 / PRD-0078: the explicit record session. Its clock is anchored
        // to the projection now-line (PRD-0069) so the record playhead tracks
        // musical time while a deck drives the grid, and free-runs over silence
        // otherwise. The DawPanel's global Record button arms/stops the session;
        // the record playhead overlay reads the live timeline position.
        recordingClock = std::make_unique<Daw::LiveRecordingClock> (clockMgr, gridService);
        recordingClock->setMasterTimelineProvider (
            [this]() -> std::int64_t { return liveProjection->getNowLineSample(); });
        recordingController = std::make_unique<Daw::RecordingSessionController> (
            DawState::getOrCreateDawBranch (rootState), gridService, clockMgr,
            recordingClock.get());

        dawPanel.onRecordToggle = [this]()
        {
            if (recordingController == nullptr)
                return;
            if (recordingController->state() == Daw::RecordingState::Stopped)
            {
                // Reset the DAW timeline to beat 1 so every recording session
                // starts at the same origin. A deck playing 30s before Record
                // is pressed will have a clip at beat 1, sourced from the
                // 30-second position of the audio file.
                liveProjection->resetTimeline();
                recordingController->arm();
            }
            else
            {
                recordingController->stop();
            }
        };

        dawPanel.setRecordStateProvider (
            [this]() -> Daw::DawPanel::RecordUiState
            {
                if (recordingController == nullptr)
                    return Daw::DawPanel::RecordUiState::Idle;
                switch (recordingController->state())
                {
                    case Daw::RecordingState::Armed:
                        return Daw::DawPanel::RecordUiState::Armed;
                    case Daw::RecordingState::Recording:
                        return Daw::DawPanel::RecordUiState::Recording;
                    case Daw::RecordingState::Stopped:
                    default:
                        return Daw::DawPanel::RecordUiState::Idle;
                }
            });

        // The record playhead is rendered in the same DAW coordinate space as
        // the clip blocks. Clips are anchored to nowLineSample_ (the absolute
        // DAW timeline position), so the playhead must track nowLineSample_
        // too. recordingController->currentTimelinePosition() counts elapsed
        // time from 0 and diverges from nowLineSample_ whenever the deck was
        // already playing when recording began.
        dawPanel.setRecordPlayheadProvider (
            [this]() -> std::int64_t
            {
                return liveProjection != nullptr
                           ? liveProjection->getNowLineSample()
                           : 0;
            });

        // EPIC-0010: wire the arrangement playback engine. The DawPlaybackController
        // owns the streamer pool + source resolution; it provides the compiler's
        // per-clip resolver so every clip gets a primed streamer. The AudioEngine's
        // TimelineRenderer reads the same publisher + pool the panel compiles into.
        playbackController = std::make_unique<Daw::DawPlaybackController> (
            trackDatabase, dawPanel.getDawTransport());
        playbackController->setRuntimeSampleRate (audioEngine.getSampleRate());

        if (auto* trigger = dawPanel.getRecompileTrigger())
        {
            trigger->setCompiler (playbackController->makeCompiler());
            trigger->compileNow();
        }

        audioEngine.setDawPlayback (&dawPanel.getArrangementPublisher(),
                                    &playbackController->getPool(),
                                    &dawPanel.getDawTransport());

        // EPIC-0010 playback fix: the recorded arrangement (PRD-0069) is anchored
        // at the master-grid phase origin, which is non-zero whenever a master
        // deck drives the grid, while the transport plays from its origin. Seed
        // that origin with the first clip's start on every Play so playback lands
        // on the recorded content instead of running silently from sample 0 up to
        // it. The arrangement is stored in project-rate samples; scale to the
        // runtime/device rate the transport playhead advances in.
        {
            auto innerPlay = dawPanel.onTransportPlay; // panel's real play action
            dawPanel.onTransportPlay = [this, innerPlay]()
            {
                if (playbackController != nullptr)
                {
                    const std::int64_t startProject = DawState::earliestClipStartSample (
                        DawState::getOrCreateDawBranch (rootState));
                    const std::int64_t originRuntime = static_cast<std::int64_t> (
                        std::llround (static_cast<double> (startProject)
                                      * playbackController->sampleRateScale()));
                    dawPanel.getDawTransport().setOriginSample (originRuntime);
                }
                if (innerPlay)
                    innerPlay();
            };
        }

        // EPIC-0010: the DAW now-line (playback cursor) tracks the transport
        // playhead. Returns -1 when stopped, so the panel hides the now-line
        // outside of arrangement playback (recording uses the record playhead).
        dawPanel.setNowLineProvider (
            [this]() -> std::int64_t
            {
                return dawPanel.getDawTransport().getPlayheadSample();
            });

        // EPIC-0011: automation capture + playback wiring (message thread only).
        //
        // The model is built over the SAME `daw` branch DawPanel built its own
        // AutomationModel over, so both share the one `automation` ValueTree node:
        // capture writes are observed by the DAW UI automatically. The append sink
        // is the ONLY path capture takes into the model (no parallel back door).
        automationModel = std::make_unique<Daw::AutomationModel> (
                              DawState::getOrCreateDawBranch (rootState));
        automationSink  = std::make_unique<Daw::ModelAutomationAppendSink> (*automationModel);

        // The capture gate: true while Armed OR Recording (i.e. not Stopped).
        auto isRecordingArmed = [this]() -> bool
        {
            return recordingController != nullptr
                   && recordingController->state() != Daw::RecordingState::Stopped;
        };

        // The record playhead for ALL automation capture is the ABSOLUTE DAW
        // timeline sample clips are anchored to (the projection now-line), NOT
        // recordingController->currentTimelinePosition() (which counts from 0 and
        // would desync breakpoints from the clips/playhead they must align with).
        auto recordPlayhead = [this]() -> std::int64_t
        {
            return liveProjection != nullptr ? liveProjection->getNowLineSample() : 0;
        };

        // The deck resolver: channel index -> deck ValueTree. SAME resolution
        // logic DawPanel uses (only Deck-typed children counted). Shared by the
        // boolean capture component and the applier's boolean sink.
        std::function<juce::ValueTree (int)> deckResolver =
            [rootTree = rootState](int deckIndex) -> juce::ValueTree
            {
                auto decks = rootTree.getChildWithName (IDs::Decks);
                int count = 0;
                for (int i = 0; i < decks.getNumChildren(); ++i)
                {
                    auto deck = decks.getChild (i);
                    if (deck.hasType (IDs::Deck))
                    {
                        if (count == deckIndex)
                            return deck;
                        ++count;
                    }
                }
                return {};
            };

        // PRD-0090: the twenty continuous per-channel mixer lanes (filter / gain /
        // eq.high|mid|low for channels A..D), observed on the mixer ValueTrees.
        automationCaptureTaps = std::make_unique<Daw::AutomationCaptureTaps> (
                                    isRecordingArmed, recordPlayhead, *automationSink);
        Daw::ChannelContinuousAutomationCapture::registerTaps (*automationCaptureTaps, mixerSchema);

        // PRD-0091: the derived per-deck boolean lanes (keyLock / pitchStretch /
        // keyStepper), observed on the deck ValueTrees.
        channelBooleanCapture = std::make_unique<Daw::ChannelBooleanAutomationCapture> (
                                    deckResolver, isRecordingArmed, recordPlayhead, *automationSink);

        // PRD-0089: the single master/tempo lane, polled from the authoritative
        // published grid BPM each tick while recording.
        masterTempoCapture = std::make_unique<Daw::MasterTempoAutomationCapture> (
                                 [&gridService]() -> double { return gridService.snapshotGrid().bpm; },
                                 isRecordingArmed, recordPlayhead, *automationSink);

        // PRD-0092: the playback applier. tempoSink routes to the ONE tempo
        // authority (MasterClockManager override). booleanSink routes keyLock to
        // the deck's keyLockEnabled property; keyStepper / pitchStretch are a
        // documented no-op (see below).
        automationApplier = std::make_unique<Daw::AutomationApplier> (
            *automationModel,
            mixerSchema,
            dawPanel.getDawTransport(),
            [&clockMgr](double bpm) { clockMgr.setAutomationTempoOverride (bpm); },
            [deckResolver](int ch, const juce::String& paramId, bool state)
            {
                if (paramId == "keyLock")
                {
                    // keyLock has a faithful single-boolean deck target: the
                    // PRD-0011 keyLockEnabled property. Resolve by channel index
                    // (identity channel<->deck) and write through the same
                    // authoritative deck property the live UI / MIDI controls use.
                    auto deck = deckResolver (ch);
                    if (deck.isValid())
                        deck.setProperty (IDs::keyLockEnabled, state, nullptr);
                    return;
                }

                // keyStepper / pitchStretch are DERIVED boolean conditions with no
                // faithful single-boolean deck target: the engaged flag alone
                // cannot restore the semitone magnitude owned by PRD-0025, so
                // writing a bare bool would be lossy/destructive. Per PRD-0092's
                // note that the sink routes to production-appropriate targets only
                // where one exists, these are an intentional HARMLESS NO-OP here.
                juce::ignoreUnused (paramId, state);
            });

        // PRD-0092 re-entrancy guard: bind the applier's "applying" predicate to
        // every capture component so values the applier writes during playback are
        // recognised as automation-originated and are NOT re-captured (no
        // capture<->playback feedback loop). Both sides run on the message thread.
        automationCaptureTaps->setApplyingAutomationGuard (automationApplier->makeApplyingGuard());
        channelBooleanCapture->setApplyingAutomationGuard (automationApplier->makeApplyingGuard());
        masterTempoCapture->setApplyingAutomationGuard (automationApplier->makeApplyingGuard());

        // Metronome (testing aid): route the DawPanel toggle to the engine. The
        // click is summed into the live output only, so it is heard but is never
        // part of the DAW recording (a source-file reconstruction) or an export.
        dawPanel.onMetronomeToggle = [this] (bool on)
        {
            audioEngine.setMetronomeEnabled (on);
        };

        // Drive the record playhead clock on the message thread at UI cadence and
        // promote Armed -> Recording the instant any deck begins producing audio.
        startTimerHz (30);
    }

    ~MainContentComponent() override
    {
        stopTimer();
        if (liveProjection != nullptr)
            liveProjection->stop();
        rootState.removeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFFF9F9F9)); // surface
    }

    // PRD-0078: advance the record playhead clock and promote Armed -> Recording
    // as soon as a deck starts producing audio. Message thread only.
    void timerCallback() override
    {
        // EPIC-0010: keep the playback rate scale in sync with the live device
        // rate. The device may start (or change) after construction; when the
        // rate differs from what the compiler assumed, update it and recompile
        // so clip sample positions are reconciled to the runtime rate.
        if (playbackController != nullptr)
        {
            const double liveRate = audioEngine.getSampleRate();
            if (liveRate > 0.0 && std::abs (liveRate - lastRuntimeRate_) > 0.5)
            {
                lastRuntimeRate_ = liveRate;
                playbackController->setRuntimeSampleRate (liveRate);
                if (auto* trigger = dawPanel.getRecompileTrigger())
                {
                    trigger->setCompiler (playbackController->makeCompiler());
                    trigger->requestRecompile();
                }
            }

            // Metronome (testing aid): publish the DAW grid in the transport's
            // runtime sample domain so the audio-thread click stays locked to the
            // grid during arrangement playback. Phase origin and BPM come from the
            // single master-grid authority; the scale matches the compiler's.
            const auto   grid        = gridService_.snapshotGrid();
            const double runtimeRate = audioEngine.getSampleRate();
            const double bpm         = grid.bpm > 0.0 ? grid.bpm : 120.0;
            const double beatLenRt   = (runtimeRate > 0.0)
                                           ? (runtimeRate * 60.0 / bpm) : 0.0;
            const double scale       = playbackController->sampleRateScale();
            const auto   originRt     = static_cast<std::int64_t> (
                std::llround (static_cast<double> (grid.phaseOriginSample) * scale));
            audioEngine.setMetronomeGrid (beatLenRt, originRt, DawState::kBeatsPerBar);
        }

        if (recordingController == nullptr)
            return;

        recordingController->tick();

        if (recordingController->state() == Daw::RecordingState::Armed
            && anyDeckPlaying())
            recordingController->beginCapture();

        // EPIC-0011: drive automation capture + playback (message thread only).
        driveAutomation();
    }

    // EPIC-0011: capture/playback driving, factored out of timerCallback for
    // clarity. Message thread only — no audio-thread code. Detects record-start /
    // record-stop edges to seed/flush lanes, polls master tempo while recording,
    // and ticks the playback applier (which self-gates on transport playing).
    void driveAutomation()
    {
        const bool recordingActive = recordingController != nullptr
            && recordingController->state() != Daw::RecordingState::Stopped;
        const std::int64_t nowLine = liveProjection != nullptr
            ? liveProjection->getNowLineSample() : 0;

        // RECORD-START edge: seed each lane's initial value at the now-line so
        // every lane is defined from its first sample.
        if (recordingActive && ! prevRecordingActive_)
        {
            if (automationCaptureTaps != nullptr)
                automationCaptureTaps->captureInitialValues (nowLine);
            if (channelBooleanCapture != nullptr)
                channelBooleanCapture->captureInitialValues (nowLine);
            if (masterTempoCapture != nullptr)
                masterTempoCapture->seedAtRecordStart();
        }

        // WHILE recording: poll the master tempo each tick (continuous/boolean
        // taps are listener-driven and need no per-tick call).
        if (recordingActive && masterTempoCapture != nullptr)
            masterTempoCapture->captureTick();

        // RECORD-STOP edge: flush continuous lanes onto their resting value so a
        // decimated sweep terminates exactly where the control came to rest.
        if (! recordingActive && prevRecordingActive_)
        {
            if (automationCaptureTaps != nullptr)
                automationCaptureTaps->flush (nowLine);
        }

        prevRecordingActive_ = recordingActive;

        // PLAYBACK: the applier self-gates (no-op unless the transport is playing),
        // so calling it unconditionally every tick is safe.
        if (automationApplier != nullptr)
            automationApplier->tick();

        // PLAYBACK-STOP edge: when the transport transitions from playing to not
        // playing, clear any automation tempo override so the master clock reverts
        // to its derived BPM. Clearing when no override is active is a no-op.
        const bool transportPlaying = dawPanel.getDawTransport().isPlaying();
        if (! transportPlaying && prevTransportPlaying_)
            masterClockManager.clearAutomationTempoOverride();
        prevTransportPlaying_ = transportPlaying;
    }

    bool anyDeckPlaying() const
    {
        if (projectionSource == nullptr)
            return false;

        const int numDecks = projectionSource->getNumDecks();
        for (int slot = 0; slot < numDecks; ++slot)
        {
            auto* audio = projectionSource->getAudioState (slot);
            if (audio == nullptr)
                continue;
            if (audio->playbackStatus.load (std::memory_order_acquire)
                == static_cast<int> (PlaybackStatusCode::playing))
                return true;
        }
        return false;
    }

    void resized() override
    {
        auto b = getLocalBounds();
        toolbar.setBounds (b.removeFromTop (toolbarHeight));

        // PRD-0066: the DAW arrangement panel docks directly beneath the global
        // toolbar, above the deck rack. It owns its height (collapsed/expanded).
        dawPanel.setBounds (b.removeFromTop (dawPanel.getPreferredHeight()));

        // Deck area: fixed height derived from the number of decks — never
        // compressed, never stretched.  Horizontal margins only (no top/bottom).
        const int deckH = layoutManager.getPreferredHeight();
        auto deckArea = b.removeFromTop (deckH);
        deckArea.removeFromLeft  (8);
        deckArea.removeFromRight (8);
        layoutManager.setBounds (deckArea);

        // PRD-0060 (revised): the mixer no longer occupies a separate band
        // beneath the deck rack — it sits in the centre column carved out by
        // DeckLayoutManager so it visually anchors between deck A and deck B.
        // We translate the manager-local mixer rect to parent coordinates.
        auto mixerCol = layoutManager.getMixerColumnArea();
        if (! mixerCol.isEmpty())
        {
            mixerCol.translate (layoutManager.getX(), layoutManager.getY());
            mixerComponent.setBounds (mixerCol);
            mixerComponent.setVisible (true);
        }
        else
        {
            mixerComponent.setVisible (false);
        }

        // Library fills all remaining vertical space flush below the deck rack.
        libraryComponent->setBounds (b);
    }

    void registerScannerWithLibrary (WatchFolderScanner& scanner)
    {
        if (libraryComponent)
            libraryComponent->setScanner (scanner);
    }

    /** PRD-0048: route the toolbar MIDI button click to the application. */
    void setOnMidiClicked (std::function<void()> cb)
    {
        toolbar.onMidiClicked = std::move (cb);
    }

    /** Access the library component for MIDI control wiring. */
    LibraryComponent* getLibraryComponent() const noexcept { return libraryComponent.get(); }

    /** Access the layout manager for MIDI handler engine registration. */
    DeckLayoutManager& getLayoutManager() noexcept { return layoutManager; }

    /** PRD-0096: the DAW arrangement panel — the SessionController host wires
        view-state capture/restore, the undo baseline, and the in-DAW session
        indicator through it. */
    Daw::DawPanel& getDawPanel() noexcept { return dawPanel; }

    /** PRD-0098: the arrangement playback controller — the host injects the
        import-source publisher into its ClipSourceResolver so imported clips
        ("import:<hash>") resolve to the atomic in-memory reader. */
    Daw::DawPlaybackController* getPlaybackController() noexcept { return playbackController.get(); }

    void savePreparationListBeforeQuit (std::function<void(bool)> completion)
    {
        if (libraryComponent != nullptr)
            libraryComponent->savePreparationListBeforeQuit (std::move (completion));
        else if (completion)
            completion (true);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // PRD-0102: Cmd+Shift+Z = redo, Cmd+Z = undo for the DAW arrangement
        // edits (move / trim / extend / split / delete / gain). Multi-level: the
        // shared juce::UndoManager keeps the full transaction history, so each
        // press steps one more action back (or forward for redo). Handled here at
        // the top-level content component so it works regardless of which DAW
        // sub-component currently holds keyboard focus. Check the shift (redo)
        // variant first since it also has the command modifier down.
        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier
                                            | juce::ModifierKeys::shiftModifier, 0))
        {
            if (auto* disp = dawPanel.getEditDispatcher())
            {
                disp->redo();
                return true;
            }
        }
        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
        {
            if (auto* disp = dawPanel.getEditDispatcher())
            {
                disp->undo();
                return true;
            }
        }

        // Cmd+1..4 to switch active deck (macOS)
        auto mods = key.getModifiers();
        if (mods.isCommandDown())
        {
            auto ids = deckStateManager.getDeckIds();
            int keyCode = key.getKeyCode();

            // KeyPress encodes '1'..'4' as their ASCII codes
            if (keyCode >= '1' && keyCode <= '4')
            {
                int idx = keyCode - '1';
                if (idx < ids.size())
                {
                    deckStateManager.setActiveDeck (ids[idx]);
                    return true;
                }
            }
        }

        // Space for play/pause
        if (key == juce::KeyPress::spaceKey)
        {
            auto activeDeck = deckStateManager.getActiveDeckId();
            auto* state = deckStateManager.getAudioState (activeDeck);
            if (state != nullptr)
            {
                auto st = static_cast<PlaybackStatusCode> (
                    state->playbackStatus.load (std::memory_order_relaxed));
                if (st == PlaybackStatusCode::playing)
                {
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Pause);
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "paused");
                }
                else if (st == PlaybackStatusCode::stopped
                         || st == PlaybackStatusCode::paused)
                {
                    audioEngine.sendTransportCommand (activeDeck, TransportCommand::Play);
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "playing");
                }
            }
            return true;
        }

        // S for stop
        if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
        {
            auto activeDeck = deckStateManager.getActiveDeckId();
            auto* state = deckStateManager.getAudioState (activeDeck);
            if (state != nullptr)
            {
                auto st = static_cast<PlaybackStatusCode> (
                    state->playbackStatus.load (std::memory_order_relaxed));
                if (st == PlaybackStatusCode::playing || st == PlaybackStatusCode::paused)
                    // Keep ValueTree in sync so MasterClockManager is notified (PRD-0027)
                    deckStateManager.setPlaybackStatus (activeDeck, "stopped");
            }
            audioEngine.sendTransportCommand (activeDeck, TransportCommand::Stop);
            return true;
        }

        return false;
    }

    void visibilityChanged() override
    {
        if (!isVisible())
            return;

        juce::Component::SafePointer<MainContentComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr && safeThis->isShowing())
                safeThis->grabKeyboardFocus();
        });
    }

private:
    void handleAddDeck()
    {
        auto newId = deckStateManager.addDeck();
        if (newId.isNotEmpty())
        {
            // Register with audio engine
            auto* state = deckStateManager.getAudioState (newId);
            if (state != nullptr)
                audioEngine.registerDeck (newId, state);

            toolbar.updateButtons();
        }
    }

    // ValueTree::Listener — update toolbar when deck count or active deck changes
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override
    {
        if (tree == rootState
            && (property == IDs::deckCount || property == IDs::activeDeckId))
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                {
                    safeThis->toolbar.updateButtons();
                    safeThis->resized(); // re-layout when deck count changes
                }
            });
        }
    }

    DeckStateManager&   deckStateManager;
    AudioEngine&        audioEngine;
    AudioFileLoader&    audioFileLoader;
    TrackDatabase&      trackDatabase;
    MasterClockManager& masterClockManager; // EPIC-0011: tempo override revert
    Daw::MasterGridService& gridService_;    // metronome grid publication
    juce::ValueTree     rootState;

    GlobalToolbar      toolbar;
    DeckLayoutManager  layoutManager;
    MixerComponent     mixerComponent;
    Daw::DawPanel      dawPanel;            // PRD-0066
    juce::TooltipWindow tooltipWindow { this };

    // ---- Library panel ----------------------------------------------------

    std::unique_ptr<LibraryComponent> libraryComponent;

    // ---- PRD-0069 live-deck projection bridge ----------------------------
    std::unique_ptr<Daw::DeckManagerProjectionSource> projectionSource;
    std::unique_ptr<Daw::LiveProjectionTimer>         liveProjection;

    // ---- EPIC-0009 record session (PRD-0071 / PRD-0078) -------------------
    std::unique_ptr<Daw::LiveRecordingClock>          recordingClock;
    std::unique_ptr<Daw::RecordingSessionController>  recordingController;

    // ---- EPIC-0010 arrangement playback ----------------------------------
    std::unique_ptr<Daw::DawPlaybackController>       playbackController;
    double                                            lastRuntimeRate_ { 44100.0 };

    // ---- EPIC-0011 automation capture + playback -------------------------
    // Declaration order is destruction-safe and respects the dependency chain:
    // the model owns the `automation` ValueTree node; the append sink references
    // the model; the capture components + applier reference the sink/model. So
    // the model MUST outlive the sink, and the sink MUST outlive every capture
    // component (reverse-order destruction guarantees this).
    std::unique_ptr<Daw::AutomationModel>                automationModel;
    std::unique_ptr<Daw::ModelAutomationAppendSink>      automationSink;
    std::unique_ptr<Daw::AutomationCaptureTaps>          automationCaptureTaps;
    std::unique_ptr<Daw::ChannelBooleanAutomationCapture> channelBooleanCapture;
    std::unique_ptr<Daw::MasterTempoAutomationCapture>   masterTempoCapture;
    std::unique_ptr<Daw::AutomationApplier>              automationApplier;

    // Record/transport state-edge tracking for the 30 Hz timer (message thread).
    bool prevRecordingActive_ { false };
    bool prevTransportPlaying_ { false };

    static constexpr int toolbarHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainContentComponent)
};
