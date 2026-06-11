#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "DeckAudioSource.h"
#include "AudioBufferHolder.h"
#include "../Deck/DeckIdentifiers.h"
#include "../../Features/TimeStretch/TimeStretcher.h"
#include "AudioEngineMidiBridge.h"   // PRD-0041
#include "../Mixer/State/MixerAtomicSnapshot.h"       // PRD-0053
#include "../Mixer/State/MixerMeterSnapshot.h"        // PRD-0058
#include "../Mixer/Routing/ChannelStripSnapshot.h"    // PRD-0053
#include "../Mixer/Routing/CrossfaderSnapshot.h"      // PRD-0053
#include "../Mixer/Routing/MasterSnapshot.h"          // PRD-0053
#include "../Mixer/Routing/ChannelStripProcessor.h"   // PRD-0053
#include "../Mixer/Routing/ABBus.h"                   // PRD-0053
#include "../Mixer/Routing/CrossfaderStage.h"         // PRD-0053
#include "../Mixer/Routing/MasterStage.h"             // PRD-0053
#include "../Daw/Playback/ArrangementPublisher.h"     // PRD-0079
#include "../Daw/Playback/ClipStreamer.h"              // PRD-0080
#include "../Daw/Playback/TimelineRenderer.h"          // PRD-0081
#include "../Daw/Playback/DawTransport.h"              // PRD-0082
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

class MasterClockPublisher; // PRD-0026
class SyncEngine;           // PRD-0027
class PhaseLockEngine;      // PRD-0028

class AudioEngine final : public juce::AudioIODeviceCallback,
                           public juce::ChangeListener,
                           public sonik::midi::AudioMidiEventHandler, // PRD-0041
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

    /// Set stem buffers for a deck. Called on the message thread only (PRD-0021).
    /// Stores 4 buffer holders and publishes raw pointers with release stores.
    /// Does NOT activate stems: per PRD-0062 the deck stays on the original
    /// source until the DJ deliberately switches to "stems" via setDeckSourceMode.
    /// The original buffer holder is retained for instant, click-free switching.
    void setDeckStemBuffers (const juce::String& deckId,
                             AudioBufferHolder::Ptr vocals,
                             AudioBufferHolder::Ptr drums,
                             AudioBufferHolder::Ptr bass,
                             AudioBufferHolder::Ptr other);

    /// Select the deck's playback source (PRD-0062). Called on the message
    /// thread only. When useStems is true and stem buffers exist, builds the
    /// stem stretcher set first (if key-lock is active) and then publishes
    /// stemsActive = true so the audio thread never sees a half-built set.
    /// When useStems is false, publishes stemsActive = false (the audio thread
    /// crossfades back to the original buffer). If no stem buffers exist the
    /// request to enable stems is a no-op (deck stays locked to original).
    void setDeckSourceMode (const juce::String& deckId, bool useStems);

    /// Clear stem buffers for a deck. Called on the message thread only (PRD-0021).
    /// Sets stemsActive = false, nullifies stem channel pointers, releases holders.
    void clearDeckStemBuffers (const juce::String& deckId);

    /// Create per-stem time stretchers for a deck (message thread only, PRD-0022).
    /// Called when both key lock is on AND stems are active.
    void createStemStretchers (const juce::String& deckId);

    /// Destroy per-stem time stretchers for a deck (message thread only, PRD-0022).
    void destroyStemStretchers (const juce::String& deckId);

    /// Retrieve the audio buffer for a deck (message thread only).
    AudioBufferHolder::Ptr getDeckBuffer (const juce::String& deckId);

    /// Send a transport command to a deck. Thread-safe, lock-free.
    void sendTransportCommand (const juce::String& deckId, TransportCommand cmd);

    /// Seek a deck to a specific sample position. Thread-safe, lock-free.
    void seekDeck (const juce::String& deckId, int64_t targetSample);

    /// Scratch seek: emits a short audible burst even when paused/stopped,
    /// without changing transport status. Used by waveform touch-drag.
    void scratchSeekDeck (const juce::String& deckId, int64_t targetSample);

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

    /// Wire the master clock publisher into all deck slots (PRD-0026).
    /// Call from the message thread. The pointer is stored atomically in each DeckAudioSource.
    void setMasterClockPublisher (MasterClockPublisher* publisher);

    /// PRD-0041: Wire the MIDI message bridge. Message thread only. The audio
    /// thread reads `midiBridge` with acquire each callback and drains the
    /// FIFO at the top of the block before any audio processing.
    void setMidiMessageBridge (sonik::midi::MidiMessageBridge* bridge);

    /// PRD-0041: AudioMidiEventHandler. Called from the audio thread for every
    /// inbound jog/scratch event drained from the FIFO. Stub for now; the
    /// downstream jog-control logic lives in PRD-0042/0044.
    void applyAudioMidiEvent (const sonik::midi::MidiAudioEvent&) noexcept override;

    /// PRD-0053: Wire the mixer atomic snapshot so the audio thread can read
    /// channel-strip, crossfader, and master parameters each block.
    /// Call from the message thread before starting the audio device.
    void setMixerAtomicSnapshot (MixerAtomicSnapshot* snapshot) noexcept;

    /// PRD-0058: Wire the mixer meter snapshot so the audio thread can
    /// publish per-channel and master metering each block. Call from the
    /// message thread before starting the audio device.
    void setMixerMeterSnapshot (MixerMeterSnapshot* snapshot) noexcept;

    /// PRD-0082: Wire the DAW arrangement publisher and transport for timeline playback.
    /// Call from the message thread before starting the audio device.
    void setDawPlayback (Daw::ArrangementPublisher* publisher,
                         Daw::ClipStreamerPool*     streamerPool,
                         Daw::DawTransport*         transport) noexcept;

    /// Metronome (testing aid). Toggles an audible click locked to the master
    /// grid. The click is summed into the LIVE output only — it is never part of
    /// the DAW recording (a source-file reconstruction, not an output capture)
    /// nor of an offline export, so "audible but not recorded" is intrinsic.
    /// Message thread.
    void setMetronomeEnabled (bool enabled) noexcept;

    /// True while the metronome is enabled (any thread).
    bool isMetronomeEnabled() const noexcept
    {
        return metronomeEnabled_.load (std::memory_order_acquire);
    }

    /// Publish the DAW arrangement grid expressed in the transport's runtime
    /// sample domain so the audio-thread metronome can tick on the beat during
    /// arrangement playback. (During live recording the click is driven straight
    /// from the master clock on the audio thread, so this is only consumed while
    /// the DAW transport is playing.) Message thread.
    void setMetronomeGrid (double  beatLenRuntimeSamples,
                           int64_t phaseOriginRuntimeSamples,
                           int     beatsPerBar) noexcept;

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

    /// Re-point the engine at the current system default output device after a
    /// device-topology change (e.g. headphones plugged into the jack, a USB
    /// interface connected). JUCE binds the AudioDeviceManager to a *specific*
    /// device at start() and does not follow the system default on its own, so
    /// we follow it explicitly here. Message thread only.
    void followDefaultOutputDevice();

    static int deckIdToSlot (const juce::String& deckId);

    /// PRD-0081/0082: (re)build + prepare the DAW timeline renderer if the
    /// arrangement pointers are wired and the device is prepared. Message thread.
    void rebuildDawRenderer();

    juce::AudioDeviceManager deviceManager;
    juce::ValueTree rootState;
    juce::ValueTree audioDeviceNode;

    // Fixed-size deck source slots (lock-free, audio-thread safe)
    std::array<DeckAudioSource, 4>                deckSources;
    std::array<std::atomic<DeckAudioSource*>, 4>  deckSlots;

    // Per-deck sync engines (PRD-0027, owned here, set in setMasterClockPublisher)
    std::array<std::unique_ptr<SyncEngine>, 4>    deckSyncEngines;

    // Per-deck phase lock engines (PRD-0028, owned here, set in setMasterClockPublisher)
    std::array<std::unique_ptr<PhaseLockEngine>, 4> deckPhaseLockEngines;

    // Cached master clock publisher pointer (set in setMasterClockPublisher, audio-thread read).
    // Audio thread uses this each block to write the master deck's playhead for PhaseLockEngine.
    MasterClockPublisher* cachedClockPublisher_ = nullptr;

    // PRD-0041: Lock-free MIDI bridge pointer. Audio thread reads with acquire
    // every callback; Message thread writes with release in setMidiMessageBridge.
    std::atomic<sonik::midi::MidiMessageBridge*> midiBridge { nullptr };

    // PRD-0053: Mixer pipeline stages (owned on the message thread, read on the
    // audio thread via pre-allocated scratch buffers and pass-through logic).
    std::array<ChannelStripProcessor, 4> channelStrips;
    CrossfaderStage                      crossfaderStage;
    MasterStage                          masterStage;

    // PRD-0053: Lock-free pointer to the ValueTree-backed atomic snapshot.
    // Set once on the message thread; read on the audio thread with acquire.
    std::atomic<MixerAtomicSnapshot*> mixerAtomicSnapshot { nullptr };

    // PRD-0058: Lock-free pointer to the metering snapshot. The audio
    // thread publishes per-channel and master meter values into this
    // snapshot every block via relaxed atomic stores.
    std::atomic<MixerMeterSnapshot*>  mixerMeterSnapshot  { nullptr };

    // PRD-0053: Pre-allocated scratch buffers (sized in audioDeviceAboutToStart).
    // All are std::vector<float> — allocated on the message thread only, accessed
    // on the audio thread by raw pointer (no reallocation during a callback).
    std::array<std::vector<float>, 4> deckScratchL;    ///< per-deck output — left
    std::array<std::vector<float>, 4> deckScratchR;    ///< per-deck output — right
    std::array<std::vector<float>, 4> channelScratchL; ///< post-strip output — left
    std::array<std::vector<float>, 4> channelScratchR; ///< post-strip output — right
    std::vector<float> busAL, busAR;                   ///< A bus
    std::vector<float> busBL, busBR;                   ///< B bus
    std::vector<float> masterScratchL, masterScratchR; ///< master scratch

    // PRD-0079-0082: DAW arrangement playback pipeline.
    // All pointers are set once on the message thread; read on the audio thread with acquire.
    std::atomic<Daw::ArrangementPublisher*> dawPublisher_   { nullptr };
    std::atomic<Daw::ClipStreamerPool*>     dawStreamerPool_ { nullptr };
    std::atomic<Daw::DawTransport*>         dawTransport_   { nullptr };
    std::unique_ptr<Daw::TimelineRenderer>  dawRenderer_;
    // Audio-thread-visible raw pointer to the prepared renderer (published with
    // release once fully constructed; read with acquire in processBlock).
    std::atomic<Daw::TimelineRenderer*>     dawRendererPtr_ { nullptr };
    // Pre-allocated master feed buffer for the timeline renderer (sized in audioDeviceAboutToStart).
    std::vector<float> dawMasterFeedL_, dawMasterFeedR_;

    // --- Metronome (testing aid) -----------------------------------------
    // Cross-thread controls: message thread writes, audio thread reads.
    std::atomic<bool>    metronomeEnabled_        { false };
    std::atomic<double>  metroBeatLenRuntime_     { 0.0 }; // samples/beat, transport domain
    std::atomic<int64_t> metroPhaseOriginRuntime_ { 0 };   // grid origin, transport domain
    std::atomic<int>     metroBeatsPerBar_        { 4 };
    // Pre-rendered click waveforms (built in audioDeviceAboutToStart, read on
    // the audio thread): a brighter/louder downbeat and a softer regular beat.
    std::vector<float>   metroClickHi_;
    std::vector<float>   metroClickLo_;
    // Audio-thread-only beat tracking + in-flight click state.
    int64_t  metroLastBeatIndex_         { std::numeric_limits<int64_t>::min() };
    int      metroPrevClockSource_       { 0 }; // 0 none, 1 transport, 2 master clock
    int      metroClickSamplesRemaining_ { 0 };
    bool     metroClickIsDownbeat_       { false };

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
