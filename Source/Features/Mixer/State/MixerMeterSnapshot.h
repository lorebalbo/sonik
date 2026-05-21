#pragma once
//==============================================================================
// PRD-0052: MixerMeterSnapshot — lock-free metering snapshot.
//
// The audio thread writes metering values to this struct (using
// std::atomic::store(std::memory_order_relaxed)) after the channel-strip
// DSP stage. UI meter components poll this struct via a juce::Timer at
// 30–60 Hz (PRD-0058) using std::atomic::load(std::memory_order_relaxed).
//
// Metering values do NOT live in the juce::ValueTree — this avoids listener
// storms on the message thread and eliminates the need for the audio thread
// to marshal values through the ValueTree (which requires a lock).
//
// Identifier mapping (dotted path → struct field):
//   "mixer.channel.A.levelPeakL"   → channels[0].levelPeakL
//   "mixer.channel.A.levelPeakR"   → channels[0].levelPeakR
//   "mixer.channel.A.levelRmsL"    → channels[0].levelRmsL
//   "mixer.channel.A.levelRmsR"    → channels[0].levelRmsR
//   "mixer.channel.A.clip"         → channels[0].clip
//   (analogous for B=1, C=2, D=3)
//   "mixer.master.levelPeakL"      → master.levelPeakL
//   "mixer.master.clip"            → master.clip
//   (etc.)
//
// Units: levelPeak* and levelRms* are linear amplitude [0, +inf).
//        clip is a latched bool: true if any sample ≥ 0 dBFS at the
//        channel or master output. Reset by the UI on user click
//        (PRD-0058 defines the exact reset mechanism).
//
// NOTE: PRD-0058 amends this file to add levelPeakHoldL / levelPeakHoldR
//       slots per channel and per master. Those slots are declared here in
//       advance to avoid a breaking change at PRD-0058 implementation time.
//==============================================================================

#include <atomic>

//==============================================================================
// Per-channel / per-master meter slots.
//==============================================================================
struct ChannelMeterSlots
{
    std::atomic<float> levelPeakL     { 0.0f };
    std::atomic<float> levelPeakR     { 0.0f };
    std::atomic<float> levelPeakHoldL { 0.0f };  // amended by PRD-0058
    std::atomic<float> levelPeakHoldR { 0.0f };  // amended by PRD-0058
    std::atomic<float> levelRmsL      { 0.0f };
    std::atomic<float> levelRmsR      { 0.0f };
    std::atomic<bool>  clip           { false };

    ChannelMeterSlots() = default;
    ChannelMeterSlots (const ChannelMeterSlots&) = delete;
    ChannelMeterSlots& operator= (const ChannelMeterSlots&) = delete;

    void resetToDefaults() noexcept
    {
        levelPeakL    .store (0.0f,  std::memory_order_relaxed);
        levelPeakR    .store (0.0f,  std::memory_order_relaxed);
        levelPeakHoldL.store (0.0f,  std::memory_order_relaxed);
        levelPeakHoldR.store (0.0f,  std::memory_order_relaxed);
        levelRmsL     .store (0.0f,  std::memory_order_relaxed);
        levelRmsR     .store (0.0f,  std::memory_order_relaxed);
        clip          .store (false, std::memory_order_relaxed);
    }

    /// PRD-0058: clear the latched clip indicator. Safe to call from the
    /// message thread (UI click handler — owned by PRD-0059). The audio
    /// thread observes the cleared flag on its next block and resets its
    /// internal sample-since-clip counter so the latch does not immediately
    /// re-fire from stale state.
    void clearClip() noexcept
    {
        clip.store (false, std::memory_order_relaxed);
    }
};

//==============================================================================
// Full mixer meter snapshot.
// Channel layout: index 0 = A, 1 = B, 2 = C, 3 = D.
//==============================================================================
struct MixerMeterSnapshot
{
    ChannelMeterSlots channels[4];
    ChannelMeterSlots master;

    MixerMeterSnapshot() = default;
    MixerMeterSnapshot (const MixerMeterSnapshot&) = delete;
    MixerMeterSnapshot& operator= (const MixerMeterSnapshot&) = delete;

    ChannelMeterSlots&       getChannel (int idx)       noexcept { return channels[idx]; }
    const ChannelMeterSlots& getChannel (int idx) const noexcept { return channels[idx]; }

    void resetChannel (int idx) noexcept
    {
        channels[idx].resetToDefaults();
    }

    void resetAll() noexcept
    {
        for (auto& ch : channels)
            ch.resetToDefaults();
        master.resetToDefaults();
    }

    /// PRD-0058: convenience helper to clear the clip latch on either a
    /// specific channel (idx 0..3) or the master (idx == 4).
    void clearChannelClip (int idx) noexcept
    {
        if (idx >= 0 && idx < 4)
            channels[idx].clearClip();
    }

    void clearMasterClip() noexcept
    {
        master.clearClip();
    }
};
