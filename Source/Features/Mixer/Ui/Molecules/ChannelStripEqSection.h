#pragma once
//==============================================================================
// PRD-0060: ChannelStripEqSection — horizontal row of three EQ knob + kill
// pairs (HIGH, MID, LOW) for a single channel.
//
// Composes PRD-0059 atoms: MixRotaryKnob (taper = DbTapered, range -60..+6 dB)
// and MixKillButton. Each column stacks knob over kill button.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Atoms/MixRotaryKnob.h"
#include "../Atoms/MixKillButton.h"

class ChannelStripEqSection final : public juce::Component
{
public:
    /// @param channelEqTree   the "eq" sub-tree under mixer.channel.{A,B,C,D}
    explicit ChannelStripEqSection (juce::ValueTree channelEqTree);
    ~ChannelStripEqSection() override = default;

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Testing accessors.
    MixRotaryKnob& getKnobHigh()  noexcept { return knobHigh; }
    MixRotaryKnob& getKnobMid()   noexcept { return knobMid; }
    MixRotaryKnob& getKnobLow()   noexcept { return knobLow; }
    MixKillButton& getKillHigh()  noexcept { return killHigh; }
    MixKillButton& getKillMid()   noexcept { return killMid; }
    MixKillButton& getKillLow()   noexcept { return killLow; }

    // Vertical layout: three stacked rows (HIGH / MID / LOW). Each row hosts
    // a square-ish knob with a small kill button overlaid in the top-right
    // corner so the rotary remains the dominant gesture target.
    static constexpr int kKillH   = 10;
    static constexpr int kKillW   = 14;
    static constexpr int kEqRowH = 50;

private:
    static MixRotaryKnob::Config makeEqConfig (const juce::String& label);

    juce::ValueTree   eqTree;
    MixRotaryKnob     knobHigh;
    MixRotaryKnob     knobMid;
    MixRotaryKnob     knobLow;
    MixKillButton     killHigh;
    MixKillButton     killMid;
    MixKillButton     killLow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStripEqSection)
};
