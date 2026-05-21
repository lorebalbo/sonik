#pragma once
//==============================================================================
// PRD-0059 / PRD-0058: MixLevelMeter — stereo L/R level meter atom.
//
// Reads from a ChannelMeterSlots block (one per channel + one per master,
// populated on the audio thread by PRD-0058). All cross-thread reads use
// std::memory_order_relaxed atomic loads — no locks, no allocation, no
// I/O on the audio thread.
//
// Visual (DESIGN.md compliance):
//   • Two vertical stereo bars, monochrome.
//   • Dithered checkerboard fill density rises with level (per DESIGN.md
//     §2 "Dithered Gradients"). No green/yellow/red, no smooth gradients.
//   • A 1-px solid ink line on top of each bar marks peak-hold.
//   • A clip indicator (small filled ink square in the meter's top-left)
//     latches with the snapshot's clip atomic. Click = clear.
//   • 2-px ink border around the meter chassis. Zero border-radius.
//
// Polling cadence:
//   • juce::Timer at 60 Hz (PRD-0059 §1.5.4 — well below audio rate,
//     matches display refresh).
//   • Timer started in parentHierarchyChanged() once the meter has a
//     visible peer; stopped on removal. Off-screen meters do no work.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

struct ChannelMeterSlots;

class MixLevelMeter final : public juce::Component,
                              private juce::Timer
{
public:
    MixLevelMeter (ChannelMeterSlots& slotsIn,
                   juce::String       chassisLabel = {});

    ~MixLevelMeter() override;

    // Test hook: pull fresh values from the snapshot and repaint without
    // depending on the message-thread timer tick. Safe to call at any time.
    void pollNow();

    // Manually clear the latched clip indicator (also bound to mouseUp).
    void clearClip();

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

    static constexpr int kMinimumWidth  = 24;   // PRD §1.4
    static constexpr int kMinimumHeight = 80;

    // Visual / decay constants (matched to PRD-0058 metering ballistics).
    static constexpr int   kPollHz             = 60;
    static constexpr float kPeakHoldDecayDbPerSec = 12.0f; // ≈ 1.5 s drop

    // Range: -60 dB (bar empty) to 0 dB (bar full).
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb =   0.0f;

private:
    void timerCallback() override;

    static float linearToDb (float linear) noexcept;
    static float dbToFillFraction (float db) noexcept;

    void paintMeterBar (juce::Graphics& g,
                        juce::Rectangle<int> area,
                        float                fill,
                        float                peakHoldFill) const;

    ChannelMeterSlots& slots;
    juce::String       chassisLabel;

    // Cached values polled from the snapshot.
    float cachedPeakL     = 0.0f;
    float cachedPeakR     = 0.0f;
    float cachedPeakHoldL = 0.0f;
    float cachedPeakHoldR = 0.0f;
    bool  cachedClip      = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixLevelMeter)
};
