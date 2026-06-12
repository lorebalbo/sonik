#include "AudioEngine.h"
#include "../Quantize/QuantizeService.h"
#include "../Sync/MasterClockPublisher.h"
#include "../Sync/SyncEngine.h"
#include "../Sync/PhaseLockEngine.h"
#include <cmath>
#include <algorithm>

AudioEngine::AudioEngine (juce::ValueTree parentState)
    : rootState (parentState)
{
    for (auto& slot : deckSlots)
        slot.store (nullptr, std::memory_order_relaxed);

    // Create the AudioDevice state node and attach to the tree.
    audioDeviceNode = juce::ValueTree (IDs::AudioDevice);
    audioDeviceNode.setProperty (IDs::deviceName,      "",      nullptr);
    audioDeviceNode.setProperty (IDs::sampleRate,       0.0,     nullptr);
    audioDeviceNode.setProperty (IDs::bufferSize,       0,       nullptr);
    audioDeviceNode.setProperty (IDs::outputLatencyMs,  0.0,     nullptr);
    audioDeviceNode.setProperty (IDs::cpuLoad,          0.0f,    nullptr);
    audioDeviceNode.setProperty (IDs::cpuOverload,      false,   nullptr);
    audioDeviceNode.setProperty (IDs::deviceError,      "",      nullptr);
    rootState.addChild (audioDeviceNode, -1, nullptr);

    // PRD-0053: Pre-allocate pipeline scratch buffers to a conservative
    // default size so the audio callback is safe even when invoked in tests
    // without a preceding audioDeviceAboutToStart call. The real device
    // call will resize these to the true block size in audioDeviceAboutToStart.
    constexpr size_t kDefaultBlockSize = 2048;
    for (size_t s = 0; s < 4; ++s)
    {
        deckScratchL[s].assign (kDefaultBlockSize, 0.0f);
        deckScratchR[s].assign (kDefaultBlockSize, 0.0f);
        channelScratchL[s].assign (kDefaultBlockSize, 0.0f);
        channelScratchR[s].assign (kDefaultBlockSize, 0.0f);
        channelStrips[s].prepareToPlay (44100.0, static_cast<int>(kDefaultBlockSize), 2);
    }
    busAL.assign (kDefaultBlockSize, 0.0f);
    busAR.assign (kDefaultBlockSize, 0.0f);
    busBL.assign (kDefaultBlockSize, 0.0f);
    busBR.assign (kDefaultBlockSize, 0.0f);
    masterScratchL.assign (kDefaultBlockSize, 0.0f);
    masterScratchR.assign (kDefaultBlockSize, 0.0f);
    crossfaderStage.prepareToPlay (44100.0, static_cast<int>(kDefaultBlockSize), 2);
    masterStage.prepareToPlay     (44100.0, static_cast<int>(kDefaultBlockSize), 2);
}

AudioEngine::~AudioEngine()
{
    stop();

    // Flush all deferred-delete stretcher slots.
    // After stop(), the audio callback is no longer running, so all
    // pending stretchers can be freed safely.
    for (auto& src : deckSources)
    {
        delete src.timeStretcherOwned;
        src.timeStretcherOwned = nullptr;
        delete src.timeStretcherPendingDelete;
        src.timeStretcherPendingDelete = nullptr;

        for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
        {
            delete src.stemTimeStretchersOwned[s];
            src.stemTimeStretchersOwned[s] = nullptr;
            delete src.stemStretchersPendingDelete[s];
            src.stemStretchersPendingDelete[s] = nullptr;
        }
    }

    rootState.removeChild (audioDeviceNode, nullptr);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AudioEngine::start()
{
    shuttingDown = false;

    auto result = deviceManager.initialise (
        0,        // numInputChannels
        2,        // numOutputChannels
        nullptr,  // savedState XML
        true,     // selectDefaultDeviceOnFailure
        {},       // preferredDefaultDeviceName
        nullptr   // preferredSetupOptions
    );

    if (result.isNotEmpty())
    {
        audioDeviceNode.setProperty (IDs::deviceError, result, nullptr);
        return;
    }

    // Try to set desired sample rate / buffer size.
    if (deviceManager.getCurrentAudioDevice() != nullptr)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.sampleRate = 44100.0;
        setup.bufferSize = 128;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    deviceManager.addAudioCallback (this);
    deviceManager.addChangeListener (this);

    updateDeviceState();
    startTimer (timerIntervalMs);
}

void AudioEngine::stop()
{
    shuttingDown = true;
    stopTimer();

    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

// ---------------------------------------------------------------------------
// Master Clock wiring (PRD-0026, message thread)
// ---------------------------------------------------------------------------

void AudioEngine::setMasterClockPublisher (MasterClockPublisher* publisher)
{
    cachedClockPublisher_ = publisher;  // cache for audio-thread master-playhead write

    for (auto& src : deckSources)
        src.masterClockPublisher.store (publisher, std::memory_order_release);

    // Create or destroy per-deck sync engines (PRD-0027)
    for (int i = 0; i < 4; ++i)
        deckSyncEngines[static_cast<size_t> (i)] = publisher ? std::make_unique<SyncEngine> (*publisher) : nullptr;

    // Create or destroy per-deck phase lock engines (PRD-0028)
    for (int i = 0; i < 4; ++i)
        deckPhaseLockEngines[static_cast<size_t> (i)] = publisher ? std::make_unique<PhaseLockEngine> (*publisher) : nullptr;
}

// ---------------------------------------------------------------------------
// PRD-0041: MIDI bridge wiring (message thread) + audio-thread handler stub.
// ---------------------------------------------------------------------------

void AudioEngine::setMidiMessageBridge (sonik::midi::MidiMessageBridge* bridge)
{
    JUCE_ASSERT_MESSAGE_THREAD;
    midiBridge.store (bridge, std::memory_order_release);
}

void AudioEngine::setMixerAtomicSnapshot (MixerAtomicSnapshot* snapshot) noexcept
{
    // Store with release so the audio thread's acquire-load sees the
    // fully-constructed snapshot object.
    mixerAtomicSnapshot.store (snapshot, std::memory_order_release);
}

void AudioEngine::setMixerMeterSnapshot (MixerMeterSnapshot* snapshot) noexcept
{
    // PRD-0058: wire each ChannelStripProcessor and the MasterStage with the
    // slot they will publish into. Called once from the message thread
    // before the audio device starts; the meter slot pointers are stable
    // for the lifetime of the application.
    JUCE_ASSERT_MESSAGE_THREAD;
    mixerMeterSnapshot.store (snapshot, std::memory_order_release);
    for (int i = 0; i < 4; ++i)
        channelStrips[static_cast<size_t> (i)].setMeterSlot (
            snapshot != nullptr ? &snapshot->channels[i] : nullptr);
    masterStage.setMeterSlot (snapshot != nullptr ? &snapshot->master : nullptr);
}

void AudioEngine::setDawPlayback (Daw::ArrangementPublisher* publisher,
                                   Daw::ClipStreamerPool*     streamerPool,
                                   Daw::DawTransport*         transport) noexcept
{
    // PRD-0082: wire the DAW arrangement pipeline.
    // All pointers are published with release so the audio thread's acquire
    // reads see the fully-constructed objects.
    JUCE_ASSERT_MESSAGE_THREAD;
    dawPublisher_.store   (publisher,    std::memory_order_release);
    dawStreamerPool_.store (streamerPool, std::memory_order_release);
    dawTransport_.store   (transport,    std::memory_order_release);

    // If the device is already running, the renderer was not built during
    // audioDeviceAboutToStart (pointers were still null then). Build it now so
    // playback works without requiring a device restart.
    rebuildDawRenderer();
}

void AudioEngine::setMetronomeEnabled (bool enabled) noexcept
{
    metronomeEnabled_.store (enabled, std::memory_order_release);
}

void AudioEngine::setMetronomeGrid (double  beatLenRuntimeSamples,
                                    int64_t phaseOriginRuntimeSamples,
                                    int     beatsPerBar) noexcept
{
    metroBeatLenRuntime_.store     (beatLenRuntimeSamples,     std::memory_order_relaxed);
    metroPhaseOriginRuntime_.store (phaseOriginRuntimeSamples, std::memory_order_relaxed);
    metroBeatsPerBar_.store        (beatsPerBar,               std::memory_order_relaxed);
}

void AudioEngine::rebuildDawRenderer()
{
    JUCE_ASSERT_MESSAGE_THREAD;

    auto* publisher    = dawPublisher_.load    (std::memory_order_acquire);
    auto* streamerPool = dawStreamerPool_.load (std::memory_order_acquire);
    auto* transport    = dawTransport_.load    (std::memory_order_acquire);

    if (publisher == nullptr || streamerPool == nullptr || transport == nullptr)
        return;

    const int    blockSize = currentBufferSize.load (std::memory_order_relaxed);
    const double sr        = currentSampleRate.load (std::memory_order_relaxed);
    if (blockSize <= 0 || sr <= 0.0)
        return; // device not prepared yet — audioDeviceAboutToStart will build it

    // Ensure the master-feed scratch is sized (also done in audioDeviceAboutToStart).
    if (static_cast<int> (dawMasterFeedL_.size()) < blockSize)
    {
        dawMasterFeedL_.assign (static_cast<size_t> (blockSize), 0.0f);
        dawMasterFeedR_.assign (static_cast<size_t> (blockSize), 0.0f);
    }

    // Build a fully-prepared renderer in a local before publishing its pointer,
    // so the audio thread never observes a half-prepared renderer. When called
    // from audioDeviceAboutToStart the audio callback is stopped (safe to swap);
    // when called from setDawPlayback at runtime the audio thread still sees the
    // old (typically null) pointer until the atomic store below.
    auto fresh = std::make_unique<Daw::TimelineRenderer> (
        *publisher, *streamerPool, transport->playheadAtomic());
    fresh->prepare (sr, blockSize, Daw::kMaxLanes, Daw::kMaxClipsPerLane);

    dawRendererPtr_.store (fresh.get(), std::memory_order_release);
    dawRenderer_ = std::move (fresh);
}

void AudioEngine::applyAudioMidiEvent (const sonik::midi::MidiAudioEvent& event) noexcept
{
    // The jog/scratch state-machine that consumes these events lives in
    // PRD-0042/0044. PRD-0041 only guarantees the transport; this handler
    // is intentionally a no-op until those PRDs land. It must remain
    // noexcept, allocation-free, and lock-free.
    juce::ignoreUnused (event);
}

// ---------------------------------------------------------------------------
// Deck registration (message thread)
// ---------------------------------------------------------------------------

void AudioEngine::registerDeck (const juce::String& deckId, DeckAudioState* audioState)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];
    src.audioState    = audioState;
    src.channelL.store (nullptr, std::memory_order_relaxed);
    src.channelR.store (nullptr, std::memory_order_relaxed);
    src.bufferNumFrames.store (0, std::memory_order_relaxed);
    src.bufferHolder = nullptr;
    src.peakL.store (0.0f, std::memory_order_relaxed);
    src.peakR.store (0.0f, std::memory_order_relaxed);
    src.rmsL.store (0.0f, std::memory_order_relaxed);
    src.rmsR.store (0.0f, std::memory_order_relaxed);

    // Reset transport state (PRD-0004)
    src.pendingCommand.store (0, std::memory_order_relaxed);
    src.seekTarget.store (0, std::memory_order_relaxed);
    src.playheadAccumulator    = 0.0;
    src.isCuePreviewing        = false;
    src.fadeRampSamplesRemaining = 0;
    src.isFadingIn             = false;
    src.isFadingOut            = false;
    src.deferredAction         = DeckAudioSource::DeferredAction::None;
    src.deferredSeekTarget     = 0;
    src.deferredIsSlipDisplacement = false;
    src.loopFadeRemaining      = 0;
    src.loopCrossfadeLength    = 64;
    src.loopFadeReadPos        = 0.0;

    // Slip mode (PRD-0017)
    src.shadowPlayheadAccumulator = 0.0;
    src.slipDisplacedLocal        = false;
    src.wasLoopActiveLastBlock    = false;

    if (src.audioState != nullptr)
    {
        src.audioState->scratchActive.store (false, std::memory_order_relaxed);
        src.audioState->scratchTargetSample.store (0, std::memory_order_relaxed);
        src.audioState->scratchVelocityPerSample.store (0.0f, std::memory_order_relaxed);
    }
    src.prevScratchVelocityRead = 0.0f;
    src.scratchVelocityDecayed  = 0.0f;

    // Stem state (PRD-0021)
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        src.stemChannelL[s].store (nullptr, std::memory_order_relaxed);
        src.stemChannelR[s].store (nullptr, std::memory_order_relaxed);
        src.stemBufferNumFrames[s].store (0, std::memory_order_relaxed);
        src.stemBufferHolders[s] = nullptr;
        src.stemFadeRemaining[s] = 0;
        src.stemFadeDirection[s] = false;
        src.stemWasMuted[s]      = false;
    }
    src.stemsActive.store (false, std::memory_order_relaxed);
    src.stemsActivationFadeRemaining = 0;
    src.stemsActivationFadeDirection = false;
    src.wasStemsActiveLocal          = false;

    // Stem stretchers (PRD-0022)
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        src.stemTimeStretchers[s].store(nullptr, std::memory_order_relaxed);
        src.stemTimeStretchersOwned[s] = nullptr;
    }
    src.stemStretcherLatency = 0;
    src.stemStretchDegraded.store(false, std::memory_order_relaxed);

    // Publish pointer — use release so the audio thread sees all writes above.
    deckSlots[static_cast<size_t> (slot)].store (&src, std::memory_order_release);
}

void AudioEngine::unregisterDeck (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    // Atomically hide the source from the audio thread first.
    deckSlots[static_cast<size_t> (slot)].store (nullptr, std::memory_order_release);

    // Safe to clear after pointer is nulled — audio thread won't read anymore.
    auto& src = deckSources[static_cast<size_t> (slot)];
    src.audioState = nullptr;
    src.peakL.store (0.0f, std::memory_order_relaxed);
    src.peakR.store (0.0f, std::memory_order_relaxed);
    src.rmsL.store (0.0f, std::memory_order_relaxed);
    src.rmsR.store (0.0f, std::memory_order_relaxed);

    // Flush deferred-delete slots — the deck is fully hidden from the
    // audio thread now, so all pending stretchers can be freed safely.
    delete src.timeStretcherOwned;
    src.timeStretcherOwned = nullptr;
    delete src.timeStretcherPendingDelete;
    src.timeStretcherPendingDelete = nullptr;

    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        delete src.stemTimeStretchersOwned[s];
        src.stemTimeStretchersOwned[s] = nullptr;
        delete src.stemStretchersPendingDelete[s];
        src.stemStretchersPendingDelete[s] = nullptr;
    }
}

void AudioEngine::setDeckBuffer (const juce::String& deckId, AudioBufferHolder::Ptr holder)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    if (holder != nullptr && holder->getBuffer().getNumChannels() >= 2)
    {
        // PRD-0021: Clear stems on new track load, reset stem mute atomics (AC#20, AC#24)
        clearDeckStemBuffers (deckId);
        if (src.audioState != nullptr)
        {
            src.audioState->stemVocalsMuted.store (false, std::memory_order_relaxed);
            src.audioState->stemDrumsMuted.store (false, std::memory_order_relaxed);
            src.audioState->stemBassMuted.store (false, std::memory_order_relaxed);
            src.audioState->stemOtherMuted.store (false, std::memory_order_relaxed);
        }

        // Reset transport state so the new track loads stopped at position 0
        src.pendingCommand.store (0, std::memory_order_relaxed);
        src.seekTarget.store (0, std::memory_order_relaxed);
        src.playheadAccumulator = 0.0;
        src.isCuePreviewing = false;
        src.fadeRampSamplesRemaining = 0;
        src.isFadingIn = false;
        src.isFadingOut = false;
        src.deferredIsSlipDisplacement = false;
        src.loopFadeRemaining   = 0;
        src.loopCrossfadeLength = 64;
        src.loopFadeReadPos     = 0.0;

        // Slip mode (PRD-0017): reset shadow on track load
        src.shadowPlayheadAccumulator = 0.0;
        src.slipDisplacedLocal        = false;
        src.wasLoopActiveLastBlock    = false;

        if (src.audioState != nullptr)
        {
            src.audioState->playbackStatus.store (1, std::memory_order_relaxed); // stopped
            src.audioState->playheadPosition.store (0, std::memory_order_relaxed);
            src.audioState->tempCuePosition.store (0, std::memory_order_relaxed);
            src.audioState->scratchActive.store (false, std::memory_order_relaxed);
            src.audioState->scratchTargetSample.store (0, std::memory_order_relaxed);
            src.audioState->scratchVelocityPerSample.store (0.0f, std::memory_order_relaxed);
            // PRD-0017: reset slip atomics
            src.audioState->slipShadowPosition.store (0.0, std::memory_order_relaxed);
            src.audioState->slipDisplaced.store (false, std::memory_order_relaxed);
        }
        src.prevScratchVelocityRead = 0.0f;
        src.scratchVelocityDecayed  = 0.0f;

        // Store the holder first (message thread ownership)
        src.bufferHolder = holder;

        // Create time stretcher for this deck (message thread, allocates buffers)
        {
            double sr = currentSampleRate.load (std::memory_order_relaxed);
            int maxBlock = currentBufferSize.load (std::memory_order_relaxed);
            if (maxBlock <= 0) maxBlock = 512;

            // Tear down old stretcher: hide from audio thread, then defer
            // deletion so any in-flight audio callback finishes safely.
            src.timeStretcher.store (nullptr, std::memory_order_release);

            // Delete the *previous* pending-delete (safe — it has been hidden
            // for at least one full setDeckBuffer cycle).
            delete src.timeStretcherPendingDelete;
            // Move the current stretcher into the pending-delete slot instead
            // of deleting it immediately — the audio thread may still be
            // inside process() on this very object.
            src.timeStretcherPendingDelete = src.timeStretcherOwned;

            auto* newStretcher = new TimeStretcher (sr, 2, maxBlock * 4);

            // Prime with actual track audio so the stretcher output is
            // immediately valid and latency-aligned with the vinyl path.
            // primeWithAudio() returns the effective pipeline depth — the
            // exact read-ahead offset that makes stretched output align with
            // the vinyl path (zero phase offset at Key Lock toggle).
            const auto& buf = holder->getBuffer();
            src.stretcherLatency = newStretcher->primeWithAudio (
                buf.getReadPointer (0),
                buf.getReadPointer (1),
                holder->getNumFrames());
            src.timeStretcherOwned = newStretcher;
            src.timeStretcher.store (newStretcher, std::memory_order_release);

            // Reset key lock crossfade state
            src.wasKeyLockEnabled = false;
            src.keyLockFadeSamplesRemaining = 0;
            src.keyLockFadingIn = false;
            src.keyLockFadingOut = false;
            src.smoothedTimeRatio = 1.0;
            src.stretchInputSampleCarry = 0.0;
            src.stemStretchInputSampleCarry = 0.0;

            // PRD-0022: Stem stretchers will be created when stems become active + key lock
            destroyStemStretchers(deckId);
        }

        // Publish raw pointers atomically for the audio thread
        src.bufferNumFrames.store (holder->getNumFrames(), std::memory_order_relaxed);
        src.channelR.store (holder->getBuffer().getReadPointer (1), std::memory_order_release);
        src.channelL.store (holder->getBuffer().getReadPointer (0), std::memory_order_release);
    }
    else
    {
        clearDeckBuffer (deckId);
    }
}

AudioBufferHolder::Ptr AudioEngine::getDeckBuffer (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return nullptr;

    return deckSources[static_cast<size_t> (slot)].bufferHolder;
}

void AudioEngine::clearDeckBuffer (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    // PRD-0021: Clear stems on eject, reset stem mute atomics (AC#19, AC#24)
    clearDeckStemBuffers (deckId);
    if (src.audioState != nullptr)
    {
        src.audioState->stemVocalsMuted.store (false, std::memory_order_relaxed);
        src.audioState->stemDrumsMuted.store (false, std::memory_order_relaxed);
        src.audioState->stemBassMuted.store (false, std::memory_order_relaxed);
        src.audioState->stemOtherMuted.store (false, std::memory_order_relaxed);
        src.audioState->scratchActive.store (false, std::memory_order_relaxed);
        src.audioState->scratchTargetSample.store (0, std::memory_order_relaxed);
        src.audioState->scratchVelocityPerSample.store (0.0f, std::memory_order_relaxed);
    }
    src.prevScratchVelocityRead = 0.0f;
    src.scratchVelocityDecayed  = 0.0f;

    // Null the pointers first so the audio thread stops reading
    src.channelL.store (nullptr, std::memory_order_release);
    src.channelR.store (nullptr, std::memory_order_release);
    src.bufferNumFrames.store (0, std::memory_order_relaxed);

    // Destroy time stretcher — hide from audio thread, then defer deletion
    // so any in-flight audio callback finishes safely before the object is freed.
    src.timeStretcher.store (nullptr, std::memory_order_release);
    delete src.timeStretcherPendingDelete;
    src.timeStretcherPendingDelete = src.timeStretcherOwned;
    src.timeStretcherOwned = nullptr;
    src.stretcherLatency = 0;
    src.smoothedTimeRatio = 1.0;
    src.stretchInputSampleCarry = 0.0;
    src.stemStretchInputSampleCarry = 0.0;

    // PRD-0022: Tear down stem stretchers on eject
    destroyStemStretchers(deckId);

    // Release ownership
    src.bufferHolder = nullptr;

    // Now safe to release deferred stem holders — audio thread sees no deck data
    for (int i = 0; i < DeckAudioSource::NUM_STEMS; ++i)
        src.stemBufferHolders[i] = nullptr;
}

// ---------------------------------------------------------------------------
// Stem buffer management (PRD-0021, message thread only)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Stem buffer management (PRD-0021, message thread only)
// ---------------------------------------------------------------------------

void AudioEngine::setDeckStemBuffers (const juce::String& deckId,
                                       AudioBufferHolder::Ptr vocals,
                                       AudioBufferHolder::Ptr drums,
                                       AudioBufferHolder::Ptr bass,
                                       AudioBufferHolder::Ptr other)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    AudioBufferHolder::Ptr holders[DeckAudioSource::NUM_STEMS] = {
        vocals, drums, bass, other
    };

    // Store holders first (message thread ownership, AC#3)
    for (int i = 0; i < DeckAudioSource::NUM_STEMS; ++i)
        src.stemBufferHolders[i] = holders[i];

    // Extract raw pointers and publish with release stores (AC#21)
    // All 4 pointer pairs stored BEFORE stemsActive set to true.
    for (int i = 0; i < DeckAudioSource::NUM_STEMS; ++i)
    {
        if (holders[i] != nullptr && holders[i]->getBuffer().getNumChannels() >= 2)
        {
            src.stemBufferNumFrames[i].store (holders[i]->getNumFrames(),
                                              std::memory_order_relaxed);
            src.stemChannelR[i].store (holders[i]->getBuffer().getReadPointer (1),
                                       std::memory_order_relaxed);
            src.stemChannelL[i].store (holders[i]->getBuffer().getReadPointer (0),
                                       std::memory_order_release);
        }
        else
        {
            src.stemChannelL[i].store (nullptr, std::memory_order_relaxed);
            src.stemChannelR[i].store (nullptr, std::memory_order_relaxed);
            src.stemBufferNumFrames[i].store (0, std::memory_order_relaxed);
        }
    }

    // Set stemsActive AFTER all pointers stored (AC#5, AC#21)
    // PRD-0062: buffer presence alone no longer activates stems. The deck stays
    // on the original source until the DJ deliberately selects "stems" via
    // setDeckSourceMode; stemsActive is derived from the sourceMode property.
    // Both the original buffer and the four stem buffers are now retained so
    // the source can be switched instantly and click-free.
}

void AudioEngine::setDeckSourceMode (const juce::String& deckId, bool useStems)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    if (useStems)
    {
        // Locked to original unless stem buffers actually exist.
        bool haveStemBuffers = false;
        for (int i = 0; i < DeckAudioSource::NUM_STEMS; ++i)
        {
            if (src.stemBufferNumFrames[i].load (std::memory_order_relaxed) > 0)
            {
                haveStemBuffers = true;
                break;
            }
        }
        if (! haveStemBuffers)
            return;

        // PRD-0062 §1.5.6: if key-lock is engaged, build and publish the stem
        // stretcher set BEFORE flipping stemsActive, so the audio thread (which
        // reads the stretcher pointers and stemsActive with acquire semantics)
        // never observes a half-built set.
        if (src.audioState != nullptr
            && src.audioState->keyLockEnabled.load (std::memory_order_relaxed))
        {
            createStemStretchers (deckId);
        }

        src.stemsActive.store (true, std::memory_order_release);
    }
    else
    {
        // The audio thread crossfades back to the original buffer. The stem
        // stretcher set (if any) is retired by the PRD-0022 reconciliation in
        // timerCallback, via the established deferred-deletion path.
        src.stemsActive.store (false, std::memory_order_release);
    }
}

void AudioEngine::clearDeckStemBuffers (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    // PRD-0022: Tear down stem stretchers
    destroyStemStretchers(deckId);

    // Deactivate first (AC#6) — audio thread will stop reading stem data
    src.stemsActive.store (false, std::memory_order_release);

    // PRD-0062 AC: clearing the stems forces the deck's source mode back to
    // "original" (there is no longer a stem source to play). Message-thread only.
    auto decksNode = rootState.getChildWithName (IDs::Decks);
    if (decksNode.isValid())
    {
        auto deckTree = decksNode.getChildWithProperty (IDs::id, deckId);
        if (deckTree.isValid())
            deckTree.setProperty (IDs::sourceMode, "original", nullptr);
    }

    // Null all stem channel pointers
    for (int i = 0; i < DeckAudioSource::NUM_STEMS; ++i)
    {
        src.stemChannelL[i].store (nullptr, std::memory_order_release);
        src.stemChannelR[i].store (nullptr, std::memory_order_relaxed);
        src.stemBufferNumFrames[i].store (0, std::memory_order_relaxed);
    }

    // IMPORTANT: Do NOT release stemBufferHolders here.
    // The audio thread may still be in a 64-sample deactivation crossfade,
    // reading from the stem buffer pointers it captured before seeing stemsActive=false.
    // Releasing holders would free the underlying buffers → use-after-free.
    // Instead, let the next setDeckStemBuffers() or setDeckBuffer() overwrite
    // them naturally, at which point the audio thread will have finished
    // any deactivation crossfade.
}

// ---------------------------------------------------------------------------
// Stem time stretcher management (PRD-0022, message thread only)
// ---------------------------------------------------------------------------

void AudioEngine::createStemStretchers (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0) return;
    auto& src = deckSources[static_cast<size_t> (slot)];

    // Must have valid stem buffers. Gate on buffer presence (not stemsActive):
    // PRD-0062 §1.5.6 builds the stretcher set BEFORE flipping stemsActive, so
    // this can legitimately be called while stemsActive is still false.
    bool haveStemBuffers = false;
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        if (src.stemBufferNumFrames[s].load (std::memory_order_relaxed) > 0)
        {
            haveStemBuffers = true;
            break;
        }
    }
    if (! haveStemBuffers)
        return;

    double sr = currentSampleRate.load (std::memory_order_relaxed);
    int maxBlock = currentBufferSize.load (std::memory_order_relaxed);
    if (maxBlock <= 0) maxBlock = 512;

    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        // Hide old stretcher from audio thread, defer deletion
        src.stemTimeStretchers[s].store (nullptr, std::memory_order_release);
        delete src.stemStretchersPendingDelete[s];
        src.stemStretchersPendingDelete[s] = src.stemTimeStretchersOwned[s];

        auto* stretcher = new TimeStretcher (sr, 2, maxBlock * 4);

        // Prime with stem audio
        auto* stemL = src.stemChannelL[s].load (std::memory_order_relaxed);
        auto* stemR = src.stemChannelR[s].load (std::memory_order_relaxed);
        auto  stemLen = src.stemBufferNumFrames[s].load (std::memory_order_relaxed);

        if (stemL != nullptr && stemR != nullptr && stemLen > 0)
        {
            int eff = stretcher->primeWithAudio (stemL, stemR, static_cast<int>(stemLen));
            // Store latency from first stem (all stems share same stretcher params)
            if (s == 0)
                src.stemStretcherLatency = eff;
        }
        else
        {
            stretcher->prime();
            if (s == 0)
                src.stemStretcherLatency = stretcher->getLatency();
        }

        src.stemTimeStretchersOwned[s] = stretcher;
    }

    src.stemStretchInputSampleCarry = 0.0;

    // Publish all 4 atomically via release stores
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
        src.stemTimeStretchers[s].store (src.stemTimeStretchersOwned[s], std::memory_order_release);

    src.stemStretchDegraded.store (false, std::memory_order_relaxed);
}

void AudioEngine::destroyStemStretchers (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0) return;
    auto& src = deckSources[static_cast<size_t> (slot)];

    // Hide from audio thread first
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
        src.stemTimeStretchers[s].store (nullptr, std::memory_order_release);

    // Defer deletion — the audio thread may still be mid-process() on the
    // old stretcher.  Move to pending-delete; the *previous* pending-delete
    // is safe to free (it has been hidden for at least one full cycle).
    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
    {
        delete src.stemStretchersPendingDelete[s];
        src.stemStretchersPendingDelete[s] = src.stemTimeStretchersOwned[s];
        src.stemTimeStretchersOwned[s] = nullptr;
    }

    src.stemStretcherLatency = 0;
    src.stemStretchInputSampleCarry = 0.0;
    src.stemStretchDegraded.store (false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// AudioIODeviceCallback
// ---------------------------------------------------------------------------

void AudioEngine::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (inputChannelData, numInputChannels, context);

    const auto startTicks = juce::Time::getHighResolutionTicks();

    // PRD-0041: drain the lock-free MIDI FIFO BEFORE any audio processing so
    // jog/scratch state lands in atomics before this block reads them.
    if (auto* bridge = midiBridge.load (std::memory_order_acquire))
        bridge->drainAudioThreadFifo (*this);

    // Clear output buffers.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    // PRD-0053: clear per-deck scratch buffers so paused/stopped decks do not
    // bleed stale audio from the previous block through the mixer pipeline.
    // (The deck loop only writes to deckScratchL/R when actively generating
    // samples; a paused deck skips that write entirely.)
    for (size_t s = 0; s < 4; ++s)
    {
        juce::FloatVectorOperations::clear (deckScratchL[s].data(), numSamples);
        juce::FloatVectorOperations::clear (deckScratchR[s].data(), numSamples);
    }

    if (numOutputChannels < 2 || numSamples == 0)
        return;

    auto* outL = outputChannelData[0];
    auto* outR = outputChannelData[1];

    // PRD-0028 phase fix: publish the master deck's current playhead position
    // so PhaseLockEngine can compute phase relative to the master's beat grid.
    // Runs BEFORE the per-deck loop, using each deck's playheadAccumulator from
    // the end of the previous block (1-block lag, ~10 ms — imperceptible for sync).
    if (cachedClockPublisher_ != nullptr)
    {
        const int masterSlot =
            cachedClockPublisher_->masterSlotIndex.load (std::memory_order_relaxed);
        if (masterSlot >= 0 && masterSlot < 4)
        {
            auto* masterSrc =
                deckSlots[static_cast<size_t> (masterSlot)].load (std::memory_order_acquire);
            if (masterSrc != nullptr)
                cachedClockPublisher_->masterPlayheadSample.store (
                    static_cast<int64_t> (masterSrc->playheadAccumulator),
                    std::memory_order_relaxed);
        }
    }

    // Mix each registered deck into the output.
    for (size_t slot = 0; slot < 4; ++slot)
    {
        auto* source = deckSlots[slot].load (std::memory_order_acquire);
        if (source == nullptr)
            continue;

        auto* audioState = source->audioState;
        if (audioState == nullptr)
            continue;

        // 1. Check buffer — if null, output silence for this deck
        auto* chL   = source->channelL.load (std::memory_order_acquire);
        auto* chR   = source->channelR.load (std::memory_order_acquire);
        auto  bufLen = source->bufferNumFrames.load (std::memory_order_relaxed);

        if (chL == nullptr || chR == nullptr || bufLen <= 0)
        {
            source->peakL.store (0.0f, std::memory_order_relaxed);
            source->peakR.store (0.0f, std::memory_order_relaxed);
            source->rmsL.store  (0.0f, std::memory_order_relaxed);
            source->rmsR.store  (0.0f, std::memory_order_relaxed);
            continue;
        }

        // 2. Read pending command
        auto cmd = static_cast<TransportCommand> (
            source->pendingCommand.exchange (0, std::memory_order_relaxed));

        auto status = static_cast<PlaybackStatusCode> (
            audioState->playbackStatus.load (std::memory_order_relaxed));

        // 3. Process command
        switch (cmd)
        {
            case TransportCommand::Play:
                if (status == PlaybackStatusCode::stopped
                    || status == PlaybackStatusCode::paused)
                {
                    audioState->playbackStatus.store (
                        static_cast<int> (PlaybackStatusCode::playing),
                        std::memory_order_relaxed);
                    status = PlaybackStatusCode::playing;
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingIn  = true;
                    source->isFadingOut = false;
                }
                break;

            case TransportCommand::Pause:
                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction = DeckAudioSource::DeferredAction::Pause;
                }
                break;

            case TransportCommand::Stop:
                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction = DeckAudioSource::DeferredAction::Stop;
                }
                else if (status == PlaybackStatusCode::paused)
                {
                    audioState->playbackStatus.store (
                        static_cast<int> (PlaybackStatusCode::stopped),
                        std::memory_order_relaxed);
                    status = PlaybackStatusCode::stopped;
                    source->playheadAccumulator = 0.0;
                    // PRD-0017: Stop resets both playheads
                    source->shadowPlayheadAccumulator = 0.0;
                    source->slipDisplacedLocal = false;
                }
                else if (status == PlaybackStatusCode::stopped)
                {
                    // Already stopped — just ensure slip state is clean
                    source->playheadAccumulator = 0.0;
                    source->shadowPlayheadAccumulator = 0.0;
                    source->slipDisplacedLocal = false;
                }
                break;

            case TransportCommand::Seek:
            {
                auto target = source->seekTarget.load (std::memory_order_relaxed);
                target = juce::jlimit<int64_t> (0, bufLen - 1, target);

                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction     = DeckAudioSource::DeferredAction::Seek;
                    source->deferredSeekTarget = target;
                    source->deferredIsSlipDisplacement = false; // navigation seek
                }
                else
                {
                    source->playheadAccumulator = static_cast<double> (target);
                    // PRD-0017: navigation seek resets shadow
                    source->shadowPlayheadAccumulator = static_cast<double> (target);
                    source->slipDisplacedLocal = false;
                }
                break;
            }

            case TransportCommand::ScratchSeek:
            {
                auto target = source->seekTarget.load (std::memory_order_relaxed);
                target = juce::jlimit<int64_t> (0, bufLen - 1, target);

                if (status == PlaybackStatusCode::playing)
                {
                    // While actively playing, scratch-seek behaves like a
                    // regular seek (fade-out, reposition, fade-in).
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction     = DeckAudioSource::DeferredAction::Seek;
                    source->deferredSeekTarget = target;
                    source->deferredIsSlipDisplacement = false;
                }
                else
                {
                    // Paused/stopped: reposition instantly, then emit a
                    // short decaying burst without changing transport state.
                    source->playheadAccumulator = static_cast<double> (target);
                    source->shadowPlayheadAccumulator = static_cast<double> (target);
                    source->slipDisplacedLocal = false;

                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction = DeckAudioSource::DeferredAction::None;
                }
                break;
            }

            case TransportCommand::CueSet:
                if (status == PlaybackStatusCode::paused)
                {
                    auto curPos = static_cast<int64_t> (source->playheadAccumulator);

                    // Quantize snap (PRD-0013): snap temp cue to nearest beat if enabled
                    if (audioState->quantizeEnabled.load (std::memory_order_relaxed))
                    {
                        auto anchor   = audioState->beatgridAnchor.load (std::memory_order_relaxed);
                        auto interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
                        curPos = QuantizeService::snapToNearestBeat (curPos, anchor, interval);
                    }

                    auto curCue = audioState->tempCuePosition.load (std::memory_order_relaxed);
                    if (curPos != curCue)
                        audioState->tempCuePosition.store (curPos, std::memory_order_relaxed);
                }
                break;

            case TransportCommand::CueReturn:
                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction = DeckAudioSource::DeferredAction::CueReturn;
                    source->isCuePreviewing = false;
                }
                break;

            case TransportCommand::CuePreview:
                if (status == PlaybackStatusCode::paused)
                {
                    auto cuePos = audioState->tempCuePosition.load (std::memory_order_relaxed);
                    if (cuePos >= 0
                        && static_cast<int64_t> (source->playheadAccumulator) == cuePos)
                    {
                        audioState->playbackStatus.store (
                            static_cast<int> (PlaybackStatusCode::playing),
                            std::memory_order_relaxed);
                        status = PlaybackStatusCode::playing;
                        source->isCuePreviewing = true;
                        source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                        source->isFadingIn  = true;
                        source->isFadingOut = false;
                    }
                }
                break;

            case TransportCommand::CueRelease:
                if (source->isCuePreviewing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction  = DeckAudioSource::DeferredAction::CueReturn;
                    source->isCuePreviewing = false;
                }
                break;

            case TransportCommand::CuePlayThrough:
                source->isCuePreviewing = false;
                break;

            case TransportCommand::SeekAndPlay:
            {
                auto seekPos = source->seekTarget.load (std::memory_order_relaxed);
                seekPos = juce::jlimit<int64_t> (0, bufLen - 1, seekPos);

                if (status == PlaybackStatusCode::playing)
                {
                    // Already playing → fade-out then seek (same as regular Seek)
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction     = DeckAudioSource::DeferredAction::Seek;
                    source->deferredSeekTarget = seekPos;
                    source->deferredIsSlipDisplacement = false; // navigation
                }
                else
                {
                    // Stopped/paused → seek instantly then start playing with fade-in
                    source->playheadAccumulator = static_cast<double> (seekPos);
                    // PRD-0017: navigation seek resets shadow
                    source->shadowPlayheadAccumulator = static_cast<double> (seekPos);
                    source->slipDisplacedLocal = false;
                    audioState->playbackStatus.store (
                        static_cast<int> (PlaybackStatusCode::playing),
                        std::memory_order_relaxed);
                    status = PlaybackStatusCode::playing;
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingIn  = true;
                    source->isFadingOut = false;
                }
                break;
            }

            // --- Slip mode commands (PRD-0017) ---

            case TransportCommand::SlipSeek:
            {
                auto target = source->seekTarget.load (std::memory_order_relaxed);
                target = juce::jlimit<int64_t> (0, bufLen - 1, target);

                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction     = DeckAudioSource::DeferredAction::Seek;
                    source->deferredSeekTarget = target;
                    source->deferredIsSlipDisplacement = true; // displacement seek
                }
                else
                {
                    // Not playing: displacement from current shadow position
                    if (! source->slipDisplacedLocal)
                        source->slipDisplacedLocal = true;
                    source->playheadAccumulator = static_cast<double> (target);
                }
                break;
            }

            case TransportCommand::SlipSeekAndPlay:
            {
                auto seekPos = source->seekTarget.load (std::memory_order_relaxed);
                seekPos = juce::jlimit<int64_t> (0, bufLen - 1, seekPos);

                if (status == PlaybackStatusCode::playing)
                {
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction     = DeckAudioSource::DeferredAction::Seek;
                    source->deferredSeekTarget = seekPos;
                    source->deferredIsSlipDisplacement = true; // displacement seek
                }
                else
                {
                    // Stopped/paused → seek, mark displacement, start playing
                    if (! source->slipDisplacedLocal)
                        source->slipDisplacedLocal = true;
                    source->playheadAccumulator = static_cast<double> (seekPos);
                    audioState->playbackStatus.store (
                        static_cast<int> (PlaybackStatusCode::playing),
                        std::memory_order_relaxed);
                    status = PlaybackStatusCode::playing;
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingIn  = true;
                    source->isFadingOut = false;
                }
                break;
            }

            case TransportCommand::SlipReturn:
            {
                if (source->slipDisplacedLocal)
                {
                    auto shadowPos = static_cast<int64_t> (source->shadowPlayheadAccumulator);
                    auto primaryPos = static_cast<int64_t> (source->playheadAccumulator);

                    if (std::abs (shadowPos - primaryPos) < DeckAudioSource::FADE_RAMP_LENGTH)
                    {
                        // Close enough: no audible transition, just resync
                        source->playheadAccumulator = source->shadowPlayheadAccumulator;
                        source->slipDisplacedLocal = false;
                    }
                    else if (status == PlaybackStatusCode::playing)
                    {
                        // Full snap-back with crossfade
                        source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                        source->isFadingOut = true;
                        source->isFadingIn  = false;
                        source->deferredAction = DeckAudioSource::DeferredAction::SlipReturn;
                    }
                    else
                    {
                        // Not playing: instant resync
                        source->playheadAccumulator = source->shadowPlayheadAccumulator;
                        source->slipDisplacedLocal = false;
                    }
                }
                break;
            }

            case TransportCommand::None:
            default:
                break;
        }

        // Re-read status after command processing
        status = static_cast<PlaybackStatusCode> (
            audioState->playbackStatus.load (std::memory_order_relaxed));

        // 3b. Slip mode state (PRD-0017)
        bool slipEnabled = audioState->slipEnabled.load (std::memory_order_relaxed);

        // Waveform scratch state (PRD-0016 follow-up)
        const bool scratchMode = audioState->scratchActive.load (std::memory_order_acquire);
        int64_t scratchTarget = audioState->scratchTargetSample.load (std::memory_order_relaxed);
        scratchTarget = juce::jlimit<int64_t> (0, bufLen - 1, scratchTarget);

        // When slip is disabled, clear displacement state
        if (! slipEnabled)
            source->slipDisplacedLocal = false;

        // 3c. Sync engine (PRD-0027) — runs before speed is read
        {
            const bool curIsSynced = audioState->isSynced.load (std::memory_order_relaxed);
            if (curIsSynced)
            {
                auto* se = deckSyncEngines[slot].get();
                if (se != nullptr)
                    se->process (*audioState);
            }

            // SYNC-off behavior: keep the current speedMultiplier latched.
            // This preserves the exact tempo reached while synced.
            source->prevIsSynced = curIsSynced;
        }

        // 3d. Phase lock engine (PRD-0028) — refines correctionMultiplier after SyncEngine
        {
            auto* ple = deckPhaseLockEngines[slot].get();
            if (ple != nullptr)
                ple->process (*source, *audioState,
                              currentSampleRate.load (std::memory_order_relaxed));
        }

        // 4. Read speed (PRD-0028: correctionMultiplier applied multiplicatively)
        // Scratch mode uses the velocity published by the message thread
        // (samples/sample, signed for reverse playback).  Between message-thread
        // updates the local copy is decayed per-block to simulate the natural
        // deceleration of a vinyl platter – this ensures the audio plays
        // CONTINUOUSLY rather than making one burst and then going silent until
        // the next drag event arrives.
        constexpr float kScratchMaxSpeed     = 24.0f;
        constexpr float kScratchDeadband     = 0.0005f;
        // Decay per block ≈ 200 ms half-life at 44100/128 ≈ 344 blocks/s.
        // Chosen so the velocity drops to ~95% between typical 60 Hz mouse
        // events (5 blocks) – nearly imperceptible during active dragging –
        // while decaying cleanly to silence within ~200 ms if the user holds
        // the waveform still after a scratch.
        constexpr float kScratchVelocityDecay = 0.990f;

        float speed = 0.0f;
        if (scratchMode)
        {
            const float publishedVelocity =
                audioState->scratchVelocityPerSample.load (std::memory_order_relaxed);

            if (publishedVelocity != source->prevScratchVelocityRead)
            {
                // Message thread published a new velocity: adopt it immediately.
                source->scratchVelocityDecayed  = publishedVelocity;
                source->prevScratchVelocityRead = publishedVelocity;
            }
            else
            {
                // No new velocity this block: decay the local copy.
                source->scratchVelocityDecayed *= kScratchVelocityDecay;
            }

            speed = juce::jlimit (-kScratchMaxSpeed, kScratchMaxSpeed,
                                   source->scratchVelocityDecayed);

            if (std::abs (speed) < kScratchDeadband)
            {
                speed = 0.0f;
                source->scratchVelocityDecayed = 0.0f; // snap to zero to avoid sub-deadband drift
            }
        }
        else
        {
            speed = juce::jmax (0.0f,
                static_cast<float> (
                    static_cast<double> (audioState->speedMultiplier.load (std::memory_order_relaxed))
                    * source->correctionMultiplier));
        }

        // Quick path: not playing and no active fade → silence
        if (! scratchMode
            && status != PlaybackStatusCode::playing
            && source->fadeRampSamplesRemaining <= 0)
        {
            audioState->playheadPosition.store (
                static_cast<int64_t> (source->playheadAccumulator),
                std::memory_order_relaxed);
            // Publish slip state even when silent
            audioState->slipShadowPosition.store (source->shadowPlayheadAccumulator,
                                                  std::memory_order_relaxed);
            audioState->slipDisplaced.store (source->slipDisplacedLocal,
                                            std::memory_order_relaxed);
            source->peakL.store (0.0f, std::memory_order_relaxed);
            source->peakR.store (0.0f, std::memory_order_relaxed);
            source->rmsL.store  (0.0f, std::memory_order_relaxed);
            source->rmsR.store  (0.0f, std::memory_order_relaxed);
            continue;
        }

        // 5. Generate samples
        // PRD-0054: gain is now applied inside ChannelStripProcessor with smoothing.
        // audioState->gain is no longer consumed on the audio thread.
        float sPeakL = 0.0f, sPeakR = 0.0f;
        float sumSqL = 0.0f, sumSqR = 0.0f;

        // Loop state (PRD-0014) – read atomics once for cache friendliness
        bool  loopAct = audioState->loopActive.load (std::memory_order_relaxed);
        int64_t lpIn  = audioState->loopInSamples.load (std::memory_order_relaxed);
        int64_t lpOut = audioState->loopOutSamples.load (std::memory_order_relaxed);
        bool validLoop = loopAct && lpIn >= 0 && lpOut > lpIn;
        int  loopRampLen = DeckAudioSource::FADE_RAMP_LENGTH;
        if (validLoop)
        {
            int64_t loopLen = lpOut - lpIn;
            if (loopLen < 256)
                loopRampLen = std::max (static_cast<int> (loopLen / 2), 1);
        }

        // PRD-0017: Detect loop exit → trigger slip snap-back
        bool loopJustExited = source->wasLoopActiveLastBlock && ! loopAct;
        source->wasLoopActiveLastBlock = loopAct;

        if (loopJustExited && slipEnabled && source->slipDisplacedLocal
            && ! source->isFadingOut && source->fadeRampSamplesRemaining <= 0
            && source->deferredAction == DeckAudioSource::DeferredAction::None)
        {
            auto shadowPos = static_cast<int64_t> (source->shadowPlayheadAccumulator);
            auto primaryPos = static_cast<int64_t> (source->playheadAccumulator);

            if (std::abs (shadowPos - primaryPos) < DeckAudioSource::FADE_RAMP_LENGTH)
            {
                // Close enough: no audible transition
                source->playheadAccumulator = source->shadowPlayheadAccumulator;
                source->slipDisplacedLocal = false;
            }
            else if (status == PlaybackStatusCode::playing)
            {
                source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                source->isFadingOut = true;
                source->isFadingIn  = false;
                source->deferredAction = DeckAudioSource::DeferredAction::SlipReturn;
            }
        }

        // Check key lock state
        bool keyLockOn = audioState->keyLockEnabled.load (std::memory_order_relaxed);

        // PRD-0025: key stepper transposition in semitones (range −12..+12).
        // The stretcher's pitch shift is computed as pow(2, semitones/12).
        int keyShiftSemis = audioState->keyShiftSemitones.load (std::memory_order_relaxed);
        const double pitchScale = (keyShiftSemis == 0)
            ? 1.0
            : std::pow (2.0, static_cast<double> (keyShiftSemis) / 12.0);

        // The stretcher is "engaged" (its output replaces the vinyl path)
        // whenever key lock is on OR a non-zero key shift is requested.
        // The stretcher is always FED below if it exists so that toggling
        // engaged/disengaged is gap-free.
        const bool stretcherEngaged = keyLockOn || (keyShiftSemis != 0);

        auto* stretcher = source->timeStretcher.load (std::memory_order_acquire);
        bool useStretcher = ! scratchMode && stretcherEngaged
                    && stretcher != nullptr && speed > 0.01f;

        // Detect engagement transition — apply a short crossfade between
        // vinyl and stretched paths to avoid a click at the transition.
        // The stretcher runs continuously (always fed) so its output is
        // valid at all times, making the crossfade seamless.
        if (stretcherEngaged != source->wasKeyLockEnabled)
        {
            source->wasKeyLockEnabled = stretcherEngaged;
            source->keyLockFadeSamplesRemaining = DeckAudioSource::KEY_LOCK_FADE_LENGTH;
            source->keyLockFadingIn  = stretcherEngaged;   // TO stretched
            source->keyLockFadingOut = ! stretcherEngaged; // FROM stretched

            // PRD-0011: Eliminate first-movement crackle.
            //
            // When the stretcher becomes engaged, snap smoothedTimeRatio to
            // the current target instantly so the very first slider movement
            // ramps from the correct baseline (1.0 / current speed) rather
            // than from a stale 1.0.  The KEY_LOCK_FADE crossfade hides any
            // discontinuity at the toggle itself.
            if (stretcherEngaged && speed > 0.01f)
                source->smoothedTimeRatio = 1.0 / static_cast<double> (speed);
        }

        // --- Time-stretched path ---
        // Always feed the stretcher when it exists so its internal buffers
        // stay warm.  This prevents a cold-start click when key lock is
        // toggled on — the stretched output is already valid for the
        // crossfade.  At ratio 1.0 the R3 engine is near-passthrough and
        // adds negligible CPU.
        int stretchedAvail = 0;
        if (! scratchMode && stretcher != nullptr && speed > 0.01f)
        {
            // Smoothly ramp the time ratio toward the target so that
            // setTimeRatio() never sees an abrupt jump — this prevents the
            // R3 engine's pipeline from destabilising (crackling) when the
            // pitch fader is moved while key lock is active.
            double targetTimeRatio = 1.0 / static_cast<double> (speed);

            // PRD-0011: Keep R3 permanently in "active stretching" mode by
            // never letting the target ratio settle at exactly 1.0.  R3 uses
            // a different numerical path at ratio == 1.0 than at ratio != 1.0;
            // the transition between those paths is what produces the
            // first-movement crackle.  A bias of 1e-4 corresponds to ~0.002
            // cent — three orders of magnitude below the threshold of human
            // pitch perception — yet keeps R3 in active mode at all times.
            constexpr double activeBias = 1.0e-4;
            if (std::abs (targetTimeRatio - 1.0) < activeBias)
                targetTimeRatio = 1.0 + activeBias;

            double& sRatio = source->smoothedTimeRatio;
            double delta = targetTimeRatio - sRatio;
            double maxD  = DeckAudioSource::STRETCH_RATIO_MAX_DELTA;
            if (delta > maxD)       sRatio += maxD;
            else if (delta < -maxD) sRatio -= maxD;
            else                    sRatio  = targetTimeRatio;

            // Convert the desired (fractional) source sample count to an
            // integer block size without long-term bias. This avoids
            // overfeeding the stretcher and building delayed queued output.
            const double exactSourceSamples =
                (static_cast<double> (numSamples) / sRatio)
                + source->stretchInputSampleCarry;

            int sourceSamples = 1;
            if (exactSourceSamples <= 1.0)
            {
                sourceSamples = 1;
                source->stretchInputSampleCarry = 0.0;
            }
            else if (exactSourceSamples >= static_cast<double> (DeckAudioSource::MAX_STRETCH_BLOCK))
            {
                sourceSamples = DeckAudioSource::MAX_STRETCH_BLOCK;
                source->stretchInputSampleCarry = 0.0;
            }
            else
            {
                sourceSamples = static_cast<int> (std::floor (exactSourceSamples));
                source->stretchInputSampleCarry =
                    exactSourceSamples - static_cast<double> (sourceSamples);
            }

            double readPos = source->playheadAccumulator
                           + static_cast<double> (source->stretcherLatency);
            for (int s = 0; s < sourceSamples; ++s)
            {
                double sPos = readPos + static_cast<double> (s) * 1.0;
                int64_t sIdx = static_cast<int64_t> (sPos);
                if (sIdx >= 0 && sIdx < bufLen)
                {
                    float frac = static_cast<float> (sPos - static_cast<double> (sIdx));
                    float s0L = chL[sIdx];
                    float s1L = (sIdx + 1 < bufLen) ? chL[sIdx + 1] : chL[sIdx];
                    source->stretchInL[s] = s0L + frac * (s1L - s0L);

                    float s0R = chR[sIdx];
                    float s1R = (sIdx + 1 < bufLen) ? chR[sIdx + 1] : chR[sIdx];
                    source->stretchInR[s] = s0R + frac * (s1R - s0R);
                }
                else
                {
                    source->stretchInL[s] = 0.0f;
                    source->stretchInR[s] = 0.0f;
                }
            }

            const float* inPtrs[2] = { source->stretchInL, source->stretchInR };
            float* outPtrs[2] = { source->stretchOutL, source->stretchOutR };

            // PRD-0025: apply key stepper pitch shift (semitone-based).
            // setPitchScale is a no-op when the value hasn't changed.
            stretcher->setPitchScale (pitchScale);

            stretchedAvail = stretcher->process (
                inPtrs, sourceSamples, outPtrs, numSamples, sRatio);
        }

        // --- Stem state (PRD-0021) ---
        bool stemActive = source->stemsActive.load (std::memory_order_acquire);

        const float* stmL[DeckAudioSource::NUM_STEMS] = {};
        const float* stmR[DeckAudioSource::NUM_STEMS] = {};
        int64_t stmLen[DeckAudioSource::NUM_STEMS] = {};
        bool stmMuted[DeckAudioSource::NUM_STEMS] = {};

        // Need stem data if active or during deactivation crossfade
        bool computeStems = stemActive;
        if (! stemActive && source->stemsActivationFadeRemaining > 0)
            computeStems = true;

        if (computeStems)
        {
            for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
            {
                stmL[s]   = source->stemChannelL[s].load (std::memory_order_acquire);
                stmR[s]   = source->stemChannelR[s].load (std::memory_order_relaxed);
                stmLen[s] = source->stemBufferNumFrames[s].load (std::memory_order_relaxed);
            }
            stmMuted[0] = audioState->stemVocalsMuted.load (std::memory_order_relaxed);
            stmMuted[1] = audioState->stemDrumsMuted.load (std::memory_order_relaxed);
            stmMuted[2] = audioState->stemBassMuted.load (std::memory_order_relaxed);
            stmMuted[3] = audioState->stemOtherMuted.load (std::memory_order_relaxed);
        }

        // --- Per-stem time stretching (PRD-0022) ---
        bool useStemStretchers = false;
        int stemStretchedAvail[DeckAudioSource::NUM_STEMS] = {};

        if (! scratchMode && stemActive && stretcherEngaged && speed > 0.01f
            && ! source->stemStretchDegraded.load (std::memory_order_relaxed))
        {
            auto* stemStr0 = source->stemTimeStretchers[0].load(std::memory_order_acquire);
            if (stemStr0 != nullptr)
            {
                useStemStretchers = true;

                double timeRatio = 1.0 / static_cast<double>(speed);

                const double exactStemSourceSamples =
                    (static_cast<double> (numSamples) / timeRatio)
                    + source->stemStretchInputSampleCarry;

                int sourceSamples = 1;
                if (exactStemSourceSamples <= 1.0)
                {
                    sourceSamples = 1;
                    source->stemStretchInputSampleCarry = 0.0;
                }
                else if (exactStemSourceSamples >= static_cast<double> (DeckAudioSource::MAX_STRETCH_BLOCK))
                {
                    sourceSamples = DeckAudioSource::MAX_STRETCH_BLOCK;
                    source->stemStretchInputSampleCarry = 0.0;
                }
                else
                {
                    sourceSamples = static_cast<int> (std::floor (exactStemSourceSamples));
                    source->stemStretchInputSampleCarry =
                        exactStemSourceSamples - static_cast<double> (sourceSamples);
                }

                for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
                {
                    auto* stemStretcher = source->stemTimeStretchers[s].load(std::memory_order_acquire);
                    if (stemStretcher == nullptr || stmL[s] == nullptr || stmR[s] == nullptr || stmLen[s] <= 0)
                    {
                        stemStretchedAvail[s] = 0;
                        continue;
                    }

                    // Read stem audio at speed, offset by stem stretcher latency
                    double readPos = source->playheadAccumulator
                                   + static_cast<double>(source->stemStretcherLatency);
                    for (int ss = 0; ss < sourceSamples; ++ss)
                    {
                        double sPos = readPos + static_cast<double>(ss) * 1.0;
                        int64_t sIdx = static_cast<int64_t>(sPos);
                        if (sIdx >= 0 && sIdx < stmLen[s])
                        {
                            float frac = static_cast<float>(sPos - static_cast<double>(sIdx));
                            float s0L = stmL[s][sIdx];
                            float s1L = (sIdx + 1 < stmLen[s]) ? stmL[s][sIdx + 1] : stmL[s][sIdx];
                            source->stemStretchInL[s][ss] = s0L + frac * (s1L - s0L);

                            float s0R = stmR[s][sIdx];
                            float s1R = (sIdx + 1 < stmLen[s]) ? stmR[s][sIdx + 1] : stmR[s][sIdx];
                            source->stemStretchInR[s][ss] = s0R + frac * (s1R - s0R);
                        }
                        else
                        {
                            source->stemStretchInL[s][ss] = 0.0f;
                            source->stemStretchInR[s][ss] = 0.0f;
                        }
                    }

                    const float* inPtrs[2] = { source->stemStretchInL[s], source->stemStretchInR[s] };
                    float* outPtrs[2] = { source->stemStretchOutL[s], source->stemStretchOutR[s] };

                    // PRD-0025: apply key stepper pitch shift to each stem stretcher.
                    stemStretcher->setPitchScale (pitchScale);

                    stemStretchedAvail[s] = stemStretcher->process(inPtrs, sourceSamples, outPtrs, numSamples, timeRatio);
                }
            }
        }

        // Detect stems activation transition for crossfade (AC#25)
        if (stemActive != source->wasStemsActiveLocal)
        {
            // On deactivation, check if stem pointers are still valid
            bool canCrossfade = true;
            if (! stemActive)
            {
                bool anyValid = false;
                for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
                {
                    if (stmL[s] != nullptr && stmR[s] != nullptr && stmLen[s] > 0)
                        anyValid = true;
                }
                canCrossfade = anyValid;
                if (anyValid)
                    computeStems = true;
            }

            if (canCrossfade)
            {
                source->stemsActivationFadeRemaining = DeckAudioSource::STEM_CROSSFADE_LENGTH;
                source->stemsActivationFadeDirection = stemActive;
            }
        }
        source->wasStemsActiveLocal = stemActive;

        // Detect per-stem mute transitions (AC#11)
        if (stemActive)
        {
            for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
            {
                if (stmMuted[s] != source->stemWasMuted[s])
                {
                    source->stemFadeRemaining[s] = DeckAudioSource::STEM_CROSSFADE_LENGTH;
                    source->stemFadeDirection[s] = ! stmMuted[s]; // true = unmuting
                    source->stemWasMuted[s] = stmMuted[s];
                }
            }
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // If not playing and no active fade remaining, skip
            if (! scratchMode
                && status != PlaybackStatusCode::playing
                && source->fadeRampSamplesRemaining <= 0)
                continue;

            double pos  = source->playheadAccumulator;
            int64_t idx = static_cast<int64_t> (pos);

            if (scratchMode)
            {
                const double maxPos = static_cast<double> (bufLen - 1);
                if (pos < 0.0)
                {
                    pos = 0.0;
                    source->playheadAccumulator = 0.0;
                    idx = 0;
                }
                else if (pos > maxPos)
                {
                    pos = maxPos;
                    source->playheadAccumulator = maxPos;
                    idx = bufLen - 1;
                }
            }

            // End-of-track check
            if (! scratchMode && pos >= static_cast<double> (bufLen))
            {
                if (! source->isFadingOut && source->fadeRampSamplesRemaining <= 0)
                {
                    // Start end-of-track fade-out, hold at last sample
                    source->fadeRampSamplesRemaining = DeckAudioSource::FADE_RAMP_LENGTH;
                    source->isFadingOut = true;
                    source->isFadingIn  = false;
                    source->deferredAction = DeckAudioSource::DeferredAction::EndOfTrack;
                    source->playheadAccumulator = static_cast<double> (bufLen - 1);
                    pos = source->playheadAccumulator;
                    idx = bufLen - 1;
                }
                else if (source->isFadingOut)
                {
                    // Hold at last sample during end-of-track fade
                    idx = bufLen - 1;
                }
                else
                {
                    continue;
                }
            }

            // Capture advance decision before potential fade completion
            bool shouldAdvance = scratchMode
                ? (std::abs (speed) > kScratchDeadband)
                : ((status == PlaybackStatusCode::playing) || source->isFadingOut);

            // Stationary scratch-hold should be silent, not a held DC sample.
            // Keep fade handling active if a transport fade is currently in flight.
            if (scratchMode && ! shouldAdvance
                && source->fadeRampSamplesRemaining <= 0)
            {
                deckScratchL[slot][static_cast<size_t> (i)] = 0.0f;
                deckScratchR[slot][static_cast<size_t> (i)] = 0.0f;
                continue;
            }

            // Read vinyl-mode sample (always needed for crossfade or non-stretched path)
            float vinylL, vinylR;
            if (idx >= 0 && idx < bufLen)
            {
                if (speed != 1.0f)
                {
                    float frac = static_cast<float> (pos - static_cast<double> (idx));
                    float s0L  = chL[idx];
                    float s1L  = (idx + 1 < bufLen) ? chL[idx + 1] : chL[idx];
                    vinylL = s0L + frac * (s1L - s0L);

                    float s0R = chR[idx];
                    float s1R = (idx + 1 < bufLen) ? chR[idx + 1] : chR[idx];
                    vinylR = s0R + frac * (s1R - s0R);
                }
                else
                {
                    vinylL = chL[idx];
                    vinylR = chR[idx];
                }
            }
            else
            {
                vinylL = 0.0f;
                vinylR = 0.0f;
            }

            // Select output sample: stems, stretched, or vinyl (PRD-0021)
            float rawL, rawR;

            // Compute per-stem gains with crossfade (AC#11)
            float stemGains[DeckAudioSource::NUM_STEMS] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float stemSumL = 0.0f, stemSumR = 0.0f;

            if (computeStems)
            {
                for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
                {
                    if (source->stemFadeRemaining[s] > 0)
                    {
                        float p = static_cast<float> (DeckAudioSource::STEM_CROSSFADE_LENGTH
                            - source->stemFadeRemaining[s])
                            / static_cast<float> (DeckAudioSource::STEM_CROSSFADE_LENGTH);
                        stemGains[s] = source->stemFadeDirection[s] ? p : (1.0f - p);
                        --source->stemFadeRemaining[s];
                    }
                    else
                    {
                        stemGains[s] = stmMuted[s] ? 0.0f : 1.0f;
                    }
                }

                // Read from each stem buffer at current playhead (AC#10, AC#23)
                for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
                {
                    if (stmL[s] == nullptr || stmR[s] == nullptr || stmLen[s] <= 0)
                        continue;

                    float sL, sR;

                    if (useStemStretchers && i < stemStretchedAvail[s])
                    {
                        // PRD-0022: Use per-stem stretched output
                        sL = source->stemStretchOutL[s][i];
                        sR = source->stemStretchOutR[s][i];
                    }
                    else if (idx >= 0 && idx < stmLen[s])
                    {
                        // Direct vinyl read (existing PRD-0021 code)
                        if (speed != 1.0f)
                        {
                            float frac = static_cast<float> (pos - static_cast<double> (idx));
                            float s0L = stmL[s][idx];
                            float s1L = (idx + 1 < stmLen[s]) ? stmL[s][idx + 1] : stmL[s][idx];
                            sL = s0L + frac * (s1L - s0L);

                            float s0R = stmR[s][idx];
                            float s1R = (idx + 1 < stmLen[s]) ? stmR[s][idx + 1] : stmR[s][idx];
                            sR = s0R + frac * (s1R - s0R);
                        }
                        else
                        {
                            sL = stmL[s][idx];
                            sR = stmR[s][idx];
                        }
                    }
                    else
                    {
                        sL = 0.0f;
                        sR = 0.0f;
                    }

                    stemSumL += sL * stemGains[s];
                    stemSumR += sR * stemGains[s];
                }
            }

            // Normal path output (stretched or vinyl)
            // Get stretched sample (stretcher runs continuously so output is always valid)
            float strL = (stretcher != nullptr && i < stretchedAvail) ? source->stretchOutL[i] : vinylL;
            float strR = (stretcher != nullptr && i < stretchedAvail) ? source->stretchOutR[i] : vinylR;

            float normalL, normalR;
            if (source->keyLockFadeSamplesRemaining > 0)
            {
                // Crossfade between vinyl and stretched paths during key lock toggle
                float p = static_cast<float> (DeckAudioSource::KEY_LOCK_FADE_LENGTH
                    - source->keyLockFadeSamplesRemaining)
                    / static_cast<float> (DeckAudioSource::KEY_LOCK_FADE_LENGTH);
                --source->keyLockFadeSamplesRemaining;

                if (source->keyLockFadingIn) // transitioning TO stretched
                {
                    normalL = vinylL * (1.0f - p) + strL * p;
                    normalR = vinylR * (1.0f - p) + strR * p;
                }
                else // transitioning FROM stretched
                {
                    normalL = strL * (1.0f - p) + vinylL * p;
                    normalR = strR * (1.0f - p) + vinylR * p;
                }

                if (source->keyLockFadeSamplesRemaining <= 0)
                {
                    source->keyLockFadingIn = false;
                    source->keyLockFadingOut = false;
                }
            }
            else if (useStretcher)
            {
                normalL = strL;
                normalR = strR;
            }
            else
            {
                normalL = vinylL;
                normalR = vinylR;
            }

            // Select final output based on stems state (AC#9, AC#25)
            if (stemActive && source->stemsActivationFadeRemaining <= 0)
            {
                // Fully in stem mode
                rawL = stemSumL;
                rawR = stemSumR;
            }
            else if (source->stemsActivationFadeRemaining > 0)
            {
                // Crossfade between normal and stem paths
                float p = static_cast<float> (DeckAudioSource::STEM_CROSSFADE_LENGTH
                    - source->stemsActivationFadeRemaining)
                    / static_cast<float> (DeckAudioSource::STEM_CROSSFADE_LENGTH);
                --source->stemsActivationFadeRemaining;

                if (source->stemsActivationFadeDirection) // activating stems
                {
                    rawL = normalL * (1.0f - p) + stemSumL * p;
                    rawR = normalR * (1.0f - p) + stemSumR * p;
                }
                else // deactivating stems
                {
                    rawL = stemSumL * (1.0f - p) + normalL * p;
                    rawR = stemSumR * (1.0f - p) + normalR * p;
                }
            }
            else
            {
                // Normal mode — zero overhead (AC#9)
                rawL = normalL;
                rawR = normalR;
            }

            // Loop crossfade blend (PRD-0014, stem-aware PRD-0021 AC#15)
            // After a loop wrap-back, we blend the "old continuation" audio
            // (from past loopOut) with the new audio (from loopIn) to
            // eliminate the click at the loop boundary.
            if (source->loopFadeRemaining > 0)
            {
                float progress = static_cast<float> (
                    source->loopCrossfadeLength - source->loopFadeRemaining)
                    / static_cast<float> (source->loopCrossfadeLength);

                int64_t oldIdx = static_cast<int64_t> (source->loopFadeReadPos);
                float oldL = 0.0f, oldR = 0.0f;

                if (stemActive && source->stemsActivationFadeRemaining <= 0)
                {
                    // Read old continuation from each stem buffer independently
                    for (int s = 0; s < DeckAudioSource::NUM_STEMS; ++s)
                    {
                        if (stmL[s] == nullptr || stmR[s] == nullptr || stmLen[s] <= 0)
                            continue;

                        float oL = 0.0f, oR = 0.0f;
                        if (oldIdx >= 0 && oldIdx < stmLen[s])
                        {
                            oL = stmL[s][oldIdx];
                            oR = stmR[s][oldIdx];
                        }
                        oldL += oL * stemGains[s];
                        oldR += oR * stemGains[s];
                    }
                }
                else
                {
                    if (oldIdx >= 0 && oldIdx < bufLen)
                    {
                        oldL = chL[oldIdx];
                        oldR = chR[oldIdx];
                    }
                }

                rawL = rawL * progress + oldL * (1.0f - progress);
                rawR = rawR * progress + oldR * (1.0f - progress);

                source->loopFadeReadPos += static_cast<double> (speed);
                --source->loopFadeRemaining;
            }

            // Apply fade ramp
            float fadeGain = 1.0f;
            if (source->fadeRampSamplesRemaining > 0)
            {
                float remaining = static_cast<float> (source->fadeRampSamplesRemaining);
                float length    = static_cast<float> (DeckAudioSource::FADE_RAMP_LENGTH);

                if (source->isFadingIn)
                    fadeGain = 1.0f - remaining / length;
                else if (source->isFadingOut)
                    fadeGain = remaining / length;

                --source->fadeRampSamplesRemaining;

                // Handle fade completion
                if (source->fadeRampSamplesRemaining <= 0)
                {
                    if (source->isFadingIn)
                    {
                        source->isFadingIn = false;
                    }
                    else if (source->isFadingOut)
                    {
                        source->isFadingOut = false;

                        switch (source->deferredAction)
                        {
                            case DeckAudioSource::DeferredAction::Pause:
                                audioState->playbackStatus.store (
                                    static_cast<int> (PlaybackStatusCode::paused),
                                    std::memory_order_relaxed);
                                status = PlaybackStatusCode::paused;
                                break;

                            case DeckAudioSource::DeferredAction::Stop:
                                audioState->playbackStatus.store (
                                    static_cast<int> (PlaybackStatusCode::stopped),
                                    std::memory_order_relaxed);
                                status = PlaybackStatusCode::stopped;
                                source->playheadAccumulator = 0.0;
                                // PRD-0017: Stop resets both playheads
                                source->shadowPlayheadAccumulator = 0.0;
                                source->slipDisplacedLocal = false;
                                break;

                            case DeckAudioSource::DeferredAction::Seek:
                                // EPIC-0009: capture the exact jump (cue / beat /
                                // hot-cue / navigation seek) for the recording
                                // projection — clip closes at the pre-jump sample,
                                // the next opens at the target — published before
                                // the accumulator moves.
                                publishSeekDiscontinuity (*audioState,
                                    static_cast<int64_t> (source->playheadAccumulator),
                                    source->deferredSeekTarget);
                                source->playheadAccumulator =
                                    static_cast<double> (source->deferredSeekTarget);
                                // PRD-0017: handle slip displacement on seek
                                if (source->deferredIsSlipDisplacement)
                                {
                                    source->slipDisplacedLocal = true;
                                    source->deferredIsSlipDisplacement = false;
                                }
                                else
                                {
                                    // Navigation seek: reset shadow to target
                                    source->shadowPlayheadAccumulator =
                                        source->playheadAccumulator;
                                    source->slipDisplacedLocal = false;
                                }
                                source->fadeRampSamplesRemaining =
                                    DeckAudioSource::FADE_RAMP_LENGTH;
                                source->isFadingIn = true;
                                break;

                            case DeckAudioSource::DeferredAction::CueReturn:
                            {
                                auto cuePos = audioState->tempCuePosition.load (
                                    std::memory_order_relaxed);
                                if (cuePos >= 0)
                                    source->playheadAccumulator =
                                        static_cast<double> (cuePos);
                                // PRD-0017: CUE return resets both playheads
                                source->shadowPlayheadAccumulator =
                                    source->playheadAccumulator;
                                source->slipDisplacedLocal = false;
                                audioState->playbackStatus.store (
                                    static_cast<int> (PlaybackStatusCode::paused),
                                    std::memory_order_relaxed);
                                status = PlaybackStatusCode::paused;
                                break;
                            }

                            case DeckAudioSource::DeferredAction::EndOfTrack:
                                audioState->playbackStatus.store (
                                    static_cast<int> (PlaybackStatusCode::stopped),
                                    std::memory_order_relaxed);
                                status = PlaybackStatusCode::stopped;
                                // PRD-0017: end of track resets displacement
                                source->slipDisplacedLocal = false;
                                break;

                            case DeckAudioSource::DeferredAction::SlipReturn:
                                // PRD-0017: snap back to shadow position
                                source->playheadAccumulator =
                                    source->shadowPlayheadAccumulator;
                                source->slipDisplacedLocal = false;
                                source->fadeRampSamplesRemaining =
                                    DeckAudioSource::FADE_RAMP_LENGTH;
                                source->isFadingIn = true;
                                break;

                            case DeckAudioSource::DeferredAction::None:
                            default:
                                break;
                        }
                        source->deferredAction = DeckAudioSource::DeferredAction::None;
                    }
                }
            }

            // Pre-fader metering
            float absL = std::abs (rawL);
            float absR = std::abs (rawR);
            if (absL > sPeakL) sPeakL = absL;
            if (absR > sPeakR) sPeakR = absR;
            sumSqL += rawL * rawL;
            sumSqR += rawR * rawR;

            // PRD-0053: Write into per-deck scratch buffer instead of directly
            // into the output bus. The mixer pipeline stages accumulate these
            // buffers into the output after all decks have been processed.
            // PRD-0054: gain removed from here; ChannelStripProcessor applies it
            // with per-sample smoothing (7 ms ramp) to prevent zipper noise.
            deckScratchL[slot][static_cast<size_t>(i)] = rawL * fadeGain;
            deckScratchR[slot][static_cast<size_t>(i)] = rawR * fadeGain;

            // Advance playhead
            // In stretched mode, we still advance per-sample by speed (same as vinyl).
            // The stretcher handles the pitch correction internally.
            if (shouldAdvance
                && source->deferredAction != DeckAudioSource::DeferredAction::EndOfTrack)
                source->playheadAccumulator += static_cast<double> (speed);

            // Loop wrap-back (PRD-0014)
            // When the playhead crosses loopOut, wrap back to loopIn and
            // start a crossfade to eliminate the boundary click.
            // Skip during fade-out to avoid conflicting with transport seeks.
            if (! scratchMode && validLoop && shouldAdvance && ! source->isFadingOut
                && source->playheadAccumulator >= static_cast<double> (lpOut))
            {
                source->loopFadeReadPos    = source->playheadAccumulator;
                source->loopCrossfadeLength = loopRampLen;

                int64_t loopLen = lpOut - lpIn;
                double offset = std::fmod (
                    source->playheadAccumulator - static_cast<double> (lpIn),
                    static_cast<double> (loopLen));
                if (offset < 0.0)
                    offset += static_cast<double> (loopLen);
                source->playheadAccumulator = static_cast<double> (lpIn) + offset;

                source->loopFadeRemaining = loopRampLen;

                // EPIC-0009: publish the EXACT loop boundary so the recording
                // projection closes the clip at lpOut and resumes at lpIn — no
                // polled audio loss, a contiguous seam (the deck crossfades it).
                publishSeekDiscontinuity (*audioState, lpOut, lpIn);

                // PRD-0017: Loop wrap-around sets slip displacement
                if (slipEnabled && ! source->slipDisplacedLocal)
                    source->slipDisplacedLocal = true;
            }

            // PRD-0017: Shadow playhead management
            if (! scratchMode && slipEnabled)
            {
                if (source->slipDisplacedLocal)
                {
                    // Shadow advances independently (no loop wrap)
                    if (shouldAdvance
                        && source->deferredAction != DeckAudioSource::DeferredAction::EndOfTrack)
                    {
                        source->shadowPlayheadAccumulator += static_cast<double> (speed);
                        // Clamp to track bounds
                        if (source->shadowPlayheadAccumulator >= static_cast<double> (bufLen))
                            source->shadowPlayheadAccumulator = static_cast<double> (bufLen - 1);
                        if (source->shadowPlayheadAccumulator < 0.0)
                            source->shadowPlayheadAccumulator = 0.0;
                    }
                }
                else
                {
                    // Not displaced: shadow tracks primary
                    source->shadowPlayheadAccumulator = source->playheadAccumulator;
                }
            }
            else if (scratchMode)
            {
                source->shadowPlayheadAccumulator = source->playheadAccumulator;
                source->slipDisplacedLocal = false;
            }
        }

        // 6. Update atomic playhead
        audioState->playheadPosition.store (
            static_cast<int64_t> (source->playheadAccumulator),
            std::memory_order_relaxed);

        // PRD-0017: Publish slip state to UI
        audioState->slipShadowPosition.store (source->shadowPlayheadAccumulator,
                                              std::memory_order_relaxed);
        audioState->slipDisplaced.store (source->slipDisplacedLocal,
                                        std::memory_order_relaxed);

        float invN = 1.0f / static_cast<float> (numSamples);
        source->peakL.store (sPeakL,                    std::memory_order_relaxed);
        source->peakR.store (sPeakR,                    std::memory_order_relaxed);
        source->rmsL.store  (std::sqrt (sumSqL * invN), std::memory_order_relaxed);
        source->rmsR.store  (std::sqrt (sumSqR * invN), std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // PRD-0053 + PRD-0057: Mixer pipeline
    //   1. Clear A/B bus accumulators.
    //   2. Snapshot per-block mixer parameters from PRD-0052 atomics.
    //   3. For each active channel: ChannelStripProcessor → ABBus (PRD-0057
    //      honours assignA/assignB; channels with both flags false are silent).
    //   4. CrossfaderStage: per-sample equal-power (smooth) / hard-cut (sharp)
    //      curve law with a 7 ms one-pole position smoother (PRD-0057 §1.5.6).
    //   5. MasterStage: copy masterScratch → output bus.
    //   6. Hard clip (unchanged from PRD-0002, runs AFTER MasterStage).
    //
    // §1.5.5: Inactive channels (no deck / no buffer) are skipped entirely;
    // their absence produces silence in the bus (not zero-summed, simply absent).
    // The channelActive check uses the same state already probed in the deck loop
    // above — we re-probe it here once per channel, never per-sample.
    // -------------------------------------------------------------------------

    juce::FloatVectorOperations::clear (busAL.data(), numSamples);
    juce::FloatVectorOperations::clear (busAR.data(), numSamples);
    juce::FloatVectorOperations::clear (busBL.data(), numSamples);
    juce::FloatVectorOperations::clear (busBR.data(), numSamples);

    // Snapshot block-level mixer parameters once (lock-free, no allocation).
    auto* const mxSnapshot = mixerAtomicSnapshot.load (std::memory_order_acquire);

    CrossfaderSnapshot xfSnap;
    MasterSnapshot     masterSnap;
    if (mxSnapshot != nullptr)
    {
        xfSnap.crossfader     = mxSnapshot->crossfader.load (std::memory_order_relaxed);
        // PRD-0057: translate the int-mirrored enum back into the strongly-
        // typed CrossfaderCurve. Values outside the known range fall back
        // to Smooth (the documented default).
        const int curveEnc = mxSnapshot->crossfaderCurve.load (std::memory_order_relaxed);
        xfSnap.curve = (curveEnc == 1) ? CrossfaderCurve::Sharp
                                       : CrossfaderCurve::Smooth;
        masterSnap.masterGain = mxSnapshot->masterGain.load (std::memory_order_relaxed);
    }
    else
    {
        // PRD-0057 §1.5.5 / pre-PRD-0053 identity fallback: when no mixer
        // atomic snapshot has been installed (test harnesses, early start-up),
        // the crossfader must behave as an identity passthrough so the audio
        // engine remains testable in isolation. We force the internal Linear
        // curve at the centred position; combined with the unconditional
        // dual-assign below, this yields master = (busA + busB) * 0.5 = channel.
        xfSnap.crossfader = 0.5f;
        xfSnap.curve      = CrossfaderCurve::Linear;
    }

    for (size_t slot = 0; slot < 4; ++slot)
    {
        // §1.5.5: re-probe active state (same logic as the deck loop).
        auto* src = deckSlots[slot].load (std::memory_order_acquire);
        if (src == nullptr) continue;
        if (src->audioState == nullptr) continue;
        const bool hasBuffer =
            (src->channelL.load (std::memory_order_acquire) != nullptr)
            && (src->channelR.load (std::memory_order_acquire) != nullptr)
            && (src->bufferNumFrames.load (std::memory_order_relaxed) > 0);
        if (! hasBuffer) continue;

        // Populate per-channel snapshot from mixer atomics.
        ChannelStripSnapshot chSnap;
        if (mxSnapshot != nullptr)
        {
            const auto& atomCh = mxSnapshot->channels[slot];
            chSnap.gain     = atomCh.gain.load     (std::memory_order_relaxed);
            chSnap.eqHigh   = atomCh.eqHigh.load   (std::memory_order_relaxed);
            chSnap.eqMid    = atomCh.eqMid.load    (std::memory_order_relaxed);
            chSnap.eqLow    = atomCh.eqLow.load    (std::memory_order_relaxed);
            chSnap.filter   = atomCh.filter.load   (std::memory_order_relaxed);
            chSnap.fader    = atomCh.fader.load    (std::memory_order_relaxed);
            chSnap.killHigh = atomCh.killHigh.load (std::memory_order_relaxed);
            chSnap.killMid  = atomCh.killMid.load  (std::memory_order_relaxed);
            chSnap.killLow  = atomCh.killLow.load  (std::memory_order_relaxed);
            chSnap.assignA  = atomCh.assignA.load  (std::memory_order_relaxed);
            chSnap.assignB  = atomCh.assignB.load  (std::memory_order_relaxed);
        }
        else
        {
            // PRD-0057 identity fallback (see crossfader fallback above):
            // route every active channel to both buses so the test harness
            // pipeline matches the pre-PRD-0053 direct-sum reference.
            chSnap.assignA = true;
            chSnap.assignB = true;
        }

        // PRD-0053 pass-through strip: copies deckScratch → channelScratch.
        channelStrips[slot].process (
            deckScratchL[slot].data(), deckScratchR[slot].data(),
            channelScratchL[slot].data(), channelScratchR[slot].data(),
            numSamples, chSnap);

        // PRD-0057 §1.5.1: route the per-channel signal into bus A and/or
        // bus B according to the snapshot's assign flags.
        ABBus::accumulate (
            channelScratchL[slot].data(), channelScratchR[slot].data(),
            chSnap.assignA, chSnap.assignB,
            busAL.data(), busAR.data(),
            busBL.data(), busBR.data(),
            numSamples);
    }

    // PRD-0057: per-sample (gainA * busA + gainB * busB) with smoothed
    // position and the curve selected by xfSnap.curve.
    crossfaderStage.process (
        busAL.data(), busAR.data(),
        busBL.data(), busBR.data(),
        masterScratchL.data(), masterScratchR.data(),
        numSamples, xfSnap);

    // Identity copy to output bus (PRD-0053 pass-through).
    masterStage.process (
        masterScratchL.data(), masterScratchR.data(),
        outL, outR,
        numSamples, masterSnap);

    // PRD-0081/0082: DAW timeline renderer — mix arrangement clips into the master output.
    // advancePlayhead is called first (audio-thread safe atomic update); then
    // renderBlock accumulates into a stereo feed buffer which is added to the output.
    {
        auto* dTransport = dawTransport_.load (std::memory_order_acquire);
        if (dTransport != nullptr)
            dTransport->advancePlayhead (numSamples);

        // Only pull arrangement audio while the transport is actually Playing.
        // When Paused the playhead is frozen at a positive sample (not the -1
        // Stopped sentinel), so renderBlock would otherwise keep advancing each
        // streamer's read cursor and emit sound with a stationary playhead.
        // Gating here makes Pause silence the arrangement just like Stop.
        auto* dRenderer = dawRendererPtr_.load (std::memory_order_acquire);
        if (dRenderer != nullptr
            && dTransport != nullptr
            && dTransport->isPlaying()
            && static_cast<int>(dawMasterFeedL_.size()) >= numSamples
            && static_cast<int>(dawMasterFeedR_.size()) >= numSamples)
        {
            juce::FloatVectorOperations::clear (dawMasterFeedL_.data(), numSamples);
            juce::FloatVectorOperations::clear (dawMasterFeedR_.data(), numSamples);

            // Build a two-channel AudioBuffer pointing at our pre-allocated scratch.
            float* chPtrs[2] = { dawMasterFeedL_.data(), dawMasterFeedR_.data() };
            juce::AudioBuffer<float> dawFeed (chPtrs, 2, numSamples);

            // EPIC-0011: hand the renderer the per-channel mixer atomics so each
            // channel group's playback is processed by its gain/EQ/filter DSP —
            // the path recorded automation (PRD-0092) is replayed through. mxSnapshot
            // was loaded once above; may be null (test harness) → renderer falls
            // back to a straight master sum (legacy behaviour).
            dRenderer->renderBlock (dawFeed, numSamples, mxSnapshot);

            // Accumulate into the already-written output bus.
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = juce::jlimit (-1.0f, 1.0f, outL[i] + dawMasterFeedL_[i]);
                outR[i] = juce::jlimit (-1.0f, 1.0f, outR[i] + dawMasterFeedR_[i]);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Metronome (testing aid). An audible click locked to the master grid,
    // summed into the live output here. It is NOT captured by the DAW recording
    // (a reconstruction from the source files via LiveProjectionTimer) nor by an
    // offline export (which re-renders clips), so it is audible yet never
    // recorded. Two clock sources, in priority order:
    //   1. DAW transport playing  -> tick the published runtime-domain grid.
    //   2. otherwise master clock  -> tick the master deck's native beat grid
    //      (live DJing / recording), straight off the lock-free publisher.
    // -----------------------------------------------------------------------
    if (metronomeEnabled_.load (std::memory_order_acquire))
    {
        const double sr = currentSampleRate.load (std::memory_order_relaxed);
        int beatsPerBar = metroBeatsPerBar_.load (std::memory_order_relaxed);
        if (beatsPerBar < 1)
            beatsPerBar = 4;

        int     clockSource = 0;   // 0 none, 1 DAW transport, 2 master clock
        int64_t origin      = 0;
        double  beatLen     = 0.0;
        int64_t refPos      = 0;   // block-start timeline position in that domain

        auto* dT = dawTransport_.load (std::memory_order_acquire);
        if (dT != nullptr && dT->isPlaying())
        {
            beatLen = metroBeatLenRuntime_.load (std::memory_order_relaxed);
            origin  = metroPhaseOriginRuntime_.load (std::memory_order_relaxed);
            refPos  = dT->getPlayheadSample();         // post-advance: == renderBlock's block start
            if (beatLen > 0.0 && refPos >= 0)
                clockSource = 1;
        }
        else if (cachedClockPublisher_ != nullptr && sr > 0.0)
        {
            const auto snap = cachedClockPublisher_->read();
            if (snap.masterIsPlaying && snap.masterNativeBPM > 0.0)
            {
                beatLen = (sr * 60.0) / snap.masterNativeBPM;            // source-domain samples/beat
                origin  = snap.masterPhaseOriginSample;
                refPos  = cachedClockPublisher_->masterPlayheadSample.load (std::memory_order_relaxed);
                if (beatLen > 0.0)
                    clockSource = 2;
            }
        }

        int clickStartInBlock = 0; // sample offset where a click fired THIS block begins

        if (clockSource != metroPrevClockSource_)
        {
            // Clock source changed (transport start/stop, master promotion).
            // Re-baseline so we neither fire on the transition nor dump a burst
            // of "missed" beats.
            metroPrevClockSource_ = clockSource;
            metroLastBeatIndex_   = (clockSource != 0)
                ? static_cast<int64_t> (std::floor (static_cast<double> (refPos - origin) / beatLen))
                : std::numeric_limits<int64_t>::min();
        }
        else if (clockSource != 0)
        {
            const int64_t startIdx =
                static_cast<int64_t> (std::floor (static_cast<double> (refPos - origin) / beatLen));
            if (metroLastBeatIndex_ == std::numeric_limits<int64_t>::min())
                metroLastBeatIndex_ = startIdx;

            const int64_t targetIdx = metroLastBeatIndex_ + 1;
            const double  targetPos = static_cast<double> (origin)
                                    + static_cast<double> (targetIdx) * beatLen;
            const int     offset    = static_cast<int> (
                std::llround (targetPos - static_cast<double> (refPos)));

            if (offset < 0)
            {
                // Playhead jumped past one or more beats (seek/loop wrap): resync
                // without firing a click.
                metroLastBeatIndex_ = startIdx;
            }
            else if (offset < numSamples)
            {
                // A beat boundary lands inside this block: fire a click at it.
                metroLastBeatIndex_         = targetIdx;
                metroClickSamplesRemaining_ = static_cast<int> (metroClickHi_.size());
                clickStartInBlock           = offset;
                metroClickIsDownbeat_ =
                    (((targetIdx % beatsPerBar) + beatsPerBar) % beatsPerBar) == 0;
            }
            // else: the next beat is in a future block — nothing to do this block.
        }

        // Emit any in-flight click (it may span several blocks; only the block it
        // fires in starts at clickStartInBlock, later blocks start at 0).
        if (metroClickSamplesRemaining_ > 0 && ! metroClickHi_.empty() && ! metroClickLo_.empty())
        {
            const std::vector<float>& wave = metroClickIsDownbeat_ ? metroClickHi_ : metroClickLo_;
            const int total = static_cast<int> (wave.size());
            for (int i = clickStartInBlock; i < numSamples && metroClickSamplesRemaining_ > 0; ++i)
            {
                const float s = wave[static_cast<size_t> (total - metroClickSamplesRemaining_)];
                outL[i] = juce::jlimit (-1.0f, 1.0f, outL[i] + s);
                outR[i] = juce::jlimit (-1.0f, 1.0f, outR[i] + s);
                --metroClickSamplesRemaining_;
            }
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = juce::jlimit (-1.0f, 1.0f, outL[i]);
        outR[i] = juce::jlimit (-1.0f, 1.0f, outR[i]);
    }

    // CPU load measurement.
    const auto endTicks = juce::Time::getHighResolutionTicks();
    double elapsed  = juce::Time::highResolutionTicksToSeconds (endTicks - startTicks);
    double available = static_cast<double> (numSamples)
                     / currentSampleRate.load (std::memory_order_relaxed);
    float load = (available > 0.0) ? static_cast<float> (elapsed / available) : 0.0f;

    cpuMonitor.loadPercent.store (load * 100.0f, std::memory_order_relaxed);

    if (load > 0.9f)
    {
        ++cpuMonitor.consecutiveOverloads;
        if (cpuMonitor.consecutiveOverloads >= 3)
        {
            cpuMonitor.overloadWarning.store (true, std::memory_order_relaxed);

            // PRD-0022: Degrade to single-stretcher when CPU overloaded
            for (size_t slot = 0; slot < 4; ++slot)
            {
                auto* src = deckSlots[slot].load(std::memory_order_relaxed);
                if (src != nullptr && src->stemTimeStretchers[0].load(std::memory_order_relaxed) != nullptr)
                    src->stemStretchDegraded.store(true, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        cpuMonitor.consecutiveOverloads = 0;
        cpuMonitor.overloadWarning.store (false, std::memory_order_relaxed);
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        currentSampleRate.store (device->getCurrentSampleRate(), std::memory_order_relaxed);
        currentBufferSize.store (device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    }

    // PRD-0053: Pre-allocate all mixer pipeline scratch buffers on the message
    // thread before the audio callback resumes. No allocation ever occurs on
    // the audio thread (CLAUDE.md immutable rule).
    const int blockSize   = currentBufferSize.load (std::memory_order_relaxed);
    const double sr       = currentSampleRate.load (std::memory_order_relaxed);
    constexpr int numCh   = 2;

    for (int ch = 0; ch < 4; ++ch)
    {
        deckScratchL[static_cast<size_t>(ch)].assign (static_cast<size_t>(blockSize), 0.0f);
        deckScratchR[static_cast<size_t>(ch)].assign (static_cast<size_t>(blockSize), 0.0f);
        channelScratchL[static_cast<size_t>(ch)].assign (static_cast<size_t>(blockSize), 0.0f);
        channelScratchR[static_cast<size_t>(ch)].assign (static_cast<size_t>(blockSize), 0.0f);
        channelStrips[static_cast<size_t>(ch)].prepareToPlay (sr, blockSize, numCh);
    }

    busAL.assign (static_cast<size_t>(blockSize), 0.0f);
    busAR.assign (static_cast<size_t>(blockSize), 0.0f);
    busBL.assign (static_cast<size_t>(blockSize), 0.0f);
    busBR.assign (static_cast<size_t>(blockSize), 0.0f);
    masterScratchL.assign (static_cast<size_t>(blockSize), 0.0f);
    masterScratchR.assign (static_cast<size_t>(blockSize), 0.0f);

    crossfaderStage.prepareToPlay (sr, blockSize, numCh);
    masterStage.prepareToPlay     (sr, blockSize, numCh);

    // PRD-0081/0082: Prepare the DAW timeline renderer.
    dawMasterFeedL_.assign (static_cast<size_t>(blockSize), 0.0f);
    dawMasterFeedR_.assign (static_cast<size_t>(blockSize), 0.0f);
    rebuildDawRenderer();

    // Metronome (testing aid): pre-render the click waveforms at the device rate
    // on the message thread so the audio thread never allocates. A short
    // enveloped sine burst — brighter & louder on the downbeat, softer on the
    // off-beats — with a 1 ms attack ramp so the onset itself doesn't click.
    {
        const int clickLen = juce::jmax (1, static_cast<int> (0.045 * sr)); // ~45 ms
        metroClickHi_.assign (static_cast<size_t> (clickLen), 0.0f);
        metroClickLo_.assign (static_cast<size_t> (clickLen), 0.0f);

        const auto fillClick = [clickLen, sr] (std::vector<float>& buf,
                                               double freq, float amp)
        {
            for (int i = 0; i < clickLen; ++i)
            {
                const double t   = static_cast<double> (i) / sr;
                const double atk = juce::jmin (1.0, t / 0.001);          // 1 ms ramp-in
                const double env = std::exp (-t * 42.0);                  // fast decay
                buf[static_cast<size_t> (i)] = static_cast<float> (
                    amp * atk * env
                    * std::sin (2.0 * juce::MathConstants<double>::pi * freq * t));
            }
        };

        fillClick (metroClickHi_, 1567.98, 0.60f); // downbeat (G6-ish), accented
        fillClick (metroClickLo_, 1046.50, 0.40f); // beat (C6-ish), softer
    }
}
void AudioEngine::audioDeviceStopped()
{
    if (shuttingDown)
        return;

    // Device disconnected — mark error and begin reconnection attempts.
    deviceDisconnected = true;
    reconnectAttempts  = 0;
    timerTicksSinceLastReconnect = 0;

    // Pause all playing decks on the message thread.
    juce::MessageManager::callAsync ([this]()
    {
        for (size_t slot = 0; slot < 4; ++slot)
        {
            auto* source = deckSlots[slot].load (std::memory_order_acquire);
            if (source == nullptr || source->audioState == nullptr)
                continue;

            auto status = source->audioState->playbackStatus.load (std::memory_order_relaxed);
            if (status == static_cast<int> (PlaybackStatusCode::playing))
                source->audioState->playbackStatus.store (
                    static_cast<int> (PlaybackStatusCode::paused), std::memory_order_relaxed);
        }

        audioDeviceNode.setProperty (IDs::deviceError, "Audio device disconnected", nullptr);
    });
}

// ---------------------------------------------------------------------------
// ChangeListener
// ---------------------------------------------------------------------------

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        // A device-topology change just occurred (headphones plugged into the
        // jack, a USB interface connected, AirPods linked, etc.). JUCE binds us
        // to a *specific* device at start() and does NOT follow the system
        // default afterwards, so re-point the engine at the new default output
        // before refreshing the displayed device metadata.
        followDefaultOutputDevice();
        updateDeviceState();
    }
}

// ---------------------------------------------------------------------------
// Timer (message thread)
// ---------------------------------------------------------------------------

void AudioEngine::timerCallback()
{
    // Publish CPU load to ValueTree (message thread safe).
    audioDeviceNode.setProperty (IDs::cpuLoad,
        cpuMonitor.loadPercent.load (std::memory_order_relaxed), nullptr);
    audioDeviceNode.setProperty (IDs::cpuOverload,
        cpuMonitor.overloadWarning.load (std::memory_order_relaxed), nullptr);

    // PRD-0022: Check for stem stretcher creation/destruction needs
    for (size_t slot = 0; slot < 4; ++slot)
    {
        auto& src = deckSources[slot];
        if (src.audioState == nullptr)
            continue;

        bool keyLock = src.audioState->keyLockEnabled.load(std::memory_order_relaxed);
        bool stemsActive = src.stemsActive.load(std::memory_order_relaxed);
        bool hasStemStretchers = src.stemTimeStretchers[0].load(std::memory_order_relaxed) != nullptr;

        bool needStemStretchers = keyLock && stemsActive;

        if (needStemStretchers && !hasStemStretchers)
        {
            juce::String deckId;
            if (slot == 0) deckId = "A";
            else if (slot == 1) deckId = "B";
            else if (slot == 2) deckId = "C";
            else deckId = "D";
            createStemStretchers(deckId);
        }
        else if (!needStemStretchers && hasStemStretchers)
        {
            juce::String deckId;
            if (slot == 0) deckId = "A";
            else if (slot == 1) deckId = "B";
            else if (slot == 2) deckId = "C";
            else deckId = "D";
            destroyStemStretchers(deckId);
        }
    }

    // PRD-0022: Publish stem stretch degradation flag to ValueTree
    auto decksNode = rootState.getChildWithName(IDs::Decks);
    if (decksNode.isValid())
    {
        for (int i = 0; i < decksNode.getNumChildren(); ++i)
        {
            auto deckTree = decksNode.getChild(i);
            auto stems = deckTree.getChildWithName(IDs::Stems);
            if (stems.isValid())
            {
                int s = deckIdToSlot(deckTree.getProperty(IDs::id).toString());
                if (s >= 0)
                {
                    bool degraded = deckSources[static_cast<size_t>(s)].stemStretchDegraded.load(std::memory_order_relaxed);
                    if (static_cast<bool>(stems.getProperty(IDs::stemStretchDegraded, false)) != degraded)
                        stems.setProperty(IDs::stemStretchDegraded, degraded, nullptr);
                }
            }
        }
    }

    // Handle reconnection if device disconnected.
    if (deviceDisconnected)
    {
        ++timerTicksSinceLastReconnect;

        if (timerTicksSinceLastReconnect >= reconnectIntervalTicks)
        {
            timerTicksSinceLastReconnect = 0;
            attemptReconnection();
        }
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void AudioEngine::updateDeviceState()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        audioDeviceNode.setProperty (IDs::deviceName,
            device->getName(), nullptr);
        audioDeviceNode.setProperty (IDs::sampleRate,
            device->getCurrentSampleRate(), nullptr);
        audioDeviceNode.setProperty (IDs::bufferSize,
            device->getCurrentBufferSizeSamples(), nullptr);

        double latencyMs = (device->getOutputLatencyInSamples()
                           / device->getCurrentSampleRate()) * 1000.0;
        audioDeviceNode.setProperty (IDs::outputLatencyMs, latencyMs, nullptr);
        audioDeviceNode.setProperty (IDs::deviceError, "", nullptr);
    }
}

void AudioEngine::followDefaultOutputDevice()
{
    // Don't fight the shutdown or the disconnect/reconnect path — those own the
    // device lifecycle while they're active.
    if (shuttingDown || deviceDisconnected)
        return;

    auto* deviceType = deviceManager.getCurrentDeviceTypeObject();
    if (deviceType == nullptr)
        return;

    // JUCE has already refreshed this device type's list by the time the change
    // message reaches us, so getDefaultDeviceIndex reflects the *new* topology.
    const int defaultOutIndex = deviceType->getDefaultDeviceIndex (false /* isInput */);
    if (defaultOutIndex < 0)
        return;

    const auto outputNames = deviceType->getDeviceNames (false /* isInput */);
    if (! juce::isPositiveAndBelow (defaultOutIndex, outputNames.size()))
        return;

    const juce::String defaultOutName = outputNames[defaultOutIndex];
    if (defaultOutName.isEmpty())
        return;

    juce::String currentOutName;
    if (auto* current = deviceManager.getCurrentAudioDevice())
        currentOutName = current->getName();

    // Already on the system default — nothing to do. This is also the case when
    // the headphones share a single CoreAudio device with the speakers and only
    // the internal data source switched (CoreAudio reroutes that transparently),
    // so we correctly leave the working setup untouched.
    if (defaultOutName == currentOutName)
        return;

    // The system default output changed out from under us (e.g. headphones
    // plugged into the jack appeared as a new device). Re-point the engine at
    // it. setAudioDeviceSetup() re-broadcasts a change message, but on the next
    // pass defaultOutName == currentOutName, so this does not loop.
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = defaultOutName;
    setup.useDefaultOutputChannels = true;
    // Re-assert our preferred rate/buffer; JUCE falls back to the device's
    // nearest supported values when these aren't available.
    setup.sampleRate = 44100.0;
    setup.bufferSize = 128;

    const auto error = deviceManager.setAudioDeviceSetup (setup, true /* treatAsChosenDevice */);
    if (error.isNotEmpty())
        audioDeviceNode.setProperty (IDs::deviceError, error, nullptr);
}

void AudioEngine::attemptReconnection()
{
    if (reconnectAttempts >= maxReconnectAttempts)
    {
        audioDeviceNode.setProperty (IDs::deviceError,
            "Audio device reconnection failed after maximum attempts", nullptr);
        deviceDisconnected = false; // stop trying
        return;
    }

    ++reconnectAttempts;

    auto result = deviceManager.initialise (0, 2, nullptr, true, {}, nullptr);

    if (result.isEmpty())
    {
        // Successfully reconnected.
        if (deviceManager.getCurrentAudioDevice() != nullptr)
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.sampleRate = 44100.0;
            setup.bufferSize = 128;
            deviceManager.setAudioDeviceSetup (setup, true);
        }

        deviceManager.addAudioCallback (this);
        deviceDisconnected = false;
        reconnectAttempts  = 0;
        updateDeviceState();
    }
}

int AudioEngine::deckIdToSlot (const juce::String& deckId)
{
    if (deckId == "A") return 0;
    if (deckId == "B") return 1;
    if (deckId == "C") return 2;
    if (deckId == "D") return 3;
    return -1;
}

// ---------------------------------------------------------------------------
// Transport API (PRD-0004) — thread-safe, lock-free
// ---------------------------------------------------------------------------

void AudioEngine::sendTransportCommand (const juce::String& deckId, TransportCommand cmd)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
        source->pendingCommand.store (static_cast<int> (cmd), std::memory_order_relaxed);
}

void AudioEngine::seekDeck (const juce::String& deckId, int64_t targetSample)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
    {
        source->seekTarget.store (targetSample, std::memory_order_relaxed);
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::Seek), std::memory_order_relaxed);
    }
}

void AudioEngine::scratchSeekDeck (const juce::String& deckId, int64_t targetSample)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
    {
        source->seekTarget.store (targetSample, std::memory_order_relaxed);
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::ScratchSeek), std::memory_order_relaxed);
    }
}

void AudioEngine::seekAndPlayDeck (const juce::String& deckId, int64_t targetSample)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
    {
        source->seekTarget.store (targetSample, std::memory_order_relaxed);
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::SeekAndPlay), std::memory_order_relaxed);
    }
}

void AudioEngine::slipSeekDeck (const juce::String& deckId, int64_t targetSample)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
    {
        source->seekTarget.store (targetSample, std::memory_order_relaxed);
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::SlipSeek), std::memory_order_relaxed);
    }
}

void AudioEngine::slipSeekAndPlayDeck (const juce::String& deckId, int64_t targetSample)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
    {
        source->seekTarget.store (targetSample, std::memory_order_relaxed);
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::SlipSeekAndPlay), std::memory_order_relaxed);
    }
}

void AudioEngine::sendSlipReturn (const juce::String& deckId)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto* source = deckSlots[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    if (source != nullptr)
        source->pendingCommand.store (
            static_cast<int> (TransportCommand::SlipReturn), std::memory_order_relaxed);
}
