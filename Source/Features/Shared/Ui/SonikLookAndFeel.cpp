#include "SonikLookAndFeel.h"
#include "SonikTheme.h"
#include "BinaryData.h"

namespace sonik::ui
{
namespace
{
    constexpr int kTickColumn = 14; // left column reserved for the tick block
    constexpr int kItemHeight = 24;

    constexpr int kTooltipMaxWidth = 360;
    constexpr int kTooltipPadX     = 8;
    constexpr int kTooltipPadY     = 6;
}

SonikLookAndFeel::SonikLookAndFeel()
{
    // DESIGN.md names Space Mono as the system's only family. The typeface is
    // embedded in the binary (SIL OFL — Assets/Fonts/OFL.txt) so typography is
    // identical on every machine instead of silently falling back to the
    // platform monospaced font.
    regularTypeface_ = juce::Typeface::createSystemTypefaceFor (
        BinaryData::SpaceMonoRegular_ttf,
        static_cast<size_t> (BinaryData::SpaceMonoRegular_ttfSize));
    boldTypeface_ = juce::Typeface::createSystemTypefaceFor (
        BinaryData::SpaceMonoBold_ttf,
        static_cast<size_t> (BinaryData::SpaceMonoBold_ttfSize));

    const auto ink     = theme::ink();
    const auto surface = theme::surface();

    setColour (juce::PopupMenu::backgroundColourId,            surface);
    setColour (juce::PopupMenu::textColourId,                  ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ink);
    setColour (juce::PopupMenu::highlightedTextColourId,       surface);
    setColour (juce::PopupMenu::headerTextColourId,            ink);

    setColour (juce::ComboBox::backgroundColourId, surface);
    setColour (juce::ComboBox::textColourId,       ink);
    setColour (juce::ComboBox::outlineColourId,    ink);
    setColour (juce::ComboBox::arrowColourId,      ink);
    setColour (juce::ComboBox::buttonColourId,     surface);

    setColour (juce::TextButton::buttonColourId,   surface);
    setColour (juce::TextButton::buttonOnColourId, ink);
    setColour (juce::TextButton::textColourOffId,  ink);
    setColour (juce::TextButton::textColourOnId,   surface);

    setColour (juce::Label::textColourId,            ink);
    setColour (juce::Label::backgroundColourId,      juce::Colours::transparentBlack);

    setColour (juce::TextEditor::backgroundColourId,      surface);
    setColour (juce::TextEditor::textColourId,            ink);
    setColour (juce::TextEditor::outlineColourId,         ink);
    setColour (juce::TextEditor::focusedOutlineColourId,  ink);
    setColour (juce::TextEditor::highlightColourId,       ink);
    setColour (juce::TextEditor::highlightedTextColourId, surface);
    setColour (juce::TextEditor::shadowColourId,          juce::Colours::transparentBlack);
    setColour (juce::CaretComponent::caretColourId,       ink);

    setColour (juce::ScrollBar::backgroundColourId, theme::containerLow());
    setColour (juce::ScrollBar::thumbColourId,      ink);
    setColour (juce::ScrollBar::trackColourId,      theme::containerLow());

    setColour (juce::TooltipWindow::backgroundColourId, surface);
    setColour (juce::TooltipWindow::textColourId,       ink);
    setColour (juce::TooltipWindow::outlineColourId,    ink);

    setColour (juce::AlertWindow::backgroundColourId, surface);
    setColour (juce::AlertWindow::textColourId,       ink);
    setColour (juce::AlertWindow::outlineColourId,    ink);

    setColour (juce::ListBox::backgroundColourId, surface);
    setColour (juce::ListBox::textColourId,       ink);
    setColour (juce::ListBox::outlineColourId,    ink);
}

juce::Font SonikLookAndFeel::monoFont (float height, bool bold) const
{
    return theme::mono (height, bold);
}

//==============================================================================
// Typography
//==============================================================================
juce::Typeface::Ptr SonikLookAndFeel::getTypefaceForFont (const juce::Font& font)
{
    // One family, app-wide: every font request — whatever name it carries —
    // resolves to the embedded Space Mono so no component can drift off the
    // design system's typography.
    if (regularTypeface_ != nullptr && boldTypeface_ != nullptr)
        return font.isBold() ? boldTypeface_ : regularTypeface_;

    return juce::LookAndFeel_V4::getTypefaceForFont (font);
}

//==============================================================================
// PopupMenu
//==============================================================================
void SonikLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    // Flat panel: surface fill + 2px ink border, no shadow of any kind.
    g.fillAll (theme::surface());
    g.setColour (theme::ink());
    g.drawRect (juce::Rectangle<int> (0, 0, width, height), theme::kBorderPx);
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
        g.setColour (theme::ink());
        const auto y = area.getCentreY();
        for (int x = area.getX() + 8; x < area.getRight() - 8; x += 2)
            g.fillRect (x, y, 1, 1);
        return;
    }

    // Full fill inversion on the highlighted row — instant, no fades.
    const bool inverted = isActive && isHighlighted;
    g.setColour (inverted ? theme::ink() : theme::surface());
    g.fillRect (area);

    const auto foreground = ! isActive ? theme::inkDisabled()
                                       : (inverted ? theme::surface() : theme::ink());
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
    g.fillAll (theme::surface());

    const int cx  = width / 2;
    const int cy  = height / 2 - 3;
    const int dir = isScrollUpArrow ? 1 : -1;
    const int top = isScrollUpArrow ? cy + 4 : cy;

    g.setColour (theme::ink());
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

    // Text + tick column + side padding only. No extra slack: dropdown menus
    // opened from a button match the button's width via withMinimumWidth, so
    // the measured width must stay as tight as the drawing allows.
    const auto textWidth = juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text);
    idealWidth = juce::roundToInt (textWidth) + kTickColumn + 16;
}

juce::Font SonikLookAndFeel::getPopupMenuFont()
{
    return theme::mono (theme::kFontMenu);
}

int SonikLookAndFeel::getPopupMenuBorderSize()
{
    return theme::kBorderPx + 2; // 2px ink border + 2px breathing room
}

int SonikLookAndFeel::getMenuWindowFlags()
{
    return 0; // no ComponentPeer::windowHasDropShadow — menus carry no shadow
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
                    .withAlpha (box.isEnabled() ? 1.0f : theme::kDisabledAlpha));
    g.drawRect (r, theme::kBorderPx);

    // Pixel-stepped down chevron (no anti-aliased vector icon).
    const int cx = width - 12;
    const int cy = height / 2 - 3;
    g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                    .withAlpha (box.isEnabled() ? 1.0f : theme::kDisabledAlpha));
    g.fillRect (cx - 3, cy,     6, 2);
    g.fillRect (cx - 2, cy + 2, 4, 2);
    g.fillRect (cx - 1, cy + 4, 2, 2);
}

juce::Font SonikLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return theme::mono (theme::kFontMenu);
}

void SonikLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 2, box.getWidth() - 24, box.getHeight() - 4);
    label.setFont (getComboBoxFont (box));
    label.setColour (juce::Label::textColourId,
                     box.findColour (juce::ComboBox::textColourId));
}

//==============================================================================
// TextButton
//==============================================================================
void SonikLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& /*backgroundColour*/,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    // The per-button background colour is deliberately ignored: every stock
    // button is the DESIGN.md tactile square. Active/down invert to ink;
    // hover steps the fill to the tonal scale — instantly, no fades.
    const auto bounds  = button.getLocalBounds();
    const bool enabled = button.isEnabled();
    const bool latched = button.getToggleState();

    juce::Colour fill = theme::surface();
    if (enabled && (shouldDrawButtonAsDown || latched))
        fill = theme::ink();
    else if (enabled && shouldDrawButtonAsHighlighted)
        fill = theme::containerHighest();

    g.setColour (fill);
    g.fillRect (bounds);

    g.setColour (theme::ink().withAlpha (enabled ? 1.0f : theme::kDisabledAlpha));
    g.drawRect (bounds, theme::kBorderPx);
}

void SonikLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                       bool /*shouldDrawButtonAsHighlighted*/,
                                       bool shouldDrawButtonAsDown)
{
    const bool enabled  = button.isEnabled();
    const bool inverted = enabled && (shouldDrawButtonAsDown || button.getToggleState());

    g.setColour ((inverted ? theme::surface() : theme::ink())
                     .withAlpha (enabled ? 1.0f : theme::kDisabledAlpha));
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.drawText (button.getButtonText(),
                button.getLocalBounds().reduced (theme::kBorderPx + 2, theme::kBorderPx),
                juce::Justification::centred, false);
}

juce::Font SonikLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::mono (juce::jlimit (9.0f, theme::kFontBody,
                                      static_cast<float> (buttonHeight) * 0.45f));
}

//==============================================================================
// ScrollBar
//==============================================================================
void SonikLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar,
                                      int x, int y, int width, int height,
                                      bool isScrollbarVertical,
                                      int thumbStartPosition, int thumbSize,
                                      bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (isMouseDown);

    // Flat recessed track on the tonal scale — no rounded thumb, no fades.
    g.setColour (theme::containerLow());
    g.fillRect (x, y, width, height);

    if (thumbSize <= 0 || ! bar.isEnabled())
        return;

    auto thumb = isScrollbarVertical
        ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
        : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height);

    // Solid ink block, inset so the track reads as a channel around it.
    g.setColour (theme::ink().withAlpha (isMouseOver ? 1.0f : 0.85f));
    g.fillRect (thumb.reduced (2));
}

bool SonikLookAndFeel::areScrollbarButtonsVisible()
{
    return false; // pixel chrome only — the thumb is the whole affordance
}

//==============================================================================
// Tooltip
//==============================================================================
juce::Rectangle<int> SonikLookAndFeel::getTooltipBounds (const juce::String& tipText,
                                                         juce::Point<int> screenPos,
                                                         juce::Rectangle<int> parentArea)
{
    const auto font = theme::mono (theme::kFontLabel);

    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.append (tipText, font, theme::ink());

    juce::TextLayout layout;
    layout.createLayout (s, static_cast<float> (kTooltipMaxWidth));

    const int w = juce::roundToInt (layout.getWidth())  + kTooltipPadX * 2 + theme::kBorderPx * 2;
    const int h = juce::roundToInt (layout.getHeight()) + kTooltipPadY * 2 + theme::kBorderPx * 2;

    // Below-right of the cursor, flipped to stay inside the parent area.
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12)
                                                                       : screenPos.x + 18,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)
                                                                       : screenPos.y + 16,
                                 w, h)
        .constrainedWithin (parentArea);
}

void SonikLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text,
                                    int width, int height)
{
    const juce::Rectangle<int> bounds (0, 0, width, height);

    // Flat surface panel + the mandatory 2px ink border. No shadow, no radius.
    g.fillAll (theme::surface());
    g.setColour (theme::ink());
    g.drawRect (bounds, theme::kBorderPx);

    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, theme::mono (theme::kFontLabel), theme::ink());

    juce::TextLayout layout;
    layout.createLayout (s, static_cast<float> (kTooltipMaxWidth));
    layout.draw (g, bounds.reduced (kTooltipPadX + theme::kBorderPx,
                                    kTooltipPadY + theme::kBorderPx)
                      .toFloat());
}

//==============================================================================
// AlertWindow
//==============================================================================
void SonikLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                                     const juce::Rectangle<int>& textArea,
                                     juce::TextLayout& textLayout)
{
    const auto bounds = alert.getLocalBounds();

    // Flat surface panel, 2px ink border, zero radius, no icon glyph.
    g.fillAll (theme::surface());
    g.setColour (theme::ink());
    g.drawRect (bounds, theme::kBorderPx);

    textLayout.draw (g, textArea.toFloat());
}

juce::Font SonikLookAndFeel::getAlertWindowTitleFont()
{
    return theme::mono (15.0f, true);
}

juce::Font SonikLookAndFeel::getAlertWindowMessageFont()
{
    return theme::mono (theme::kFontBody);
}

juce::Font SonikLookAndFeel::getAlertWindowFont()
{
    return theme::mono (theme::kFontMenu);
}
} // namespace sonik::ui
