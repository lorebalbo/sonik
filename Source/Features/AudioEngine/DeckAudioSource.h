#pragma once

#include <atomic>
#include <cstdint>
#include "AudioBufferHolder.h"
#include "../Deck/AudioThreadState.h"

class TimeStretcher;
class MasterClockPublisher; // PRD-0026: forward-declared to avoid pulling in Sync headers here

// Transport commands sent from UI thread to audio thread (PRD-0004)
enum class TransportCommand : int
{
    None = 0,
    Play,
    Pause,
    Stop,
    Seek,
    CueSet,
    CueReturn,
    CuePreview,
    CueRelease,
    CuePlayThrough,
    SeekAndPlay,
    SlipSeek,          // Seek that creates slip displacement (PRD-0017)
    SlipSeekAndPlay,   // SeekAndPlay that creates slip displacement (PRD-0017)
    SlipReturn         // Snap back to shadow playhead position (PRD-0017)
};

/// Lightweight struct representing a deck's audio contribution.
/// The audio thread reads raw channel pointers via atomics (zero-alloc).
/// The message thread owns the AudioBufferHolder and sets up pointers.
struct DeckAudioSource
{
    // Raw channel pointers for audio thread (non-owning, point into holder's buffer)
    std::atomic<const float*> channelL { nullptr };
    std::atomic<const float*> channelR { nullptr };
    std::atomic<int64_t>      bufferNumFrames { 0 };

    // Ownership of decoded buffer (message thread only, NOT accessed by audio thread)
    AudioBufferHolder::Ptr bufferHolder;

    // Audio state from PRD-0001 (set by AudioEngine::registerDeck)
    DeckAudioState* audioState = nullptr;

    // Metering output (written by audio thread, read by UI)
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };
    std::atomic<float> rmsL  { 0.0f };
    std::atomic<float> rmsR  { 0.0f };

    // --- Transport (PRD-0004) ---

    // Command slot from UI thread (consumed by audio thread)
    std::atomic<int>     pendingCommand { 0 };
    std::atomic<int64_t> seekTarget     { 0 };

    // Playhead sub-sample accumulator (audio thread only)
    double playheadAccumulator = 0.0;

    // Temp cue preview state (audio thread only)
    bool isCuePreviewing = false;

    // Fade ramp state (audio thread only)
    int  fadeRampSamplesRemaining = 0;
    bool isFadingIn  = false;
    bool isFadingOut = false;
    static constexpr int FADE_RAMP_LENGTH = 64;

    // Deferred action after fade-out completes (audio thread only)
    enum class DeferredAction : int { None = 0, Pause, Stop, Seek, CueReturn, EndOfTrack, SlipReturn };
    DeferredAction deferredAction    = DeferredAction::None;
    int64_t        deferredSeekTarget = 0;
    bool           deferredIsSlipDisplacement = false; // marks Seek as slip displacement

    // --- Time stretching (PRD-0011) ---

    // Stretcher instance (owned by message thread, published via atomic for audio thread)
    std::atomic<TimeStretcher*> timeStretcher { nullptr };
    TimeStretcher* timeStretcherOwned = nullptr; // message thread ownership

    // Deferred deletion: the previously-active stretcher is kept alive here
    // until the *next* replacement, guaranteeing the audio thread has finished
    // any in-flight process() call before the object is freed.
    TimeStretcher* timeStretcherPendingDelete = nullptr; // message thread only

    // Cached stretcher latency for read-position compensation (audio thread)
    int stretcherLatency = 0;

    // Key lock crossfade state (audio thread only)
    // Longer ramp than FADE_RAMP_LENGTH so the vinyl↔stretched transition
    // (which may involve a pitch shift) is masked.  46 ms at 44100 Hz.
    static constexpr int KEY_LOCK_FADE_LENGTH = 2048;
    int  keyLockFadeSamplesRemaining = 0;
    bool keyLockFadingIn  = false;  // transitioning TO stretched
    bool keyLockFadingOut = false;  // transitioning FROM stretched
    bool wasKeyLockEnabled = false; // previous frame's key lock state

    // Loop crossfade state (audio thread only, PRD-0014)
    int    loopFadeRemaining    = 0;
    int    loopCrossfadeLength  = 64;
    double loopFadeReadPos      = 0.0;

    // Slip mode state (audio thread only, PRD-0017)
    double shadowPlayheadAccumulator = 0.0;  // shadow playhead for slip mode
    bool   slipDisplacedLocal        = false; // cached displacement state
    bool   wasLoopActiveLastBlock    = false; // for detecting loop exit

    // Pre-allocated scratch buffers for stretcher I/O (audio thread only)
    static constexpr int MAX_STRETCH_BLOCK = 4096;
    float stretchInL[MAX_STRETCH_BLOCK]  = {};
    float stretchInR[MAX_STRETCH_BLOCK]  = {};
    float stretchOutL[MAX_STRETCH_BLOCK] = {};
    float stretchOutR[MAX_STRETCH_BLOCK] = {};

    // Smoothed time ratio for the stretcher (audio thread only).
    // Updated each block toward 1/speedMultiplier at a limited rate so
    // setTimeRatio() never sees abrupt jumps — prevents crackling during
    // pitch-fader movement when key lock is active.
    // Max delta of 0.003/block ≈ ±0.5/sec → ~2% pitch change in ~40 ms.
    double smoothedTimeRatio = 1.0;
    static constexpr double STRETCH_RATIO_MAX_DELTA = 0.003;

    // --- Stem buffers (PRD-0021) ---
    static constexpr int NUM_STEMS = 4;
    static constexpr int STEM_CROSSFADE_LENGTH = 64;

    // Raw stem channel pointers for audio thread (non-owning, point into holders' buffers)
    std::atomic<const float*> stemChannelL[NUM_STEMS] = { {nullptr}, {nullptr}, {nullptr}, {nullptr} };
    std::atomic<const float*> stemChannelR[NUM_STEMS] = { {nullptr}, {nullptr}, {nullptr}, {nullptr} };
    std::atomic<int64_t>      stemBufferNumFrames[NUM_STEMS] = { {0}, {0}, {0}, {0} };

    // Stems active flag (message thread writes with release, audio thread reads with acquire)
    std::atomic<bool> stemsActive { false };

    // Ownership of stem buffers (message thread only, NOT accessed by audio thread)
    AudioBufferHolder::Ptr stemBufferHolders[NUM_STEMS];

    // Per-stem mute crossfade state (audio thread only, AC#11)
    int  stemFadeRemaining[NUM_STEMS] = { 0, 0, 0, 0 };
    bool stemFadeDirection[NUM_STEMS] = { false, false, false, false }; // true = unmuting
    bool stemWasMuted[NUM_STEMS]      = { false, false, false, false };

    // Stems activation crossfade state (audio thread only, AC#25)
    int  stemsActivationFadeRemaining = 0;
    bool stemsActivationFadeDirection = false; // true = activating stems
    bool wasStemsActiveLocal          = false;

    // --- Stem time stretching (PRD-0022) ---

    // Per-stem stretcher pointers (audio thread reads via acquire, message thread writes via release)
    std::atomic<TimeStretcher*> stemTimeStretchers[NUM_STEMS] = { {nullptr}, {nullptr}, {nullptr}, {nullptr} };
    // Message-thread ownership pointers
    TimeStretcher* stemTimeStretchersOwned[NUM_STEMS] = { nullptr, nullptr, nullptr, nullptr };
    // Deferred deletion for per-stem stretchers (same pattern as main stretcher)
    TimeStretcher* stemStretchersPendingDelete[NUM_STEMS] = { nullptr, nullptr, nullptr, nullptr };
    // Cached stem stretcher latency (same for all 4, set by message thread, read by audio thread)
    int stemStretcherLatency = 0;

    // Pre-allocated scratch buffers for per-stem stretcher I/O (audio thread only)
    float stemStretchInL[NUM_STEMS][MAX_STRETCH_BLOCK]  = {};
    float stemStretchInR[NUM_STEMS][MAX_STRETCH_BLOCK]  = {};
    float stemStretchOutL[NUM_STEMS][MAX_STRETCH_BLOCK] = {};
    float stemStretchOutR[NUM_STEMS][MAX_STRETCH_BLOCK] = {};

    // CPU degradation flag: when true, per-stem stretching fell back to single stretcher
    std::atomic<bool> stemStretchDegraded { false };

    // --- Master Clock (PRD-0026) ---
    // Set by AudioEngine::setMasterClockPublisher() on the message thread before audio starts.
    // Audio thread calls masterClockPublisher.load(acquire) then publisher->read() to get clock.
    // The pointer itself is atomic; the SeqLock inside MasterClockPublisher makes read() lock-free.
    std::atomic<MasterClockPublisher*> masterClockPublisher { nullptr };

    // --- Sync Engine state (PRD-0027, audio thread only) ---
    // Tracks whether the deck was synced last block, used to detect synced→unsynced transition.
    bool prevIsSynced = false;

    // --- Phase Lock Engine state (PRD-0028) ---
    // correctionMultiplier is audio-thread-only (never std::atomic): only read/written inside
    // audioDeviceIOCallbackWithContext.  Applied multiplicatively to speedMultiplier each block.
    double correctionMultiplier = 1.0;

    // phaseOffset is published from the audio thread for the phase meter UI (PRD-0029).
    // Written each block by PhaseLockEngine, read asynchronously by the message thread.
    std::atomic<float> phaseOffset { 0.0f };
};
