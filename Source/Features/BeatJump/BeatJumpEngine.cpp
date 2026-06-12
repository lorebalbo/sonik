#include "BeatJumpEngine.h"
#include "../Loop/LoopEngine.h"
#include "../Daw/Recording/PerformanceCaptureSink.h"

BeatJumpEngine::BeatJumpEngine (juce::ValueTree deckTree,
                                 AudioEngine& engine,
                                 const juce::String& id)
    : tree (deckTree),
      trackMetaNode (deckTree.getChildWithName (IDs::TrackMetadata)),
      beatGridNode (deckTree.getChildWithName (IDs::BeatGrid)),
      loopNode (deckTree.getChildWithName (IDs::Loop)),
      audioEngine (engine),
      deckId (id)
{
}

void BeatJumpEngine::setAudioState (DeckAudioState* state)
{
    audioState = state;
}

void BeatJumpEngine::setLoopEngine (LoopEngine* engine)
{
    loopEngine = engine;
}

void BeatJumpEngine::setPerformanceCapture (Daw::PerformanceCaptureSink* sink, int deckIndex)
{
    capture_          = sink;
    captureDeckIndex_ = deckIndex;
}

void BeatJumpEngine::jumpForward()
{
    executeJump (true);
}

void BeatJumpEngine::jumpBackward()
{
    executeJump (false);
}

void BeatJumpEngine::setJumpSize (double beats)
{
    // Validate against allowed sizes
    for (int i = 0; i < numJumpSizes; ++i)
    {
        if (juce::exactlyEqual (jumpSizes[i], beats))
        {
            tree.setProperty (IDs::beatJumpSize, beats, nullptr);
            return;
        }
    }
}

void BeatJumpEngine::cycleJumpSize (bool forward)
{
    double current = static_cast<double> (tree.getProperty (IDs::beatJumpSize, 4.0));

    int currentIndex = 3; // default to 4.0
    for (int i = 0; i < numJumpSizes; ++i)
    {
        if (juce::exactlyEqual (jumpSizes[i], current))
        {
            currentIndex = i;
            break;
        }
    }

    if (forward)
        currentIndex = (currentIndex + 1) % numJumpSizes;
    else
        currentIndex = (currentIndex - 1 + numJumpSizes) % numJumpSizes;

    tree.setProperty (IDs::beatJumpSize, jumpSizes[currentIndex], nullptr);
}

// ---------------------------------------------------------------------------

void BeatJumpEngine::executeJump (bool forward)
{
    auto status = tree.getProperty (IDs::playbackStatus).toString();
    if (status == "empty")
        return;

    auto totalSamples = static_cast<int64_t> (trackMetaNode.getProperty (IDs::totalSamples, 0));
    if (totalSamples <= 0)
        return;

    double beatInterval = getBeatInterval();
    if (beatInterval <= 0.0)
        return;

    double jumpBeats = static_cast<double> (tree.getProperty (IDs::beatJumpSize, 4.0));
    auto jumpOffset = static_cast<int64_t> (std::round (jumpBeats * beatInterval));

    if (! forward)
        jumpOffset = -jumpOffset;

    // PRD-0017: Check if slip is enabled
    bool slipOn = static_cast<bool> (tree.getProperty (IDs::slipEnabled, false));

    // Check if loop is active — use loop shift instead (unless slip suppresses it)
    bool loopActive = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    if (loopActive && loopEngine != nullptr && ! slipOn)
    {
        // Pass quantize info so shifted boundaries snap to beat grid
        bool quantizeOn = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
        double bgBpm = static_cast<double> (beatGridNode.getProperty (IDs::bpm, 0.0));
        int64_t anchor = static_cast<int64_t> (beatGridNode.getProperty (IDs::anchorSample, 0));
        bool snap = quantizeOn && bgBpm > 0.0;

        bool shifted = loopEngine->shiftLoop (jumpOffset, snap, anchor, beatInterval);
        if (! shifted)
            return; // Rejected (boundary clamp failed)

        // Also seek playhead by the same offset, maintaining relative position
        int64_t playhead = readPlayhead();
        int64_t dest = playhead + jumpOffset;
        dest = std::clamp (dest, int64_t (0), totalSamples - 1);
        audioEngine.seekDeck (deckId, dest);
        return;
    }

    // Normal jump (or slip-mode jump — no loop shift)
    int64_t playhead = readPlayhead();
    int64_t rawDest  = playhead + jumpOffset;

    // Quantize snap if enabled
    bool quantizeEnabled = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
    double bgBpm = static_cast<double> (beatGridNode.getProperty (IDs::bpm, 0.0));

    if (quantizeEnabled && bgBpm > 0.0)
    {
        auto anchor = static_cast<int64_t> (beatGridNode.getProperty (IDs::anchorSample, 0));
        rawDest = QuantizeService::snapToNearestBeat (rawDest, anchor, beatInterval);
    }

    // Clamp to valid range
    int64_t dest = std::clamp (rawDest, int64_t (0), totalSamples - 1);

    // Skip if already at boundary and trying to go further
    if (dest == playhead)
        return;

    // PRD-0075: surface the source discontinuity for recording capture. The
    // loop-active branch above returns early and is owned by PRD-0076.
    if (capture_ != nullptr)
        capture_->captureJump (captureDeckIndex_, Daw::PerformanceEventType::BeatJump,
                               playhead, dest);

    // PRD-0017: Use slip-aware seek when slip is enabled
    if (slipOn)
        audioEngine.slipSeekDeck (deckId, dest);
    else
        audioEngine.seekDeck (deckId, dest);
}

double BeatJumpEngine::getBeatInterval() const
{
    double bgBpm = static_cast<double> (beatGridNode.getProperty (IDs::bpm, 0.0));
    double sr = getSampleRate();

    if (bgBpm > 0.0 && sr > 0.0)
        return sr * 60.0 / bgBpm;

    // Fallback from audio state atomics
    if (audioState != nullptr)
    {
        double interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
        if (interval > 0.0)
            return interval;
    }

    // Fallback: 120 BPM
    return sr * 60.0 / 120.0;
}

double BeatJumpEngine::getSampleRate() const
{
    double sr = static_cast<double> (trackMetaNode.getProperty (IDs::sampleRate, 0.0));
    if (sr > 0.0)
        return sr;
    return 44100.0;
}

int64_t BeatJumpEngine::readPlayhead() const
{
    if (audioState != nullptr)
        return audioState->playheadPosition.load (std::memory_order_relaxed);
    return static_cast<int64_t> (tree.getChildWithName (IDs::Playhead)
                                     .getProperty (IDs::position, 0));
}
