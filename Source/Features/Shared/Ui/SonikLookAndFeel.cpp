#include "SonikLookAndFeel.h"

namespace sonik::ui
{
namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D }; // DESIGN.md primary
    const juce::Colour kSurface { 0xFFFDFDFD }; // DESIGN.md surface
    const juce::Colour kDimmed  { 0xFFE2E2E2 }; // surface-container-highest (disabled text)

    constexpr int kShadowOffset = 2;  // dithered drop: 2px offset, zero blur
    constexpr int kBorderPx     = 2;  // mandatory 2px ink border
    constexpr int kTickColumn   = 14; // left column reserved for the tick block
    constexpr int kItemHeight   = 24;
}

SonikLookAndFeel::SonikLookAndFeel()
{
    // DESIGN.md names Space Mono; fall back to the platform monospaced face so
    // the terminal look survives on machines without the font installed.
    fontFamily_ = juce::Font::getDefaultMonospacedFontName();
    for (const auto& name : juce::Font::findAllTypefaceNames())
    {
        if (name == "Space Mono")
        {
            fontFamily_ = name;
            break;
        }
    }

    setColour (juce::PopupMenu::backgroundColourId,            kSurface);
    setColour (juce::PopupMenu::textColourId,                  kInk);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kInk);
    setColour (juce::PopupMenu::highlightedTextColourId,       kSurface);
    setColour (juce::PopupMenu::headerTextColourId,            kInk);

    setColour (juce::ComboBox::backgroundColourId, kSurface);
    setColour (juce::ComboBox::textColourId,       kInk);
    setColour (juce::ComboBox::outlineColourId,    kInk);
    setColour (juce::ComboBox::arrowColourId,      kInk);
    setColour (juce::ComboBox::buttonColourId,     kSurface);
}

juce::Font SonikLookAndFeel::monoFont (float height, bool bold) const
{
    return juce::Font (juce::FontOptions (fontFamily_, height,
                                          bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
// PopupMenu
//==============================================================================
void SonikLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (kSurface);

    // 2px ink border on the menu panel; the right/bottom strips are reserved
    // for the dithered drop shadow.
    const juce::Rectangle<int> panel (0, 0, width - kShadowOffset, height - kShadowOffset);
    g.setColour (kInk);
    g.drawRect (panel, kBorderPx);

    // JUCE owns the popup window chrome, so painting outside the window bounds
    // is not possible. The dithered 2px-offset shadow (DESIGN.md §4: 50%
    // checkerboard, zero blur) is painted as an internal right/bottom band.
    for (int y = kShadowOffset; y < height; ++y)
        for (int x = width - kShadowOffset; x < width; ++x)
            if (((x + y) & 1) == 0)
                g.fillRect (x, y, 1, 1);

    for (int y = height - kShadowOffset; y < height; ++y)
        for (int x = kShadowOffset; x < width - kShadowOffset; ++x)
            if (((x + y) & 1) == 0)
                g.fillRect (x, y, 1, 1);
}

void SonikLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool hasSubMenu, const juce::String& text,
                                          const juce::String& shortcutKeyText,
                                          const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused (icon, textColour);

    if (isSeparator)
    {
        // Dotted 1px rule instead of a solid divider (DESIGN.md "no-line" rule).
        g.setColour (kInk);
        const auto y = area.getCentreY();
        for (int x = area.getX() + 8; x < area.getRight() - 8; x += 2)
            g.fillRect (x, y, 1, 1);
        return;
    }

    // Full fill inversion on the highlighted row — instant, no fades.
    const bool inverted = isActive && isHighlighted;
    g.setColour (inverted ? kInk : kSurface);
    g.fillRect (area);

    const auto foreground = ! isActive ? kDimmed
                                       : (inverted ? kSurface : kInk);
    g.setColour (foreground);
    g.setFont (getPopupMenuFont());

    auto textArea = area.reduced (8, 0);

    // Tick: a solid pixel block, not a vector checkmark.
    const auto tickArea = textArea.removeFromLeft (kTickColumn);
    if (isTicked)
        g.fillRect (tickArea.withSizeKeepingCentre (5, 5));

    if (hasSubMenu)
        g.drawText (">", textArea.removeFromRight (14), juce::Justification::centred);

    if (shortcutKeyText.isNotEmpty())
        g.drawText (shortcutKeyText, textArea,
                    juce::Justification::centredRight, true);

    g.drawText (text, textArea, juce::Justification::centredLeft, true);
}

void SonikLookAndFeel::drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height,
                                                 bool isScrollUpArrow)
{
    // The stock implementation fades the arrow zone with a gradient; DESIGN.md
    // forbids gradients, so draw a flat strip with a pixel-stepped chevron.
    g.fillAll (kSurface);

    const int cx  = width / 2;
    const int cy  = height / 2 - 3;
    const int dir = isScrollUpArrow ? 1 : -1;
    const int top = isScrollUpArrow ? cy + 4 : cy;

    g.setColour (kInk);
    g.fillRect (cx - 1, top,           2, 2);
    g.fillRect (cx - 2, top + 2 * dir, 4, 2);
    g.fillRect (cx - 3, top + 4 * dir, 6, 2);
}

void SonikLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                                  int standardMenuItemHeight,
                                                  int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth  = 50;
        idealHeight = 9;
        return;
    }

    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : kItemHeight;

    const auto textWidth = juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text);
    idealWidth = juce::roundToInt (textWidth) + kTickColumn + 16 + 14;
}

juce::Font SonikLookAndFeel::getPopupMenuFont()
{
    return monoFont (12.0f);
}

int SonikLookAndFeel::getPopupMenuBorderSize()
{
    // 2px ink border + 2px breathing room; also clears the shadow band on the
    // right/bottom edges.
    return kBorderPx + kShadowOffset;
}

int SonikLookAndFeel::getMenuWindowFlags()
{
    return 0; // no ComponentPeer::windowHasDropShadow — zero-blur rule
}

//==============================================================================
// ComboBox
//==============================================================================
void SonikLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                     int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<int> r (0, 0, width, height);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRect (r);

    g.setColour (box.findColour (juce::ComboBox::outlineColourId)
                    .withAlpha (box.isEnabled() ? 1.0f : 0.35f));
    g.drawRect (r, kBorderPx);

    // Pixel-stepped down chevron (no anti-aliased vector icon).
    const int cx = width - 12;
    const int cy = height / 2 - 3;
    g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                    .withAlpha (box.isEnabled() ? 1.0f : 0.35f));
    g.fillRect (cx - 3, cy,     6, 2);
    g.fillRect (cx - 2, cy + 2, 4, 2);
    g.fillRect (cx - 1, cy + 4, 2, 2);
}

juce::Font SonikLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return monoFont (12.0f);
}

void SonikLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 2, box.getWidth() - 24, box.getHeight() - 4);
    label.setFont (getComboBoxFont (box));
    label.setColour (juce::Label::textColourId,
                     box.findColour (juce::ComboBox::textColourId));
}
} // namespace sonik::ui
