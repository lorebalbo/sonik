#include "AudioEngine.h"
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
}

void AudioEngine::setDeckBuffer (const juce::String& deckId, AudioBufferHolder::Ptr holder)
{
    int slot = deckIdToSlot (deckId);
    if (slot < 0)
        return;

    auto& src = deckSources[static_cast<size_t> (slot)];

    if (holder != nullptr && holder->getBuffer().getNumChannels() >= 2)
    {
        // Reset transport state so the new track loads stopped at position 0
        src.pendingCommand.store (0, std::memory_order_relaxed);
        src.seekTarget.store (0, std::memory_order_relaxed);
        src.playheadAccumulator = 0.0;
        src.isCuePreviewing = false;
        src.fadeRampSamplesRemaining = 0;
        src.isFadingIn = false;
        src.isFadingOut = false;

        if (src.audioState != nullptr)
        {
            src.audioState->playbackStatus.store (1, std::memory_order_relaxed); // stopped
            src.audioState->playheadPosition.store (0, std::memory_order_relaxed);
            src.audioState->tempCuePosition.store (0, std::memory_order_relaxed);
        }

        // Store the holder first (message thread ownership)
        src.bufferHolder = holder;

        // Create time stretcher for this deck (message thread, allocates buffers)
        {
            double sr = currentSampleRate.load (std::memory_order_relaxed);
            int maxBlock = currentBufferSize.load (std::memory_order_relaxed);
            if (maxBlock <= 0) maxBlock = 512;

            // Tear down old stretcher first (hide from audio thread)
            src.timeStretcher.store (nullptr, std::memory_order_release);
            delete src.timeStretcherOwned;

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

    // Null the pointers first so the audio thread stops reading
    src.channelL.store (nullptr, std::memory_order_release);
    src.channelR.store (nullptr, std::memory_order_release);
    src.bufferNumFrames.store (0, std::memory_order_relaxed);

    // Destroy time stretcher (hide from audio thread first)
    src.timeStretcher.store (nullptr, std::memory_order_release);
    delete src.timeStretcherOwned;
    src.timeStretcherOwned = nullptr;
    src.stretcherLatency = 0;

    // Release ownership
    src.bufferHolder = nullptr;
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
                }
                else
                {
                    source->playheadAccumulator = static_cast<double> (target);
                }
                break;
            }

            case TransportCommand::CueSet:
                if (status == PlaybackStatusCode::paused)
                {
                    auto curPos = static_cast<int64_t> (source->playheadAccumulator);
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
                }
                else
                {
                    // Stopped/paused → seek instantly then start playing with fade-in
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

            case TransportCommand::None:
            default:
                break;
        }

        // Re-read status after command processing
        status = static_cast<PlaybackStatusCode> (
            audioState->playbackStatus.load (std::memory_order_relaxed));

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

        // Check key lock state
        bool keyLockOn = audioState->keyLockEnabled.load (std::memory_order_relaxed);
        auto* stretcher = source->timeStretcher.load (std::memory_order_acquire);
        // Use the stretcher whenever key lock is on — even at speed=1.0 the
        // stretcher runs in passthrough so its internal buffers stay warm.
        // This prevents a cold-start click when the pitch fader first moves.
        bool useStretcher = keyLockOn && stretcher != nullptr && speed > 0.01f;

        // Detect key lock toggle — no crossfade needed.
        // The stretcher runs continuously (always fed) so its output is
        // valid at all times.  A crossfade between vinyl and stretched
        // signals is harmful because RubberBand's algorithmic latency
        // (~2048-4096 samples) puts the two signals at different temporal
        // positions in the track, causing phase cancellation and clicks.
        // At speed=1.0 both paths produce identical audio.  At non-1.0
        // speeds a hard switch is inaudible because the stretcher output
        // is already continuous.
        if (keyLockOn != source->wasKeyLockEnabled)
            source->wasKeyLockEnabled = keyLockOn;

        // --- Time-stretched path ---
        // Always feed the stretcher when it exists so its internal buffers
        // stay warm.  This prevents a cold-start click when key lock is
        // toggled on — the stretched output is already valid for the
        // crossfade.  At ratio 1.0 the R3 engine is near-passthrough and
        // adds negligible CPU.
        int stretchedAvail = 0;
        if (stretcher != nullptr && speed > 0.01f)
        {
            // Read source samples at speed into scratch buffers.
            // Offset by stretcherLatency so the stretcher output (which is
            // delayed by L samples internally) is temporally aligned with
            // the vinyl path at playheadAccumulator.
            int sourceSamples = std::min (
                static_cast<int> (std::ceil (static_cast<double> (numSamples) * static_cast<double> (speed))),
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

            double timeRatio = 1.0 / static_cast<double> (speed);
            stretchedAvail = stretcher->process (
                inPtrs, sourceSamples, outPtrs, numSamples, timeRatio);
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

            // Select output sample: stretched or vinyl
            float rawL, rawR;
            if (useStretcher)
            {
                // Use stretched output when available; on underrun, use
                // vinyl to avoid silence gaps.
                rawL = (i < stretchedAvail) ? source->stretchOutL[i] : vinylL;
                rawR = (i < stretchedAvail) ? source->stretchOutR[i] : vinylR;
            }
            else
            {
                rawL = vinylL;
                rawR = vinylR;
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
                                break;

                            case DeckAudioSource::DeferredAction::Seek:
                                source->playheadAccumulator =
                                    static_cast<double> (source->deferredSeekTarget);
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
        }

        // 6. Update atomic playhead
        audioState->playheadPosition.store (
            static_cast<int64_t> (source->playheadAccumulator),
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
            cpuMonitor.overloadWarning.store (true, std::memory_order_relaxed);
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
