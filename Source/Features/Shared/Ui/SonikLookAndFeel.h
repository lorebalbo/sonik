#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonik::ui
{
//==============================================================================
// App-wide LookAndFeel implementing the DESIGN.md "1-Bit Deck" chrome for the
// stock JUCE widgets that cannot be custom-painted per component: popup menus,
// combo boxes, text buttons, scrollbars, tooltips, text editors and alert
// windows.
//
// Installed once as the default LookAndFeel in SonikApplication::initialise(),
// so every bare stock widget in the application picks it up automatically.
// Components with bespoke LookAndFeels should inherit from this class instead
// of juce::LookAndFeel_V4 so their chrome stays consistent.
//
// Typography: the Space Mono typeface (SIL OFL) is embedded in the binary and
// served from getTypefaceForFont() for EVERY font request, so the entire app
// renders in the design system's one family on every machine — no silent
// fallback to the platform font. Take colours/metrics from SonikTheme.
//
// DESIGN.md rules enforced here:
//   - strict monochrome palette (#2d2d2d ink on #fdfdfd surface)
//   - 2px solid ink borders, zero border-radius, no shadows, no gradients
//   - highlighted/active items use full fill inversion (ink fill, surface text)
//   - instant state changes; hover steps the fill to the tonal scale
//
// Dropdown menus opened from a button should match the button's width: pass
// PopupMenu::Options().withMinimumWidth (button.getWidth()) at the call site.
//==============================================================================
class SonikLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SonikLookAndFeel();

    // The DESIGN.md body font at the given height (embedded Space Mono).
    juce::Font monoFont (float height, bool bold = false) const;

    //--------------------------------------------------------------------------
    // Typography — every font in the app resolves to the embedded Space Mono.
    //--------------------------------------------------------------------------
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    //--------------------------------------------------------------------------
    // PopupMenu
    //--------------------------------------------------------------------------
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon,
                            const juce::Colour* textColour) override;

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

    void drawPopupMenuUpDownArrow (juce::Graphics&, int width, int height,
                                   bool isScrollUpArrow) override;

    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;

    // Menus carry no shadow at all — neither the native window drop shadow
    // nor a painted one.
    int getMenuWindowFlags() override;

    //--------------------------------------------------------------------------
    // ComboBox
    //--------------------------------------------------------------------------
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    //--------------------------------------------------------------------------
    // TextButton — the stock JUCE button becomes the DESIGN.md tactile square
    // (flat fill, 2px ink border, full inversion when down/toggled).
    //--------------------------------------------------------------------------
    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    //--------------------------------------------------------------------------
    // ScrollBar — flat track on the tonal scale, solid ink thumb, no buttons.
    //--------------------------------------------------------------------------
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y,
                        int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

    bool areScrollbarButtonsVisible() override;

    //--------------------------------------------------------------------------
    // Tooltip — flat surface panel with the mandatory 2px ink border.
    //--------------------------------------------------------------------------
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                           juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;

    void drawTooltip (juce::Graphics&, const juce::String& text,
                      int width, int height) override;

    //--------------------------------------------------------------------------
    // AlertWindow — flat surface panel, 2px ink border, zero radius, no icon.
    //--------------------------------------------------------------------------
    void drawAlertBox (juce::Graphics&, juce::AlertWindow&,
                       const juce::Rectangle<int>& textArea,
                       juce::TextLayout&) override;

    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

private:
    juce::Typeface::Ptr regularTypeface_;
    juce::Typeface::Ptr boldTypeface_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonikLookAndFeel)
};
} // namespace sonik::ui
