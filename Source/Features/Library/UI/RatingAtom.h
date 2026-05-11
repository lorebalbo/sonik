#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

/// Atom: 5 pixel-art square glyphs representing a 0-5 star rating.
/// Filled squares (i < rating) and empty-outline squares (i >= rating).
/// When isRowInverted = true, both filled and outline colours invert to #f9f9f9.
/// Click position determines which star is toggled; clicking the same star
/// resets the rating to 0.
class RatingAtom final : public juce::Component
{
public:
    int  rating        = 0;    ///< 0 = no stars, 5 = all stars
    bool isRowInverted = false;
    std::function<void (int newRating)> onRatingChanged;

    RatingAtom() = default;

    void paint  (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;

    // Geometry constants
    static constexpr int kGlyphSize  = 4;
    static constexpr int kGlyphGap   = 2;
    static constexpr int kTotalWidth = 5 * kGlyphSize + 4 * kGlyphGap; // 28 px

    /// Draw the 5 glyphs into an arbitrary bounds rectangle (used by
    /// TrackTableMolecule::paintCell as well).
    static void drawGlyphs (juce::Graphics& g,
                             juce::Rectangle<int> bounds,
                             int rating,
                             bool inverted);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RatingAtom)
};
