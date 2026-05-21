#include "ChannelStripEqSection.h"
#include "../../State/MixerIdentifiers.h"

MixRotaryKnob::Config ChannelStripEqSection::makeEqConfig (const juce::String& label)
{
    MixRotaryKnob::Config cfg;
    cfg.label        = label;
    cfg.taper        = MixRotaryKnob::Normalisation::DbTapered;
    cfg.minValue     = -60.0f;
    cfg.maxValue     =  12.0f;
    cfg.defaultValue =   0.0f;
    cfg.wheelIncrement = 0.5f;
    cfg.showBottomLabel = true;  // Name text below circle, no dB display
    return cfg;
}

ChannelStripEqSection::ChannelStripEqSection (juce::ValueTree channelEqTree)
    : eqTree (channelEqTree),
      knobHigh (eqTree, MixerIDs::high, makeEqConfig ("HIGH")),
      knobMid  (eqTree, MixerIDs::mid,  makeEqConfig ("MID")),
      knobLow  (eqTree, MixerIDs::low,  makeEqConfig ("LOW")),
      killHigh (eqTree, MixerIDs::killHigh, "K"),
      killMid  (eqTree, MixerIDs::killMid,  "K"),
      killLow  (eqTree, MixerIDs::killLow,  "K")
{
    setOpaque (false);

    // The knobs paint the "KILL" indicator when their bound kill bool is true.
    knobHigh.setKillIndicatorBinding (MixerIDs::killHigh);
    knobMid .setKillIndicatorBinding (MixerIDs::killMid);
    knobLow .setKillIndicatorBinding (MixerIDs::killLow);

    // Kill buttons render as plain circles (no letter).
    killHigh.setCircleStyle (true);
    killMid .setCircleStyle (true);
    killLow .setCircleStyle (true);

    addAndMakeVisible (knobHigh);
    addAndMakeVisible (knobMid);
    addAndMakeVisible (knobLow);
    addAndMakeVisible (killHigh);
    addAndMakeVisible (killMid);
    addAndMakeVisible (killLow);
}

void ChannelStripEqSection::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFFFDFDFD));
}

void ChannelStripEqSection::resized()
{
    auto bounds = getLocalBounds();
    if (bounds.isEmpty()) return;

    // Three stacked rows: HIGH / MID / LOW (top to bottom). Each row gets
    // the same height; the knob fills the row and the kill button is
    // overlaid in the top-right corner so the rotary stays the primary
    // gesture target.
    const int rowH = juce::jmax (1, bounds.getHeight() / 3);

    // Kill circle size — square, sits in the right end of the label band.
    constexpr int kCircleKillSz = 12;

    auto layoutRow = [&] (juce::Rectangle<int> row,
                          MixRotaryKnob& knob, MixKillButton& kill)
    {
        knob.setBounds (row.reduced (1, 0));

        // Place the kill circle at the right end of the knob's label band.
        const int kY = row.getBottom() - MixRotaryKnob::kLabelBandH
                       + (MixRotaryKnob::kLabelBandH - kCircleKillSz) / 2;
        const int kX = row.getRight() - kCircleKillSz - 2;
        kill.setBounds (kX, kY, kCircleKillSz, kCircleKillSz);
        kill.toFront (false);
    };

    layoutRow (bounds.removeFromTop (rowH),     knobHigh, killHigh);
    layoutRow (bounds.removeFromTop (rowH),     knobMid,  killMid);
    layoutRow (bounds,                          knobLow,  killLow);
}
