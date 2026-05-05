#pragma once

#include <rubberband/RubberBandStretcher.h>
#include <memory>
#include <cstddef>

/// Wrapper around RubberBand::RubberBandStretcher for real-time key-lock.
///
/// Construct on the message thread (allocates internally). After construction,
/// process() and reset() are audio-thread safe — no allocation, no locks.
class TimeStretcher
{
public:
    /// @param sampleRate       Device sample rate.
    /// @param channels         Number of audio channels (must be 2).
    /// @param maxBlockSize     Maximum block size the audio callback may use.
    TimeStretcher (double sampleRate, int channels, int maxBlockSize);
    ~TimeStretcher();

    TimeStretcher (const TimeStretcher&) = delete;
    TimeStretcher& operator= (const TimeStretcher&) = delete;

    /// Feed source samples to the stretcher and retrieve time-stretched output.
    ///
    /// Audio-thread safe (no alloc after construction in RealTime mode).
    ///
    /// @param input            Array of channel pointers (interleaved L/R).
    /// @param inputSamples     Number of source samples to feed.
    /// @param output           Array of channel pointers to receive output.
    /// @param maxOutputSamples Maximum number of output samples per channel.
    /// @param timeRatio        Time ratio: >1 = slower, <1 = faster.
    ///                         For key lock at speed S, pass 1.0/S so that the
    ///                         stretched output has the original duration.
    /// @return Number of output samples actually written.
    int process (const float* const* input, int inputSamples,
                 float* const* output, int maxOutputSamples,
                 double timeRatio);

    /// Flush internal buffers. Call when loading a new track or seeking.
    /// Audio-thread safe in RealTime mode.
    void reset();

    /// Returns the algorithmic latency in samples.
    int getLatency() const;

    /// Returns the currently queued output samples inside RubberBand.
    /// Primarily used by tests/diagnostics.
    int getBufferedOutputSamples() const;

    /// Feed silence to prime the stretcher after construction or reset.
    /// Call on the message thread before publishing to the audio thread.
    void prime();

    /// Prime the stretcher with actual track audio so its output is
    /// immediately valid when playback starts.  Feeds getPreferredStartPad()
    /// + getLatency() samples from the track buffer, reading from position 0.
    /// Call on the message thread before publishing to the audio thread.
    ///
    /// @return Effective pipeline depth: the value to use as stretcherLatency
    ///         in AudioEngine (= getPreferredStartPad() + getLatency() minus
    ///         the output samples discarded during priming).  Feed the
    ///         stretcher from playheadAccumulator + this value so its output
    ///         aligns exactly with the vinyl path.
    int primeWithAudio (const float* channelL, const float* channelR,
                        int numFramesAvailable);

private:
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    double lastTimeRatio = 1.0;
};
