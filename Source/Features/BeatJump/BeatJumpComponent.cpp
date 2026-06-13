#include "BeatJumpComponent.h"
#include "Features/Shared/Ui/SonikTheme.h"

namespace theme = sonik::ui::theme;

BeatJumpComponent::BeatJumpComponent (juce::ValueTree tree)
    : deckTree (tree)
{
    currentSize = static_cast<double> (deckTree.getProperty (IDs::beatJumpSize, 4.0));
    setTooltip ("Beat Jump");
    deckTree.addListener (this);
}

BeatJumpComponent::~BeatJumpComponent()
{
    deckTree.removeListener (this);
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

// Returns bounds for button at index:
//   0 = ◄ (arrow back)   width=kArrowW
//   1-4 = size buttons    width=kBtnW
//   5 = ► (arrow fwd)    width=kArrowW
juce::Rectangle<int> BeatJumpComponent::getButtonBounds (int idx) const
{
    const int xOff = juce::jmax (0, (getWidth()  - kTotalW) / 2);
    const int yOff = juce::jmax (0, (getHeight() - kBtnH)   / 2);

    if (idx == 0)
        return { xOff, yOff, kArrowW, kBtnH };

    if (idx >= 1 && idx <= 4)
    {
        const int x = xOff + (kArrowW - kBorderW) + (idx - 1) * (kBtnW - kBorderW);
        return { x, yOff, kBtnW, kBtnH };
    }

    // idx == 5 (forward arrow)
    const int x = xOff + (kArrowW - kBorderW) + 4 * (kBtnW - kBorderW);
    return { x, yOff, kArrowW, kBtnH };
}

BeatJumpComponent::Region BeatJumpComponent::getRegionAt (int x, int y) const
{
    auto pt = juce::Point<int> (x, y);
    for (int i = 0; i <= 5; ++i)
    {
        if (getButtonBounds (i).contains (pt))
        {
            if (i == 0) return Region::Backward;
            if (i == 5) return Region::Forward;
            // i == 1..4 → Size0..Size3
            return static_cast<Region> (static_cast<int> (Region::Size0) + (i - 1));
        }
    }
    return Region::None;
}

int BeatJumpComponent::regionToSizeIndex (Region r) noexcept
{
    switch (r)
    {
        case Region::Size0:    return 0;
        case Region::Size1:    return 1;
        case Region::Size2:    return 2;
        case Region::Size3:    return 3;
        case Region::None:
        case Region::Backward:
        case Region::Forward:  return -1;
    }
    return -1;
}

juce::String BeatJumpComponent::formatSize (double beats) const
{
    if (beats == 0.5)  return "1/2";
    if (beats == 1.0)  return "1";
    if (beats == 2.0)  return "2";
    if (beats == 4.0)  return "4";
    if (beats == 8.0)  return "8";
    if (beats == 16.0) return "16";
    if (beats == 32.0) return "32";
    return juce::String (beats);
}

bool BeatJumpComponent::isDeckEmpty() const
{
    return deckTree.getProperty (IDs::playbackStatus).toString() == "empty";
}

bool BeatJumpComponent::hasBeatgrid() const
{
    auto bg = deckTree.getChildWithName (IDs::BeatGrid);
    if (bg.isValid())
        return static_cast<double> (bg.getProperty (IDs::bpm, 0.0)) > 0.0;
    return false;
}

void BeatJumpComponent::triggerFlash (Region r)
{
    flashRegion = r;
    flashStartTime = juce::Time::currentTimeMillis();

    // Schedule repaint after flash duration to clear
    auto safeThis = juce::Component::SafePointer (this);
    juce::Timer::callAfterDelay (flashDurationMs + 10, [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->repaint();
    });
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void BeatJumpComponent::paint (juce::Graphics& g)
{
    bool empty  = isDeckEmpty();
    bool hasBg  = hasBeatgrid();
    float alpha = empty ? theme::kDisabledAlpha : 1.0f;

    // Check if flash is still active
    bool flashActive = false;
    if (flashRegion != Region::None)
    {
        int64_t elapsed = juce::Time::currentTimeMillis() - flashStartTime;
        flashActive = elapsed < flashDurationMs;
        if (! flashActive)
            flashRegion = Region::None;
    }

    // Draw 6 buttons: [◄] [2] [4] [8] [16] [►]
    for (int idx = 0; idx <= 5; ++idx)
    {
        auto bounds = getButtonBounds (idx);

        // Determine which Region this button is
        Region btnRegion;
        if (idx == 0)       btnRegion = Region::Backward;
        else if (idx == 5)  btnRegion = Region::Forward;
        else                btnRegion = static_cast<Region> (static_cast<int> (Region::Size0) + (idx - 1));

        bool isSizeBtn   = (idx >= 1 && idx <= 4);
        bool isActive    = isSizeBtn && juce::exactlyEqual (kSizes[idx - 1], currentSize) && (! empty) && hasBg;
        bool isHovered   = (hoveredRegion == btnRegion) && (! empty);
        bool isFlashing  = flashActive && (flashRegion == btnRegion);

        // Label
        juce::String label;
        if (idx == 0) label = juce::String::charToString (0x25C4);
        else if (idx == 5) label = juce::String::charToString (0x25BA);
        else label = formatSize (kSizes[idx - 1]);

        // Uniform alpha for all buttons — matches Loop panel pattern.
        // Beatgrid availability only controls interactivity (mouseDown), not visuals.
        juce::Colour bg, fg;
        if (isFlashing || isActive)
        {
            // Active / flashing: inverted (dark bg, light text)
            bg = theme::ink().withAlpha (alpha);
            fg = theme::surface().withAlpha (alpha);
        }
        else if (isHovered)
        {
            bg = theme::containerHighest().withAlpha (alpha);
            fg = theme::ink().withAlpha (alpha);
        }
        else
        {
            bg = theme::surface().withAlpha (alpha);
            fg = theme::ink().withAlpha (alpha);
        }

        g.setColour (bg);
        g.fillRect (bounds);

        g.setColour (theme::ink().withAlpha (alpha));
        g.drawRect (bounds, theme::kBorderPx);

        g.setColour (fg);
        g.setFont (theme::mono (theme::kFontBody));
        g.drawText (label, bounds, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void BeatJumpComponent::mouseDown (const juce::MouseEvent& e)
{
    if (isDeckEmpty())
        return;

    auto region = getRegionAt (e.x, e.y);

    switch (region)
    {
        case Region::Backward:
            triggerFlash (Region::Backward);
            if (onJumpBackward)
                onJumpBackward();
            repaint();
            break;

        case Region::Forward:
            triggerFlash (Region::Forward);
            if (onJumpForward)
                onJumpForward();
            repaint();
            break;

        case Region::Size0:
        case Region::Size1:
        case Region::Size2:
        case Region::Size3:
        {
            if (! hasBeatgrid())
                break;
            int sIdx = regionToSizeIndex (region);
            deckTree.setProperty (IDs::beatJumpSize, kSizes[sIdx], nullptr);
            repaint();
            break;
        }

        case Region::None:
            break;
    }
}

void BeatJumpComponent::mouseMove (const juce::MouseEvent& e)
{
    auto newRegion = getRegionAt (e.x, e.y);
    if (newRegion != hoveredRegion)
    {
        hoveredRegion = newRegion;

        setMouseCursor (hoveredRegion != Region::None && ! isDeckEmpty()
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);

        switch (hoveredRegion)
        {
            case Region::Backward: setTooltip ("Beat Jump Backward"); break;
            case Region::Forward:  setTooltip ("Beat Jump Forward"); break;
            case Region::Size0:    setTooltip ("Jump 2 beats"); break;
            case Region::Size1:    setTooltip ("Jump 4 beats"); break;
            case Region::Size2:    setTooltip ("Jump 8 beats"); break;
            case Region::Size3:    setTooltip ("Jump 16 beats"); break;
            case Region::None:     setTooltip ("Beat Jump"); break;
        }

        repaint();
    }
}

void BeatJumpComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRegion != Region::None)
    {
        hoveredRegion = Region::None;
        repaint();
    }
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void BeatJumpComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                    const juce::Identifier& property)
{
    if (changedTree == deckTree)
    {
        if (property == IDs::beatJumpSize)
        {
            double newSize = static_cast<double> (changedTree[property]);
            if (! juce::exactlyEqual (newSize, currentSize))
            {
                currentSize = newSize;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
            }
        }
        else if (property == IDs::playbackStatus)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }

    // BeatGrid bpm changed
    if (changedTree.hasType (IDs::BeatGrid) && property == IDs::bpm)
    {
        if (changedTree.getParent() == deckTree)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }
}
