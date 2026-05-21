#pragma once
//==============================================================================
// PRD-0060: ChannelStripMeter — wraps a single MixLevelMeter for a channel.
//
// A thin molecule: provides a chassis label ("L R") and forwards bounds /
// painting to the MixLevelMeter atom. Exists so the ChannelStrip layout
// code can address "the meter cell" as a single child, without recreating
// labeling code.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Atoms/MixLevelMeter.h"

struct ChannelMeterSlots;

class ChannelStripMeter final : public juce::Component
{
public:
    explicit ChannelStripMeter (ChannelMeterSlots& slots,
                                 juce::String      chassisLabel = {});
    ~ChannelStripMeter() override = default;

    void resized() override;
    void paint (juce::Graphics& g) override;

    MixLevelMeter& getMeter() noexcept { return meter; }

private:
    MixLevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStripMeter)
};
