#include "AudioEngine.h"
#include <cmath>

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
        // Store the holder first (message thread ownership)
        src.bufferHolder = holder;

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

        auto status = audioState->playbackStatus.load (std::memory_order_relaxed);
        if (status != static_cast<int> (PlaybackStatusCode::playing))
        {
            source->peakL.store (0.0f, std::memory_order_relaxed);
            source->peakR.store (0.0f, std::memory_order_relaxed);
            source->rmsL.store  (0.0f, std::memory_order_relaxed);
            source->rmsR.store  (0.0f, std::memory_order_relaxed);
            continue;
        }

        // Read audio buffer channel pointers (set by AudioFileLoader on message thread).
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

        // ---- Audio mixing path ----
        float gain = audioState->gain.load (std::memory_order_relaxed);
        int64_t pos = audioState->playheadPosition.load (std::memory_order_relaxed);
        float sPeakL = 0.0f, sPeakR = 0.0f;
        float sumSqL = 0.0f, sumSqR = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            int64_t idx = pos + static_cast<int64_t> (i);
            // Clamp to buffer bounds; output silence if beyond end
            float rawL = (idx >= 0 && idx < bufLen) ? chL[idx] : 0.0f;
            float rawR = (idx >= 0 && idx < bufLen) ? chR[idx] : 0.0f;

            // Pre-fader metering
            float absL = std::abs (rawL);
            float absR = std::abs (rawR);
            if (absL > sPeakL) sPeakL = absL;
            if (absR > sPeakR) sPeakR = absR;
            sumSqL += rawL * rawL;
            sumSqR += rawR * rawR;

            // Post-gain accumulation
            outL[i] += rawL * gain;
            outR[i] += rawR * gain;
        }

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
