#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "DeckAudioSource.h"
#include "AudioBufferHolder.h"
#include "../Deck/DeckIdentifiers.h"
#include "../../Features/TimeStretch/TimeStretcher.h"
#include <array>
#include <atomic>

class AudioEngine final : public juce::AudioIODeviceCallback,
                           public juce::ChangeListener,
                           private juce::Timer
{
public:
    explicit AudioEngine (juce::ValueTree parentState);
    ~AudioEngine() override;

    AudioEngine (const AudioEngine&) = delete;
    AudioEngine& operator= (const AudioEngine&) = delete;

    /// Initialise the audio device and begin processing.
    void start();

    /// Stop the audio device and remove callbacks.
    void stop();

    /// Register a deck for audio output. Called on the message thread.
    void registerDeck (const juce::String& deckId, DeckAudioState* audioState);

    /// Unregister a deck. Called on the message thread.
    void unregisterDeck (const juce::String& deckId);

    /// Set the decoded audio buffer for a deck. Called on the message thread.
    /// Updates atomic channel pointers so the audio thread can read from the buffer.
    void setDeckBuffer (const juce::String& deckId, AudioBufferHolder::Ptr holder);

    /// Clear the audio buffer for a deck. Called on the message thread.
    void clearDeckBuffer (const juce::String& deckId);

    /// Retrieve the audio buffer for a deck (message thread only).
    AudioBufferHolder::Ptr getDeckBuffer (const juce::String& deckId);

    /// Send a transport command to a deck. Thread-safe, lock-free.
    void sendTransportCommand (const juce::String& deckId, TransportCommand cmd);

    /// Seek a deck to a specific sample position. Thread-safe, lock-free.
    void seekDeck (const juce::String& deckId, int64_t targetSample);

    /// Seek a deck and start playback atomically. Thread-safe, lock-free.
    void seekAndPlayDeck (const juce::String& deckId, int64_t targetSample);

    /// Slip-aware seek: creates displacement instead of resetting shadow (PRD-0017).
    void slipSeekDeck (const juce::String& deckId, int64_t targetSample);

    /// Slip-aware seek+play: creates displacement instead of resetting shadow (PRD-0017).
    void slipSeekAndPlayDeck (const juce::String& deckId, int64_t targetSample);

    /// Trigger slip return: snap back to shadow playhead position (PRD-0017).
    void sendSlipReturn (const juce::String& deckId);

    /// Returns the current device sample rate.
    double getSampleRate() const { return currentSampleRate.load (std::memory_order_relaxed); }

    float getCpuLoad() const     { return cpuMonitor.loadPercent.load (std::memory_order_relaxed); }
    bool  getCpuOverload() const { return cpuMonitor.overloadWarning.load (std::memory_order_relaxed); }

    // --- AudioIODeviceCallback ---
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // --- ChangeListener ---
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

private:
    void timerCallback() override;
    void updateDeviceState();
    void attemptReconnection();
    static int deckIdToSlot (const juce::String& deckId);

    juce::AudioDeviceManager deviceManager;
    juce::ValueTree rootState;
    juce::ValueTree audioDeviceNode;

    // Fixed-size deck source slots (lock-free, audio-thread safe)
    std::array<DeckAudioSource, 4>                deckSources;
    std::array<std::atomic<DeckAudioSource*>, 4>  deckSlots;

    // CPU load monitoring
    struct CpuLoadMonitor
    {
        std::atomic<float> loadPercent     { 0.0f };
        std::atomic<bool>  overloadWarning { false };
        int consecutiveOverloads = 0; // audio thread only — no atomic needed
    };
    CpuLoadMonitor cpuMonitor;

    // Audio thread state
    std::atomic<double> currentSampleRate  { 44100.0 };
    std::atomic<int>    currentBufferSize  { 128 };

    // Reconnection state (message thread only)
    bool shuttingDown        = false;
    bool deviceDisconnected  = false;
    int  reconnectAttempts   = 0;
    int  timerTicksSinceLastReconnect = 0;

    static constexpr int maxReconnectAttempts   = 5;
    static constexpr int reconnectIntervalTicks = 20;  // 20 × 100 ms = 2 s
    static constexpr int timerIntervalMs        = 100;
};
