#include "RatingAtom.h"
#include "LibraryPalette.h"

// static
void RatingAtom::drawGlyphs (juce::Graphics& g,
                               juce::Rectangle<int> bounds,
                               int rating,
                               bool inverted)
{
    const auto filled  = inverted ? LibraryPalette::surface() : LibraryPalette::primary();
    const auto outline = inverted ? LibraryPalette::surface() : LibraryPalette::primary();

    const int startX = bounds.getX() + (bounds.getWidth()  - kTotalWidth) / 2;
    const int startY = bounds.getY() + (bounds.getHeight() - kGlyphSize)  / 2;

    for (int i = 0; i < 5; ++i)
    {
        juce::Rectangle<int> r (startX + i * (kGlyphSize + kGlyphGap), startY,
                                 kGlyphSize, kGlyphSize);
        if (i < rating)
        {
            g.setColour (filled);
            g.fillRect (r);
        }
        else
        {
            g.setColour (outline);
            g.drawRect (r, 1);
        }
    }
}

void RatingAtom::paint (juce::Graphics& g)
{
    drawGlyphs (g, getLocalBounds(), rating, isRowInverted);
}

void RatingAtom::mouseUp (const juce::MouseEvent& e)
{
    if (!onRatingChanged)
        return;

    const int startX  = (getWidth() - kTotalWidth) / 2;
    const int relX    = e.x - startX;
    const int idx     = relX / (kGlyphSize + kGlyphGap);

    if (idx < 0 || idx >= 5)
        return;

    const int newRating = (idx + 1 == rating) ? 0 : (idx + 1);
    rating = newRating;
    repaint();
    onRatingChanged (newRating);
}
