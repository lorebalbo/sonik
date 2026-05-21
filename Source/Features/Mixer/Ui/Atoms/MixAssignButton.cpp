#include "MixAssignButton.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
}

MixAssignButton::MixAssignButton (juce::ValueTree boundTree,
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

MixAssignButton::~MixAssignButton()
{
    tree.removeListener (this);
}

void MixAssignButton::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive)
        return;
    active = shouldBeActive;
    writeToTree();
    repaint();
}

void MixAssignButton::toggle()
{
    setActive (! active);
}

void MixAssignButton::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();

    g.setColour (active ? kInk : kSurface);
    g.fillRect (bounds);

    g.setColour (kInk);
    g.drawRect (bounds, 2);

    g.setColour (active ? kSurface : kInk);
    const float fontHeight = juce::jlimit (8.0f, 12.0f,
                                            static_cast<float> (bounds.getHeight()) * 0.55f);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  fontHeight, juce::Font::plain));
    g.drawText (label, bounds, juce::Justification::centred, false);
}

void MixAssignButton::mouseUp (const juce::MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition()))
        return;
    toggle();
}

void MixAssignButton::readFromTree()
{
    if (! tree.isValid())
        return;
    active = static_cast<bool> (tree.getProperty (propertyId, false));
    repaint();
}

void MixAssignButton::writeToTree()
{
    if (! tree.isValid())
        return;
    tree.setProperty (propertyId, active, nullptr);
}

void MixAssignButton::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                  const juce::Identifier& property)
{
    if (changedTree != tree || property != propertyId)
        return;
    readFromTree();
}
