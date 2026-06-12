#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonik::ui
{
//==============================================================================
// App-wide LookAndFeel implementing the DESIGN.md "1-Bit Deck" chrome for the
// stock JUCE widgets that cannot be custom-painted per component: popup menus
// (context menus, dropdown lists) and combo boxes.
//
// Installed once as the default LookAndFeel in SonikApplication::initialise(),
// so every bare juce::PopupMenu / juce::ComboBox in the application picks it
// up automatically. Components with bespoke LookAndFeels should inherit from
// this class instead of juce::LookAndFeel_V4 so their menus stay consistent.
//
// DESIGN.md rules enforced here:
//   - strict monochrome palette (#2d2d2d ink on #fdfdfd surface)
//   - 2px solid ink borders, zero border-radius, no shadows
//   - highlighted items use full fill inversion (ink fill, surface text)
//   - Space Mono (falls back to the platform monospaced font when absent)
//
// Dropdown menus opened from a button should match the button's width: pass
// PopupMenu::Options().withMinimumWidth (button.getWidth()) at the call site.
//==============================================================================
class SonikLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SonikLookAndFeel();

    // The DESIGN.md body font at the given height (Space Mono when installed).
    juce::Font monoFont (float height, bool bold = false) const;

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

private:
    juce::String fontFamily_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonikLookAndFeel)
};
} // namespace sonik::ui
