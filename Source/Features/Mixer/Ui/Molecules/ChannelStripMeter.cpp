#include "ChannelStripMeter.h"

ChannelStripMeter::ChannelStripMeter (ChannelMeterSlots& slots,
                                       juce::String      chassisLabel)
    : meter (slots, std::move (chassisLabel))
{
    setOpaque (false);
    addAndMakeVisible (meter);
}

void ChannelStripMeter::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFFFDFDFD));
}

void ChannelStripMeter::resized()
{
    meter.setBounds (getLocalBounds());
}
