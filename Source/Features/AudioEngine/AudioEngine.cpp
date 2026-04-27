#include "AudioEngine.h"
#include "../Quantize/QuantizeService.h"
#include "../Sync/MasterClockPublisher.h"
#include "../Sync/SyncEngine.h"
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
    for (auto& src : deckSources)
        src.masterClockPublisher.store (publisher, std::memory_order_release);

    // Create or destroy per-deck sync engines (PRD-0027)
    for (int i = 0; i < 4; ++i)
        deckSyncEngines[static_cast<size_t> (i)] = publisher ? std::make_unique<SyncEngine> (*publisher) : nullptr;
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
            // PRD-0017: reset slip atomics
            src.audioState->slipShadowPosition.store (0.0, std::memory_order_relaxed);
            src.audioState->slipDisplaced.store (false, std::memory_order_relaxed);
        }

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
            const auto& buf = holder->getBuffer();
            newStretcher->primeWithAudio (
                buf.getReadPointer (0),
                buf.getReadPointer (1),
                holder->getNumFrames());

            src.stretcherLatency = newStretcher->getLatency();
            src.timeStretcherOwned = newStretcher;
            src.timeStretcher.store (newStretcher, std::memory_order_release);

            // Reset key lock crossfade state
            src.wasKeyLockEnabled = false;
            src.keyLockFadeSamplesRemaining = 0;
            src.keyLockFadingIn = false;
            src.keyLockFadingOut = false;
            src.smoothedTimeRatio = 1.0;

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
    }

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
    src.stemsActive.store (true, std::memory_order_release);

    // PRD-0022: Create stem stretchers if key lock is already active
    if (src.audioState != nullptr && src.audioState->keyLockEnabled.load(std::memory_order_relaxed))
        createStemStretchers(deckId);
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

    // Must have stems active and valid stem pointers
    if (!src.stemsActive.load (std::memory_order_relaxed))
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
            stretcher->primeWithAudio (stemL, stemR, static_cast<int>(stemLen));
        else
            stretcher->prime();

        src.stemTimeStretchersOwned[s] = stretcher;
    }

    // Cache latency (identical for all stretchers with same params)
    src.stemStretcherLatency = src.stemTimeStretchersOwned[0]->getLatency();

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

    // Clear output buffers.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (numOutputChannels < 2 || numSamples == 0)
        return;

    auto* outL = outputChannelData[0];
    auto* outR = outputChannelData[1];

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
            else if (source->prevIsSynced)
            {
                // Transition synced→unsynced: snap speedMultiplier to pitch fader value
                const float faderMul = audioState->pitchFaderMultiplier.load (std::memory_order_relaxed);
                audioState->speedMultiplier.store (faderMul, std::memory_order_relaxed);
            }
            source->prevIsSynced = curIsSynced;
}

        // 4. Read speed
        float speed = juce::jmax (0.0f,
            audioState->speedMultiplier.load (std::memory_order_relaxed));

        // Quick path: not playing and no active fade → silence
        if (status != PlaybackStatusCode::playing
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
        float gain   = audioState->gain.load (std::memory_order_relaxed);
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
        auto* stretcher = source->timeStretcher.load (std::memory_order_acquire);
        // Use the stretcher whenever key lock is on — even at speed=1.0 the
        // stretcher runs in passthrough so its internal buffers stay warm.
        // This prevents a cold-start click when the pitch fader first moves.
        bool useStretcher = keyLockOn && stretcher != nullptr && speed > 0.01f;

        // Detect key lock toggle — apply a short crossfade between
        // vinyl and stretched paths to avoid a click at the transition.
        // The stretcher runs continuously (always fed) so its output is
        // valid at all times, making the crossfade seamless.
        if (keyLockOn != source->wasKeyLockEnabled)
        {
            source->wasKeyLockEnabled = keyLockOn;
            source->keyLockFadeSamplesRemaining = DeckAudioSource::KEY_LOCK_FADE_LENGTH;
            source->keyLockFadingIn  = keyLockOn;   // TO stretched
            source->keyLockFadingOut = ! keyLockOn;  // FROM stretched
        }

        // --- Time-stretched path ---
        // Always feed the stretcher when it exists so its internal buffers
        // stay warm.  This prevents a cold-start click when key lock is
        // toggled on — the stretched output is already valid for the
        // crossfade.  At ratio 1.0 the R3 engine is near-passthrough and
        // adds negligible CPU.
        int stretchedAvail = 0;
        if (stretcher != nullptr && speed > 0.01f)
        {
            // Smoothly ramp the time ratio toward the target so that
            // setTimeRatio() never sees an abrupt jump — this prevents the
            // R3 engine's pipeline from destabilising (crackling) when the
            // pitch fader is moved while key lock is active.
            double targetTimeRatio = 1.0 / static_cast<double> (speed);
            double& sRatio = source->smoothedTimeRatio;
            double delta = targetTimeRatio - sRatio;
            double maxD  = DeckAudioSource::STRETCH_RATIO_MAX_DELTA;
            if (delta > maxD)       sRatio += maxD;
            else if (delta < -maxD) sRatio -= maxD;
            else                    sRatio  = targetTimeRatio;

            // Feed enough source samples for the smoothed ratio to produce
            // exactly numSamples of output.
            int sourceSamples = std::min (
                static_cast<int> (std::ceil (static_cast<double> (numSamples) / sRatio)),
                DeckAudioSource::MAX_STRETCH_BLOCK);

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

        if (stemActive && keyLockOn && speed > 0.01f
            && ! source->stemStretchDegraded.load (std::memory_order_relaxed))
        {
            auto* stemStr0 = source->stemTimeStretchers[0].load(std::memory_order_acquire);
            if (stemStr0 != nullptr)
            {
                useStemStretchers = true;

                int sourceSamples = std::min(
                    static_cast<int>(std::ceil(static_cast<double>(numSamples) * static_cast<double>(speed))),
                    DeckAudioSource::MAX_STRETCH_BLOCK);

                double timeRatio = 1.0 / static_cast<double>(speed);

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
            if (status != PlaybackStatusCode::playing
                && source->fadeRampSamplesRemaining <= 0)
                continue;

            double pos  = source->playheadAccumulator;
            int64_t idx = static_cast<int64_t> (pos);

            // End-of-track check
            if (pos >= static_cast<double> (bufLen))
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
            bool shouldAdvance = (status == PlaybackStatusCode::playing)
                                 || source->isFadingOut;

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

            // Post-gain+fade accumulation
            outL[i] += rawL * gain * fadeGain;
            outR[i] += rawR * gain * fadeGain;

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
            if (validLoop && shouldAdvance && ! source->isFadingOut
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

                // PRD-0017: Loop wrap-around sets slip displacement
                if (slipEnabled && ! source->slipDisplacedLocal)
                    source->slipDisplacedLocal = true;
            }

            // PRD-0017: Shadow playhead management
            if (slipEnabled)
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

    // Hard clip output to [-1, 1].
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
        updateDeviceState();
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
