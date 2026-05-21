#include "MasterSection.h"

#include "../../State/MixerStateSchema.h"
#include "../../State/MixerMeterSnapshot.h"
#include "../../State/MixerIdentifiers.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
    constexpr int kHeaderH = 16;
    constexpr int kKnobH   = 70;
}

MixRotaryKnob::Config MasterSection::makeMasterGainConfig()
{
    MixRotaryKnob::Config cfg;
    cfg.label          = "MASTER";
    cfg.taper          = MixRotaryKnob::Normalisation::DbTapered;
    cfg.minValue       = -60.0f;
    cfg.maxValue       =  12.0f;
    cfg.defaultValue   =   0.0f;
    cfg.wheelIncrement =   0.5f;
    return cfg;
}

MasterSection::MasterSection (MixerStateSchema& schema,
                                 MixerMeterSnapshot& meters)
    : masterGainKnob (schema.getMasterTree(), MixerIDs::gain, makeMasterGainConfig()),
      masterMeter    (meters.master, "MST")
{
    setOpaque (false);
    addAndMakeVisible (masterGainKnob);
    addAndMakeVisible (masterMeter);
}

void MasterSection::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour (kSurface);
    g.fillRect (bounds);
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    auto header = bounds.reduced (2).removeFromTop (kHeaderH);
    g.setColour (kInk);
    g.fillRect (header);
    g.setColour (kSurface);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                   11.0f, juce::Font::plain));
    g.drawText ("MASTER", header, juce::Justification::centred);
}

void MasterSection::resized()
{
    auto bounds = getLocalBounds().reduced (2);
    if (bounds.isEmpty()) return;
    bounds.removeFromTop (kHeaderH);
    auto knobArea = bounds.removeFromTop (kKnobH);
    masterGainKnob.setBounds (knobArea.reduced (2));
    masterMeter.setBounds (bounds.reduced (2));
}
