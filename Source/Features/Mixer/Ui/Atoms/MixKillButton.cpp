#include "MixKillButton.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
}

MixKillButton::MixKillButton (juce::ValueTree boundTree,
                               juce::Identifier propertyIdIn,
                               juce::String     labelText)
    : tree (boundTree),
      propertyId (propertyIdIn),
      label (std::move (labelText))
{
    setOpaque (false);
    readFromTree();
    tree.addListener (this);
}

MixKillButton::~MixKillButton()
{
    tree.removeListener (this);
}

void MixKillButton::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive)
        return;
    active = shouldBeActive;
    writeToTree();
    repaint();
}

void MixKillButton::toggle()
{
    setActive (! active);
}

void MixKillButton::paintLatchedButton (juce::Graphics& g,
                                         bool isActiveNow,
                                         const juce::String& text) const
{
    const auto bounds = getLocalBounds();

    // Fill (full inversion on active state — DESIGN.md §5).
    g.setColour (isActiveNow ? kInk : kSurface);
    g.fillRect (bounds);

    // 2-px solid border, zero radius.
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Centred Space Mono Regular label.
    g.setColour (isActiveNow ? kSurface : kInk);
    const float fontHeight = juce::jlimit (8.0f, 12.0f,
                                            static_cast<float> (bounds.getHeight()) * 0.55f);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  fontHeight, juce::Font::plain));
    g.drawText (text, bounds, juce::Justification::centred, false);
}

void MixKillButton::paint (juce::Graphics& g)
{
    if (circleStyle)
    {
        // Simple circle: filled = active, outline = inactive.
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const float size  = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto circle = bounds.withSizeKeepingCentre (size, size);

        if (active)
        {
            g.setColour (kInk);
            g.fillEllipse (circle);
        }
        else
        {
            g.setColour (kSurface);
            g.fillEllipse (circle);
            g.setColour (kInk);
            g.drawEllipse (circle, 1.5f);
        }
        return;
    }
    paintLatchedButton (g, active, label);
}

void MixKillButton::mouseUp (const juce::MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition()))
        return;
    toggle();
}

void MixKillButton::readFromTree()
{
    if (! tree.isValid())
        return;
    active = static_cast<bool> (tree.getProperty (propertyId, false));
    repaint();
}

void MixKillButton::writeToTree()
{
    if (! tree.isValid())
        return;
    tree.setProperty (propertyId, active, nullptr);
}

void MixKillButton::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                const juce::Identifier& property)
{
    if (changedTree != tree || property != propertyId)
        return;
    readFromTree();
}
