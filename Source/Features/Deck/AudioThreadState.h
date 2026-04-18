#pragma once

#include <atomic>
#include <cstdint>
#include <array>
#include <juce_data_structures/juce_data_structures.h>
#include "DeckIdentifiers.h"

// Playback status codes for lock-free audio thread access
enum class PlaybackStatusCode : int
{
    empty   = 0,
    stopped = 1,
    playing = 2,
    paused  = 3
};

// Plain-old-data struct with only std::atomic members.
// The audio thread reads these; the message thread writes them via AudioStateSync.
struct DeckAudioState
{
    std::atomic<float>    gain            { 1.0f };
    std::atomic<float>    speedMultiplier { 1.0f };
    std::atomic<int>      playbackStatus  { static_cast<int> (PlaybackStatusCode::empty) };
    std::atomic<int64_t>  playheadPosition { 0 };
    std::atomic<int64_t>  tempCuePosition { -1 };
    std::atomic<bool>     quantizeEnabled { false };
    std::atomic<bool>     slipEnabled     { false };
    std::atomic<bool>     keyLockEnabled  { false };

    // BeatGrid parameters (PRD-0008/0013) – audio thread reads, message thread writes
    std::atomic<int64_t>  beatgridAnchor   { 0 };
    std::atomic<double>   beatgridInterval { 0.0 };

    // Loop state (PRD-0014) – audio thread reads, message thread writes
    std::atomic<int64_t>  loopInSamples  { -1 };
    std::atomic<int64_t>  loopOutSamples { -1 };
    std::atomic<bool>     loopActive     { false };

    // Hot cue positions (PRD-0012) – audio thread reads, message thread writes
    std::atomic<int64_t>  hotCuePositions[8] = { {-1}, {-1}, {-1}, {-1}, {-1}, {-1}, {-1}, {-1} };
};

// Syncs ValueTree property changes to DeckAudioState atomics on the message thread.
class AudioStateSync final : private juce::ValueTree::Listener
{
public:
    AudioStateSync (juce::ValueTree deckTree, DeckAudioState& audioState)
        : tree (deckTree), state (audioState)
    {
        tree.addListener (this);
        syncAll();
    }

    ~AudioStateSync() override
    {
        tree.removeListener (this);
    }

    AudioStateSync (const AudioStateSync&) = delete;
    AudioStateSync& operator= (const AudioStateSync&) = delete;

private:
    void syncAll()
    {
        state.gain.store (static_cast<float> (tree.getProperty (IDs::gain, 1.0f)),
                          std::memory_order_relaxed);
        state.speedMultiplier.store (static_cast<float> (tree.getProperty (IDs::speedMultiplier, 1.0f)),
                                    std::memory_order_relaxed);
        state.playbackStatus.store (statusStringToCode (tree.getProperty (IDs::playbackStatus, "empty")),
                                   std::memory_order_relaxed);
        state.quantizeEnabled.store (static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false)),
                                    std::memory_order_relaxed);
        state.slipEnabled.store (static_cast<bool> (tree.getProperty (IDs::slipEnabled, false)),
                                std::memory_order_relaxed);
        state.keyLockEnabled.store (static_cast<bool> (tree.getProperty (IDs::keyLockEnabled, false)),
                                   std::memory_order_relaxed);

        auto playhead = tree.getChildWithName (IDs::Playhead);
        if (playhead.isValid())
            state.playheadPosition.store (static_cast<int64_t> (playhead.getProperty (IDs::position, 0)),
                                         std::memory_order_relaxed);

        auto tempCue = tree.getChildWithName (IDs::TempCue);
        if (tempCue.isValid())
            state.tempCuePosition.store (static_cast<int64_t> (tempCue.getProperty (IDs::position, -1)),
                                        std::memory_order_relaxed);

        // Sync hot cue positions (PRD-0012)
        auto cuePoints = tree.getChildWithName (IDs::CuePoints);

        // Sync loop state (PRD-0014)
        auto loop = tree.getChildWithName (IDs::Loop);
        if (loop.isValid())
        {
            state.loopInSamples.store (
                static_cast<int64_t> (loop.getProperty (IDs::loopIn, -1)),
                std::memory_order_relaxed);
            state.loopOutSamples.store (
                static_cast<int64_t> (loop.getProperty (IDs::loopOut, -1)),
                std::memory_order_relaxed);
            state.loopActive.store (
                static_cast<bool> (loop.getProperty (IDs::active, false)),
                std::memory_order_relaxed);
        }

        // Sync beatgrid parameters (PRD-0013)
        auto beatGrid = tree.getChildWithName (IDs::BeatGrid);
        if (beatGrid.isValid())
        {
            state.beatgridAnchor.store (
                static_cast<int64_t> (beatGrid.getProperty (IDs::anchorSample, 0)),
                std::memory_order_relaxed);
            state.beatgridInterval.store (
                static_cast<double> (beatGrid.getProperty (IDs::beatIntervalSamples, 0.0)),
                std::memory_order_relaxed);
        }
        if (cuePoints.isValid())
        {
            for (int i = 0; i < cuePoints.getNumChildren() && i < 8; ++i)
            {
                auto cp = cuePoints.getChild (i);
                int idx = static_cast<int> (cp.getProperty (IDs::index, i));
                if (idx >= 0 && idx < 8)
                {
                    bool valid = static_cast<bool> (cp.getProperty (IDs::isValid, false));
                    int64_t pos = valid ? static_cast<int64_t> (cp.getProperty (IDs::position, -1)) : -1;
                    state.hotCuePositions[idx].store (pos, std::memory_order_relaxed);
                }
            }
        }
    }

    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override
    {
        if (changedTree == tree)
        {
            if (property == IDs::gain)
                state.gain.store (static_cast<float> (changedTree[property]), std::memory_order_relaxed);
            else if (property == IDs::speedMultiplier)
                state.speedMultiplier.store (static_cast<float> (changedTree[property]), std::memory_order_relaxed);
            else if (property == IDs::playbackStatus)
                state.playbackStatus.store (statusStringToCode (changedTree[property]), std::memory_order_relaxed);
            else if (property == IDs::quantizeEnabled)
                state.quantizeEnabled.store (static_cast<bool> (changedTree[property]), std::memory_order_relaxed);
            else if (property == IDs::slipEnabled)
                state.slipEnabled.store (static_cast<bool> (changedTree[property]), std::memory_order_relaxed);
            else if (property == IDs::keyLockEnabled)
                state.keyLockEnabled.store (static_cast<bool> (changedTree[property]), std::memory_order_relaxed);
        }
        else if (changedTree.hasType (IDs::Playhead) && property == IDs::position)
        {
            state.playheadPosition.store (static_cast<int64_t> (changedTree[property]), std::memory_order_relaxed);
        }
        else if (changedTree.hasType (IDs::TempCue) && property == IDs::position)
        {
            state.tempCuePosition.store (static_cast<int64_t> (changedTree[property]), std::memory_order_relaxed);
        }
        // Hot cue position/validity changed (PRD-0012)
        else if (changedTree.hasType (IDs::CuePoint)
                 && (property == IDs::position || property == IDs::isValid))
        {
            int idx = static_cast<int> (changedTree.getProperty (IDs::index, -1));
            if (idx >= 0 && idx < 8)
            {
                bool valid = static_cast<bool> (changedTree.getProperty (IDs::isValid, false));
                int64_t pos = valid ? static_cast<int64_t> (changedTree.getProperty (IDs::position, -1)) : -1;
                state.hotCuePositions[idx].store (pos, std::memory_order_relaxed);
            }
        }
        // BeatGrid parameters changed (PRD-0013)
        else if (changedTree.hasType (IDs::BeatGrid))
        {
            if (property == IDs::anchorSample)
                state.beatgridAnchor.store (static_cast<int64_t> (changedTree[property]),
                                            std::memory_order_relaxed);
            else if (property == IDs::beatIntervalSamples)
                state.beatgridInterval.store (static_cast<double> (changedTree[property]),
                                              std::memory_order_relaxed);
        }
        // Loop state changed (PRD-0014)
        else if (changedTree.hasType (IDs::Loop))
        {
            if (property == IDs::loopIn)
                state.loopInSamples.store (static_cast<int64_t> (changedTree[property]),
                                           std::memory_order_relaxed);
            else if (property == IDs::loopOut)
                state.loopOutSamples.store (static_cast<int64_t> (changedTree[property]),
                                            std::memory_order_relaxed);
            else if (property == IDs::active)
                state.loopActive.store (static_cast<bool> (changedTree[property]),
                                        std::memory_order_relaxed);
        }
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    static int statusStringToCode (const juce::var& v)
    {
        auto s = v.toString();
        if (s == "playing") return static_cast<int> (PlaybackStatusCode::playing);
        if (s == "paused")  return static_cast<int> (PlaybackStatusCode::paused);
        if (s == "stopped") return static_cast<int> (PlaybackStatusCode::stopped);
        return static_cast<int> (PlaybackStatusCode::empty);
    }

    juce::ValueTree tree;
    DeckAudioState& state;
};
