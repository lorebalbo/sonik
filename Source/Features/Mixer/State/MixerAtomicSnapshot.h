#pragma once
//==============================================================================
// PRD-0052: MixerAtomicSnapshot — lock-free parameter snapshot for the
// audio thread.
//
// The audio thread reads mixer parameters exclusively from this struct
// using std::atomic::load(std::memory_order_relaxed). It never touches the
// juce::ValueTree and never acquires any lock.
//
// MixerStateBridge writes to these atomics on the message thread whenever
// a corresponding juce::ValueTree property changes (see MixerStateBridge).
//
// Continuous parameter values are stored in their audio-thread native form:
//   gain (channel/master) — linear amplitude (pow(10, dB/20), 0 if dB ≤ -60)
//   eqHigh/Mid/Low        — linear amplitude (same conversion)
//   filter                — bipolar float [-1, +1] (sign = HPF/LPF mode,
//                           magnitude = normalised cutoff travel)
//   fader                 — linear [0, 1]
//   crossfader            — linear [0, 1]
//==============================================================================

#include <atomic>

//==============================================================================
// Per-channel parameter atomics.
//==============================================================================
struct ChannelAtomicParams
{
    std::atomic<float> gain     { 1.0f };  // linear (converted from dB)
    std::atomic<float> eqHigh   { 1.0f };  // linear
    std::atomic<float> eqMid    { 1.0f };  // linear
    std::atomic<float> eqLow    { 1.0f };  // linear
    std::atomic<float> filter   { 0.0f };  // bipolar -1 … +1
    std::atomic<float> fader    { 1.0f };  // linear  0 … 1
    std::atomic<bool>  killHigh { false };
    std::atomic<bool>  killMid  { false };
    std::atomic<bool>  killLow  { false };
    std::atomic<bool>  assignA  { false };
    std::atomic<bool>  assignB  { false };

    // Non-copyable / non-movable (std::atomic is neither).
    ChannelAtomicParams() = default;
    ChannelAtomicParams (const ChannelAtomicParams&) = delete;
    ChannelAtomicParams& operator= (const ChannelAtomicParams&) = delete;
};

//==============================================================================
// Full mixer atomic snapshot.
// Channel layout: index 0 = A, 1 = B, 2 = C, 3 = D.
//==============================================================================
struct MixerAtomicSnapshot
{
    ChannelAtomicParams channels[4];
    std::atomic<float>  crossfader      { 0.5f };  // [0, 1]
    std::atomic<int>    crossfaderCurve { 0    };  // PRD-0057: CrossfaderCurve enum (0=Smooth, 1=Sharp)
    std::atomic<float>  masterGain      { 1.0f };  // linear (converted from dB)

    MixerAtomicSnapshot() noexcept
    {
        // Per-channel A/B defaults: A → assignA=true, B → assignB=true,
        //                            C → assignA=true, D → assignB=true.
        channels[0].assignA.store (true,  std::memory_order_relaxed);
        channels[0].assignB.store (false, std::memory_order_relaxed);
        channels[1].assignA.store (false, std::memory_order_relaxed);
        channels[1].assignB.store (true,  std::memory_order_relaxed);
        channels[2].assignA.store (true,  std::memory_order_relaxed);
        channels[2].assignB.store (false, std::memory_order_relaxed);
        channels[3].assignA.store (false, std::memory_order_relaxed);
        channels[3].assignB.store (true,  std::memory_order_relaxed);
    }

    MixerAtomicSnapshot (const MixerAtomicSnapshot&) = delete;
    MixerAtomicSnapshot& operator= (const MixerAtomicSnapshot&) = delete;

    ChannelAtomicParams&       getChannel (int idx)       noexcept { return channels[idx]; }
    const ChannelAtomicParams& getChannel (int idx) const noexcept { return channels[idx]; }
};
