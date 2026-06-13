#include "LoopControlComponent.h"
#include "Features/Shared/Ui/SonikTheme.h"

namespace theme = sonik::ui::theme;

LoopControlComponent::LoopControlComponent (juce::ValueTree deck)
    : deckTree (deck),
      loopNode (deck.getChildWithName (IDs::Loop))
{
    loopIsActive  = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    int64_t lIn   = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut  = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
    loopIsDefined = (lIn >= 0 && lOut > lIn);

    deckTree.addListener (this);
}

LoopControlComponent::~LoopControlComponent()
{
    deckTree.removeListener (this);
}

void LoopControlComponent::setActiveAutoLoopBeats (float beats)
{
    if (! juce::exactlyEqual (activeAutoBeats, beats))
    {
        activeAutoBeats = beats;
        repaint();
    }
}

void LoopControlComponent::setPendingLoopIn (bool pending)
{
    if (pendingIn != pending)
    {
        pendingIn = pending;
        repaint();
    }
}

bool LoopControlComponent::isDeckEmpty() const
{
    return deckTree.getProperty (IDs::playbackStatus).toString() == "empty";
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

juce::Rectangle<int> LoopControlComponent::getButtonBounds (int index) const
{
    if (index < 0 || index >= numButtons)
        return {};

    // Use actual component height so the strip adapts to 23px (compact) or 46px (tall) layouts.
    const int btnH = getHeight();
    const int xOff = juce::jmax (0, (getWidth()  - kTotalW) / 2);
    const int yOff = 0;

    // Group A: buttons 0-2 (IN, OUT, LOOP) — standard width, merged 2px borders.
    if (index < 3)
        return { xOff + index * (kBtnW - kBorderW), yOff, kBtnW, btnH };

    // Group B starts after Group A + gap.
    const int xB = xOff + kGroupAW + kGroupGap;

    // index 3: < arrow (half-width)
    if (index == 3)
        return { xB, yOff, kArrowW, btnH };

    // index 4-7: standard buttons (2, 4, 8, 16)
    if (index < 8)
    {
        const int stdIdx = index - 4;  // 0..3
        return { xB + (kArrowW - kBorderW) + stdIdx * (kBtnW - kBorderW),
                 yOff, kBtnW, btnH };
    }

    // index 8: > arrow (half-width)
    return { xB + (kArrowW - kBorderW) + 4 * (kBtnW - kBorderW), yOff, kArrowW, btnH };
}

int LoopControlComponent::getButtonAt (int x, int y) const
{
    for (int i = 0; i < numButtons; ++i)
    {
        if (getButtonBounds (i).contains (x, y))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void LoopControlComponent::paint (juce::Graphics& g)
{
    bool empty = isDeckEmpty();

    for (int i = 0; i < numButtons; ++i)
    {
        auto area = getButtonBounds (i);
        const auto& def = buttons[i];

        bool isActive = false;
        if (def.type == BtnType::Toggle)
            isActive = loopIsActive;
        else if (def.type == BtnType::AutoLoop)
            isActive = loopIsActive && juce::exactlyEqual (activeAutoBeats, def.autoLoopBeats);
        else if (def.type == BtnType::LoopIn)
            isActive = pendingIn;

        // Background
        juce::Colour bg;
        if (empty)
            bg = theme::surface();
        else if (isActive)
            bg = theme::ink();
        else if (i == hoveredButton)
            bg = theme::containerHighest();
        else
            bg = theme::surface();

        g.setColour (bg);
        g.fillRect (area);

        // Border (2px). Because adjacent buttons within a group overlap by 2px,
        // each drawRect reuses its neighbours border, producing a single shared
        // 2px line — the "attached" look from Figma.
        g.setColour (theme::ink().withAlpha (empty ? theme::kDisabledAlpha : 1.0f));
        g.drawRect (area, theme::kBorderPx);

        // Text / label
        juce::Colour textColor;
        if (empty)
            textColor = theme::inkDisabled();
        else if (isActive)
            textColor = theme::surface();
        else
            textColor = theme::ink();

        if (def.type == BtnType::Toggle && ! loopIsActive && loopIsDefined && ! empty)
            textColor = theme::ink().withAlpha (0.5f);

        g.setColour (textColor);
        g.setFont (theme::mono (theme::kFontBody));
        g.drawText (def.label, area, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void LoopControlComponent::mouseDown (const juce::MouseEvent& e)
{
    if (isDeckEmpty())
        return;

    int idx = getButtonAt (e.x, e.y);
    if (idx < 0)
        return;

    const auto& def = buttons[idx];

    switch (def.type)
    {
        case BtnType::LoopIn:
            if (onLoopIn) onLoopIn();
            break;
        case BtnType::LoopOut:
            if (onLoopOut) onLoopOut();
            break;
        case BtnType::Toggle:
            if (loopIsActive)
            {
                if (onToggleLoop) onToggleLoop();
            }
            else if (loopIsDefined)
            {
                if (onReLoop) onReLoop();
            }
            break;
        case BtnType::ArrowLeft:
            if (onLoopHalve) onLoopHalve();
            break;
        case BtnType::ArrowRight:
            if (onLoopDouble) onLoopDouble();
            break;
        case BtnType::AutoLoop:
            if (onAutoLoop) onAutoLoop (def.autoLoopBeats);
            break;
    }
}

void LoopControlComponent::mouseMove (const juce::MouseEvent& e)
{
    int idx = getButtonAt (e.x, e.y);
    if (idx != hoveredButton)
    {
        hoveredButton = idx;
        setMouseCursor (idx >= 0 && ! isDeckEmpty()
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void LoopControlComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredButton != -1)
    {
        hoveredButton = -1;
        repaint();
    }
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void LoopControlComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                      const juce::Identifier& property)
{
    if (changedTree.hasType (IDs::Loop))
    {
        if (property == IDs::active || property == IDs::loopIn || property == IDs::loopOut)
        {
            loopIsActive  = static_cast<bool> (loopNode.getProperty (IDs::active, false));
            int64_t lIn   = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
            int64_t lOut  = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
            loopIsDefined = (lIn >= 0 && lOut > lIn);
            repaint();
        }
    }

    if (changedTree == deckTree && property == IDs::playbackStatus)
        repaint();
}
