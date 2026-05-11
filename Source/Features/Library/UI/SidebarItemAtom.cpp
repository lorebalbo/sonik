#include "SidebarItemAtom.h"
#include "LibraryPalette.h"

void SidebarItemAtom::paint (juce::Graphics& g)
{
    const auto bg   = isActive ? LibraryPalette::primary() : LibraryPalette::surface();
    const auto text = isActive ? LibraryPalette::surface() : LibraryPalette::primary();

    g.fillAll (bg);

    auto b = getLocalBounds();
    b.removeFromLeft (isIndented ? 20 : 6);

    g.setColour (text);
    g.setFont (LibraryPalette::boldLabelFont());

    auto countArea = b.removeFromRight (secondaryLabel.isNotEmpty() ? 42 : 0);
    g.drawText (label.toUpperCase(), b, juce::Justification::centredLeft, true);

    if (secondaryLabel.isNotEmpty())
    {
        g.setFont (LibraryPalette::bodyFont (10.0f));
        g.drawText (secondaryLabel, countArea.reduced (4, 0), juce::Justification::centredRight, true);
    }
}

void SidebarItemAtom::mouseUp (const juce::MouseEvent& e)
{
    if (!contains (e.position.toInt()))
        return;

    if (e.mods.isRightButtonDown())
    {
        auto callback = onRightClick;
        if (callback)
            callback (e.getScreenPosition());
        return;
    }

    if (e.getNumberOfClicks() > 1)
    {
        auto callback = onDoubleClick;
        if (callback)
            callback();
        return;
    }

    auto callback = onClick;
    if (callback)
        callback();
}
