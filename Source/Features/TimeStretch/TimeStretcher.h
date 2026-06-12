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

    /// Set the pitch scale.  1.0 = no shift; 2.0 = one octave up; 0.5 = one octave down.
    /// For semitone shifts, pass pow(2.0, semitones / 12.0).
    /// Audio-thread safe in RealTime mode (no allocation, no locks).
    /// The actual setPitchScale() call is only forwarded to RubberBand when the
    /// value changes meaningfully, avoiding redundant internal phase resets.
    void setPitchScale (double scale);

    /// Flush internal buffers. Call when loading a new track or seeking.
    /// Audio-thread safe in RealTime mode.
    void reset();

    /// Returns the algorithmic latency in samples.
    int getLatency() const;

    /// Preferred number of start-pad samples to feed before real audio so the
    /// pipeline is primed (RubberBand getPreferredStartPad). Combined with
    /// getLatency() this is the read-ahead a streaming caller feeds + discards so
    /// its first real output aligns to source position 0 (zero phase offset).
    int getStartPad() const;

    /// Returns the currently queued output samples inside RubberBand.
    /// Primarily used by tests/diagnostics.
    int getBufferedOutputSamples() const;

    /// Feed silence to prime the stretcher after construction or reset.
    /// Call on the message thread before publishing to the audio thread.
    /// When cushionFrames > 0, additionally feeds silence until at least that
    /// many output samples sit queued (see primeWithAudio for why).
    /// @return The number of extra frames fed for the cushion — add this to
    ///         the read-ahead offset, exactly as primeWithAudio does.
    int prime (int cushionFrames = 0);

    /// Prime the stretcher with actual track audio so its output is
    /// immediately valid when playback starts.  Feeds getPreferredStartPad()
    /// + getLatency() samples from the track buffer, reading from position 0,
    /// then (when cushionFrames > 0) feeds further audio until at least
    /// cushionFrames of output sit queued inside the stretcher.  The cushion
    /// guarantees available() never dips below one callback block at steady
    /// state, so the audio thread never has to splice unstretched samples
    /// into the stretched stream.
    /// Call on the message thread before publishing to the audio thread.
    ///
    /// @return Effective pipeline depth: the value to use as stretcherLatency
    ///         in AudioEngine (= getPreferredStartPad() + getLatency() minus
    ///         the output samples discarded during priming, plus the extra
    ///         frames fed for the cushion).  Feed the stretcher from
    ///         playheadAccumulator + this value so its output aligns exactly
    ///         with the vinyl path.
    int primeWithAudio (const float* channelL, const float* channelR,
                        int numFramesAvailable, int cushionFrames = 0);

private:
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    double lastTimeRatio = 1.0;
    double lastPitchScale = 1.0;
};
