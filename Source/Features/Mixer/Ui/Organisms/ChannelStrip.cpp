#include "ChannelStrip.h"

#include "../../State/MixerStateSchema.h"
#include "../../State/MixerMeterSnapshot.h"
#include "../../State/MixerIdentifiers.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };

    constexpr int kLetterCapH = 16;
    constexpr int kAssignsH   = 22;   // height for the A/B assign-button row
    constexpr int kKnobMinH   = 28;   // minimum height for a compact knob cell
}

MixRotaryKnob::Config ChannelStrip::makeGainConfig()
{
    MixRotaryKnob::Config cfg;
    cfg.label          = "GAIN";
    cfg.taper          = MixRotaryKnob::Normalisation::DbTapered;
    cfg.minValue       = -60.0f;
    cfg.maxValue       =  12.0f;
    cfg.defaultValue   =   0.0f;
    cfg.wheelIncrement =   0.5f;
    cfg.showBottomLabel = true;
    return cfg;
}

MixRotaryKnob::Config ChannelStrip::makeFilterConfig()
{
    MixRotaryKnob::Config cfg;
    cfg.label            = "FILTER";
    cfg.taper            = MixRotaryKnob::Normalisation::Bipolar;
    cfg.minValue         = -1.0f;
    cfg.maxValue         =  1.0f;
    cfg.defaultValue     =  0.0f;
    cfg.wheelIncrement   = 0.02f;
    cfg.bipolarDeadzone  = 0.02f;
    cfg.showBottomLabel  = true;
    return cfg;
}

MixFader::Config ChannelStrip::makeVolumeFaderConfig()
{
    MixFader::Config cfg;
    cfg.orientation  = MixFader::Orientation::Vertical;
    cfg.minValue     = 0.0f;
    cfg.maxValue     = 1.0f;
    cfg.defaultValue = 1.0f;
    cfg.detentValue.reset();
    cfg.invertVertical = false;
    return cfg;
}

ChannelStrip::ChannelStrip (MixerStateSchema& schema,
                              MixerMeterSnapshot& meters,
                              int channelIndexIn)
    : channelIndex (channelIndexIn),
      channelLetter (juce::String::charToString (kChannelLetters[juce::jlimit (0, 3, channelIndex)])),
      gainKnob   (schema.getChannelTree (channelIndex), MixerIDs::gain, makeGainConfig()),
      eqSection  (schema.getChannelEqTree (channelIndex)),
      filterKnob (schema.getChannelTree (channelIndex), MixerIDs::filter, makeFilterConfig()),
      meterMolecule (meters.channels[juce::jlimit (0, 3, channelIndex)], channelLetter),
      assignA    (schema.getChannelTree (channelIndex), MixerIDs::assignA, "A"),
      assignB    (schema.getChannelTree (channelIndex), MixerIDs::assignB, "B"),
      volumeFader (schema.getChannelTree (channelIndex), MixerIDs::fader, makeVolumeFaderConfig())
{
    setOpaque (false);

    addAndMakeVisible (gainKnob);
    addAndMakeVisible (eqSection);
    addAndMakeVisible (filterKnob);
    addAndMakeVisible (meterMolecule);
    addAndMakeVisible (assignA);
    addAndMakeVisible (assignB);
    addAndMakeVisible (volumeFader);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour (kSurface);
    g.fillRect (bounds);
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Channel letter cap.
    auto cap = bounds.reduced (2).removeFromTop (kLetterCapH);
    g.setColour (kInk);
    g.fillRect (cap);
    g.setColour (kSurface);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  11.0f, juce::Font::plain));
    g.drawText (channelLetter, cap, juce::Justification::centred);
}

void ChannelStrip::resized()
{
    auto bounds = getLocalBounds().reduced (1);
    if (bounds.isEmpty()) return;

    // Even channel index (A, C) = left deck; odd (B, D) = right deck.
    // The gain column always sits on the inward side (closer to the deck).
    const bool isLeft = (channelIndex % 2 == 0);

    bounds.removeFromTop (kLetterCapH);   // painted in paint()

    // Split into two equal columns.
    const int halfW  = bounds.getWidth() / 2;
    auto leftCol     = bounds.removeFromLeft (halfW);
    auto rightCol    = bounds;

    auto& gainCol = isLeft ? leftCol  : rightCol;
    auto& eqCol   = isLeft ? rightCol : leftCol;

    // ── Gain column: GAIN → FILTER → A/B assigns → Level meter ──────────────
    const int colH    = gainCol.getHeight();
    const int gainH   = juce::jmax (kKnobMinH, (colH * 22) / 100);
    const int filterH = juce::jmax (kKnobMinH, (colH * 22) / 100);
    const int assignH = kAssignsH;
    const int meterH  = colH - gainH - filterH - assignH;

    gainKnob.setBounds   (gainCol.removeFromTop (gainH).reduced (2));
    filterKnob.setBounds (gainCol.removeFromTop (filterH).reduced (2));

    auto assignsRow = gainCol.removeFromTop (assignH).reduced (2, 0);
    const int halfAssignW = assignsRow.getWidth() / 2;
    assignA.setBounds (assignsRow.removeFromLeft (halfAssignW).reduced (1));
    assignB.setBounds (assignsRow.reduced (1));

    if (meterH >= 4)
        meterMolecule.setBounds (gainCol.reduced (2));
    else
        meterMolecule.setBounds ({});

    // ── EQ column: HIGH / MID / LOW (compact) → Volume fader ────────────────
    const int eqColH   = eqCol.getHeight();
    const int totalEqH = juce::jmax (3 * kKnobMinH, (eqColH * 60) / 100);
    const int faderH   = eqColH - totalEqH;

    eqSection.setBounds (eqCol.removeFromTop (totalEqH).reduced (1));

    if (faderH >= 8)
        volumeFader.setBounds (eqCol.reduced (2));
    else
        volumeFader.setBounds ({});
}
