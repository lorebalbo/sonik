#include "HotCuePadComponent.h"

// ---------------------------------------------------------------------------
// Color/Label popup shown on right-click
// ---------------------------------------------------------------------------

class ColorLabelPopup final : public juce::Component
{
public:
    ColorLabelPopup (int currentColorIndex, const juce::String& currentLabel,
                     std::function<void (int)> onColor,
                     std::function<void (const juce::String&)> onLabel)
        : selectedColor (currentColorIndex),
          onColorSelected (std::move (onColor)),
          onLabelChanged (std::move (onLabel))
    {
        labelEditor.setFont (juce::FontOptions (12.0f));
        labelEditor.setText (currentLabel, juce::dontSendNotification);
        labelEditor.setInputRestrictions (12);
        labelEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFFF9F9F9));
        labelEditor.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xFF000000));
        labelEditor.setColour (juce::TextEditor::textColourId,       juce::Colour (0xFF000000));
        labelEditor.setJustification (juce::Justification::centredLeft);

        labelEditor.onReturnKey = [this]()
        {
            if (onLabelChanged)
                onLabelChanged (labelEditor.getText());
        };

        labelEditor.onFocusLost = [this]()
        {
            if (onLabelChanged)
                onLabelChanged (labelEditor.getText());
        };

        addAndMakeVisible (labelEditor);

        setSize (totalWidth, totalHeight);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFFF3F3F4));

        // Draw border
        g.setColour (juce::Colour (0xFF000000));
        g.drawRect (getLocalBounds(), 1);

        // Draw 4x4 color grid
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                int colorIdx = row * 4 + col;
                auto bounds = getColorSwatchBounds (colorIdx);
                auto colour = HotCueColors::getColour (colorIdx);

                g.setColour (colour);
                g.fillRect (bounds);

                // Selection indicator
                if (colorIdx == selectedColor)
                {
                    g.setColour (juce::Colour (0xFF000000));
                    g.drawRect (bounds, 2);
                }
            }
        }

        // Label header
        g.setColour (juce::Colour (0xFF000000));
        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.drawText ("LABEL", margin, colorGridBottom + 4, totalWidth - margin * 2, 14,
                     juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 16; ++i)
        {
            if (getColorSwatchBounds (i).contains (e.x, e.y))
            {
                selectedColor = i;
                if (onColorSelected)
                    onColorSelected (i);
                repaint();
                return;
            }
        }
    }

    void resized() override
    {
        labelEditor.setBounds (margin, colorGridBottom + 20, totalWidth - margin * 2, 22);
    }

private:
    juce::Rectangle<int> getColorSwatchBounds (int index) const
    {
        int row = index / 4;
        int col = index % 4;
        int x = margin + col * (swatchSize + swatchGap);
        int y = margin + row * (swatchSize + swatchGap);
        return { x, y, swatchSize, swatchSize };
    }

    int selectedColor;
    std::function<void (int)>              onColorSelected;
    std::function<void (const juce::String&)> onLabelChanged;
    juce::TextEditor labelEditor;

    static constexpr int margin       = 8;
    static constexpr int swatchSize   = 22;
    static constexpr int swatchGap    = 3;
    static constexpr int colorGridBottom = margin + 4 * (swatchSize + swatchGap) - swatchGap;
    static constexpr int totalWidth   = margin * 2 + 4 * swatchSize + 3 * swatchGap;
    static constexpr int totalHeight  = colorGridBottom + 20 + 22 + margin;
};

// ---------------------------------------------------------------------------
// HotCuePadComponent
// ---------------------------------------------------------------------------

HotCuePadComponent::HotCuePadComponent (juce::ValueTree dt)
    : deckTree (dt),
      cuePointsNode (dt.getChildWithName (IDs::CuePoints))
{
    setWantsKeyboardFocus (true);
    setRepaintsOnMouseActivity (true);
    deckTree.addListener (this);
}

HotCuePadComponent::~HotCuePadComponent()
{
    deckTree.removeListener (this);
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

bool HotCuePadComponent::isDeckEmpty() const
{
    return deckTree.getProperty (IDs::playbackStatus).toString() == "empty";
}

bool HotCuePadComponent::isCueActive (int padIndex) const
{
    auto cp = cuePointsNode.getChild (padIndex);
    return static_cast<bool> (cp.getProperty (IDs::isValid, false));
}

int HotCuePadComponent::getCueColorIndex (int padIndex) const
{
    auto cp = cuePointsNode.getChild (padIndex);
    return static_cast<int> (cp.getProperty (IDs::colorIndex, 0));
}

juce::String HotCuePadComponent::getCueLabel (int padIndex) const
{
    auto cp = cuePointsNode.getChild (padIndex);
    return cp.getProperty (IDs::label).toString();
}

// ---------------------------------------------------------------------------
// Pad geometry
// ---------------------------------------------------------------------------

juce::Rectangle<int> HotCuePadComponent::getPadBounds (int padIndex) const
{
    if (padIndex < 0 || padIndex >= numPads)
        return {};

    // Center the kTotalW × kPadH strip within the component bounds.
    const int xOff = juce::jmax (0, (getWidth()  - kTotalW) / 2);
    const int yOff = juce::jmax (0, (getHeight() - kPadH)   / 2);

    // Adjacent pads share a single padBorderW border (merged — no visible gap).
    const int x = xOff + padIndex * (kPadW - padBorderW);
    return { x, yOff, kPadW, kPadH };
}

int HotCuePadComponent::getPadIndexAt (int x, int y) const
{
    for (int i = 0; i < numPads; ++i)
    {
        if (getPadBounds (i).contains (x, y))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void HotCuePadComponent::paint (juce::Graphics& g)
{
    bool empty = isDeckEmpty();

    for (int i = 0; i < numPads; ++i)
    {
        auto bounds = getPadBounds (i);
        bool active = isCueActive (i);

        // Monochrome design system:
        //   unset pad  → light background  (#F9F9F9), dark text
        //   set pad    → dark background   (#2D2D2D), light text
        //   empty deck → both dimmed at 30% opacity
        juce::Colour bg, border, textColor;

        if (empty)
        {
            bg        = juce::Colour (0xFFF9F9F9);
            border    = juce::Colour (0xFF2D2D2D).withAlpha (0.3f);
            textColor = juce::Colour (0xFF2D2D2D).withAlpha (0.3f);
        }
        else if (active)
        {
            juce::Colour activeBg = juce::Colour (0xFF2D2D2D);
            if (i == pressedPad)
                activeBg = juce::Colour (0xFF111111);
            else if (i == hoveredPad)
                activeBg = juce::Colour (0xFF444444);

            bg        = activeBg;
            border    = juce::Colour (0xFF2D2D2D);
            textColor = juce::Colour (0xFFF9F9F9);
        }
        else
        {
            juce::Colour inactiveBg = juce::Colour (0xFFF9F9F9);
            if (i == hoveredPad)
                inactiveBg = juce::Colour (0xFFE5E5E5);

            bg        = inactiveBg;
            border    = juce::Colour (0xFF2D2D2D);
            textColor = juce::Colour (0xFF2D2D2D);
        }

        g.setColour (bg);
        g.fillRect (bounds);

        // 2px border. Adjacent pads overlap by 2px so shared edges appear
        // as a single line — matching the Figma CUE BUTTONS component.
        g.setColour (border);
        g.drawRect (bounds, padBorderW);

        // Pad letter (A–H), Space Mono 10px
        g.setColour (textColor);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain));
        g.drawText (juce::String::charToString (padLetters[i]), bounds, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------

void HotCuePadComponent::mouseMove (const juce::MouseEvent& e)
{
    int pad = getPadIndexAt (e.x, e.y);
    if (pad != hoveredPad)
    {
        hoveredPad = pad;
        repaint();
    }
}

void HotCuePadComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPad != -1)
    {
        hoveredPad = -1;
        repaint();
    }
}

void HotCuePadComponent::mouseDown (const juce::MouseEvent& e)
{
    if (isDeckEmpty())
        return;

    int pad = getPadIndexAt (e.x, e.y);
    if (pad < 0)
        return;

    pressedPad = pad;
    repaint();

    // Right-click → color/label popup
    if (e.mods.isPopupMenu())
    {
        if (isCueActive (pad))
            showColorLabelPopup (pad);
        return;
    }

    bool active = isCueActive (pad);

    if (e.mods.isAltDown())
    {
        // Alt+click → delete
        if (active && onDeleteCue)
            onDeleteCue (pad);
    }
    else if (e.mods.isShiftDown())
    {
        // Shift+click → set/overwrite
        if (onSetCue)
            onSetCue (pad);
    }
    else if (active)
    {
        // Click on assigned → trigger
        if (onTriggerCue)
            onTriggerCue (pad);
    }
    else
    {
        // Click on unassigned → set
        if (onSetCue)
            onSetCue (pad);
    }
}

void HotCuePadComponent::mouseUp (const juce::MouseEvent&)
{
    if (pressedPad != -1)
    {
        pressedPad = -1;
        repaint();
    }
}

bool HotCuePadComponent::keyPressed (const juce::KeyPress& key)
{
    // Cmd+Z / Ctrl+Z for undo
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        if (onUndoDelete)
            onUndoDelete();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Color/Label popup
// ---------------------------------------------------------------------------

void HotCuePadComponent::showColorLabelPopup (int padIndex)
{
    int currentColor = getCueColorIndex (padIndex);
    auto currentLabel = getCueLabel (padIndex);

    auto popup = std::make_unique<ColorLabelPopup> (
        currentColor, currentLabel,
        [this, padIndex] (int colorIndex)
        {
            if (onColorChange)
                onColorChange (padIndex, colorIndex);
        },
        [this, padIndex] (const juce::String& label)
        {
            if (onLabelChange)
                onLabelChange (padIndex, label);
        });

    auto padBounds = getPadBounds (padIndex);
    juce::CallOutBox::launchAsynchronously (
        std::move (popup),
        localAreaToGlobal (padBounds),
        nullptr);
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void HotCuePadComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                     const juce::Identifier& property)
{
    // Repaint when deck status or cue point properties change
    if (changedTree == deckTree && property == IDs::playbackStatus)
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->repaint();
        });
    }

    if (changedTree.hasType (IDs::CuePoint) && changedTree.getParent() == cuePointsNode)
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
        {
            if (safeThis != nullptr)
                safeThis->repaint();
        });
    }
}
