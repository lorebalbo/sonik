#include "CrossfaderRail.h"
#include "../../State/MixerIdentifiers.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };

    MixFader::Config makeCrossfaderConfig()
    {
        MixFader::Config cfg;
        cfg.orientation    = MixFader::Orientation::Horizontal;
        cfg.minValue       = 0.0f;
        cfg.maxValue       = 1.0f;
        cfg.defaultValue   = 0.5f;
        cfg.detentValue    = 0.5f;
        cfg.detentDeadzone = 0.02f;
        return cfg;
    }

    constexpr int kCurveButtonWidth = 64;
}

CrossfaderRail::CrossfaderRail (juce::ValueTree mixerTreeIn)
    : mixerTree (mixerTreeIn),
      fader (mixerTree, MixerIDs::crossfader, makeCrossfaderConfig())
{
    setOpaque (false);
    addAndMakeVisible (fader);
    readCurveFromTree();
    mixerTree.addListener (this);
}

CrossfaderRail::~CrossfaderRail()
{
    mixerTree.removeListener (this);
}

void CrossfaderRail::setCurveSmooth (bool smooth)
{
    if (curveIsSmooth == smooth) return;
    curveIsSmooth = smooth;
    commitCurveToTree();
    repaint();
}

juce::Rectangle<int> CrossfaderRail::getCurveButtonArea() const
{
    auto bounds = getLocalBounds().reduced (4);
    return bounds.removeFromLeft (kCurveButtonWidth);
}

void CrossfaderRail::resized()
{
    // Curve buttons are hidden; fader spans the full available width.
    fader.setBounds (getLocalBounds().reduced (4));
}

void CrossfaderRail::paint (juce::Graphics& g)
{
    // Curve buttons are hidden; draw only the chassis border.
    const auto bounds = getLocalBounds();
    g.setColour (kSurface);
    g.fillRect (bounds);
    g.setColour (kInk);
    g.drawRect (bounds, 2);
}

void CrossfaderRail::mouseUp (const juce::MouseEvent&)
{
    // Curve buttons hidden — no click handling needed.
}

void CrossfaderRail::readCurveFromTree()
{
    if (! mixerTree.isValid()) return;
    const auto s = mixerTree.getProperty (MixerIDs::crossfaderCurve,
                                            juce::String ("smooth")).toString();
    curveIsSmooth = (s != "sharp");
}

void CrossfaderRail::commitCurveToTree()
{
    if (! mixerTree.isValid()) return;
    mixerTree.setProperty (MixerIDs::crossfaderCurve,
                            juce::String (curveIsSmooth ? "smooth" : "sharp"),
                            nullptr);
}

void CrossfaderRail::valueTreePropertyChanged (juce::ValueTree& tree,
                                                 const juce::Identifier& property)
{
    if (tree != mixerTree || property != MixerIDs::crossfaderCurve)
        return;
    const bool wasSmooth = curveIsSmooth;
    readCurveFromTree();
    if (wasSmooth != curveIsSmooth)
        repaint();
}
