#include "StemToggleComponent.h"

// Monochrome palette (DESIGN.md)
static constexpr juce::uint32 kBlack   = 0xff000000;
static constexpr juce::uint32 kWhite   = 0xfff9f9f9;
static constexpr juce::uint32 kSurface = 0xffe2e2e2;

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

    // Only visible when stems are ready
    setVisible (isReady);

    if (! isReady)
        return;

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
    auto bounds = getLocalBounds().toFloat();

    if (isMuted)
    {
        // Muted: surface fill, dark text at 50% opacity
        g.setColour (juce::Colour (kSurface));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kBlack));
        g.drawRect (bounds, 1.0f);
        g.setColour (juce::Colour (kBlack).withAlpha (0.5f));
    }
    else
    {
        // Active (unmuted): black fill, white text
        g.setColour (juce::Colour (kBlack));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kWhite));
        g.drawRect (bounds, 1.0f);
        g.setColour (juce::Colour (kWhite));
    }

    g.setFont (juce::Font (juce::FontOptions (10.0f)));
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
