#include "StemToggleComponent.h"

// Design-system palette
static const juce::Colour kLight { 0xFFF9F9F9 };
static const juce::Colour kDark  { 0xFF2D2D2D };
static const juce::Colour kGreen { 0xFF0AD691 };
static const juce::Colour kRed   { 0xFFFF3C3B };

// ---------------------------------------------------------------------------
StemToggleComponent::StemToggleComponent (juce::ValueTree stems,
                                          const juce::String& label,
                                          std::vector<juce::Identifier> propertyIds,
                                          const juce::String& tooltip)
    : stemsNode (stems),
      labelText (label),
      propIds (std::move (propertyIds))
{
    jassert (stemsNode.isValid());

    if (tooltip.isNotEmpty())
        setTooltip (tooltip);

    stemsNode.addListener (this);
    refreshState();
}

StemToggleComponent::~StemToggleComponent()
{
    stemsNode.removeListener (this);
}

// ---------------------------------------------------------------------------
void StemToggleComponent::refreshState()
{
    // Check if stems are ready
    isReady = stemsNode.getProperty (IDs::status, "none").toString() == "ready";

    // Always visible — readiness is shown via disabled/dimmed paint state

    if (! isReady)
    {
        isMuted = false;
        repaint();
        return;
    }

    // Muted if ANY of the controlled properties is true
    bool anyMuted = false;
    for (const auto& pid : propIds)
    {
        if (static_cast<bool> (stemsNode.getProperty (pid, false)))
        {
            anyMuted = true;
            break;
        }
    }
    isMuted = anyMuted;
}

// ---------------------------------------------------------------------------
void StemToggleComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    if (! isReady)
    {
        // Disabled: light bg, dimmed text and border
        g.setColour (kLight);
        g.fillRect (bounds);
        g.setColour (kDark.withAlpha (0.3f));
        g.drawRect (bounds, 2);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawText (labelText, bounds, juce::Justification::centred, false);
        return;
    }

    if (isMuted)
    {
        // Muted: light bg, dark text
        g.setColour (kLight);
        g.fillRect (bounds);
        g.setColour (kDark);
        g.drawRect (bounds, 2);
        g.setColour (kDark);
    }
    else
    {
        // Active (unmuted): dark bg, light text
        g.setColour (kDark);
        g.fillRect (bounds);
        g.setColour (kDark);
        g.drawRect (bounds, 2);
        g.setColour (kLight);
    }

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText (labelText, bounds, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
void StemToggleComponent::mouseDown (const juce::MouseEvent&)
{
    if (! isReady)
        return;

    // If any are muted, unmute all. If all unmuted, mute all.
    bool anyMuted = false;
    for (const auto& pid : propIds)
    {
        if (static_cast<bool> (stemsNode.getProperty (pid, false)))
        {
            anyMuted = true;
            break;
        }
    }

    bool newValue = ! anyMuted; // mute all if none were muted
    for (const auto& pid : propIds)
        stemsNode.setProperty (pid, newValue, nullptr);
}

// ---------------------------------------------------------------------------
void StemToggleComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                    const juce::Identifier& property)
{
    if (changedTree != stemsNode)
        return;

    bool relevant = (property == IDs::status);
    if (! relevant)
    {
        for (const auto& pid : propIds)
        {
            if (property == pid)
            {
                relevant = true;
                break;
            }
        }
    }

    if (relevant)
    {
        auto safe = juce::Component::SafePointer<StemToggleComponent> (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;
            safe->refreshState();
            safe->repaint();
        });
    }
}
