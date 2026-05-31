#include "StemToggleComponent.h"

// Design-system palette (DESIGN.md §1 — strictly monochrome)
static const juce::Colour kLight { 0xFFFDFDFD };   // surface
static const juce::Colour kDark  { 0xFF2D2D2D };   // ink

// ---------------------------------------------------------------------------
StemToggleComponent::StemToggleComponent (juce::ValueTree stems,
                                          const juce::String& label,
                                          std::vector<juce::Identifier> propertyIds,
                                          const juce::String& tooltip)
    : stemsNode (stems),
      deckNode (stems.getParent()),
      labelText (label),
      propIds (std::move (propertyIds))
{
    jassert (stemsNode.isValid());

    if (tooltip.isNotEmpty())
        setTooltip (tooltip);

    // Observe the deck node when present: it is the parent of the Stems node, so
    // a single listener covers both the deck-level sourceMode property (PRD-0062)
    // and the Stems-child mute properties (recursive change notifications).
    // Stand-alone Stems nodes (e.g. in unit tests) have no parent, so fall back
    // to listening on the Stems node directly.
    if (deckNode.isValid())
        deckNode.addListener (this);
    else
        stemsNode.addListener (this);

    refreshState();
}

StemToggleComponent::~StemToggleComponent()
{
    if (deckNode.isValid())
        deckNode.removeListener (this);
    else
        stemsNode.removeListener (this);
}

// ---------------------------------------------------------------------------
void StemToggleComponent::refreshState()
{
    // Interactive only when the stems are ready AND the deck is actually playing
    // the stem source. In "original" mode the controls are greyed out and inert
    // (PRD-0062 §1.5.4); the underlying mute state is left untouched so it is
    // preserved across original↔stems round-trips. When there is no parent deck
    // (unit-test stand-alone node) sourceMode defaults to "stems", i.e. enabled.
    const bool statusReady = stemsNode.getProperty (IDs::status, "none").toString() == "ready";
    const bool sourceIsOriginal =
        deckNode.getProperty (IDs::sourceMode, "stems").toString() == "original";

    isReady = statusReady && ! sourceIsOriginal;

    // Always visible — readiness is shown via disabled/dimmed paint state

    if (! isReady)
    {
        isMuted = false;
        repaint();
        return;
    }

    // Muted (active) display only when ALL controlled properties are muted
    // (PRD-0023 §1.5.1). A mixed state — some but not all muted, reachable
    // only via an external agent — displays as unmuted, and the next click
    // mutes the whole group.
    bool allMuted = ! propIds.empty();
    for (const auto& pid : propIds)
    {
        if (! static_cast<bool> (stemsNode.getProperty (pid, false)))
        {
            allMuted = false;
            break;
        }
    }
    isMuted = allMuted;
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
        // Active (muted): inverted — dark fill, light text (DESIGN.md §Tactile).
        g.setColour (kDark);
        g.fillRect (bounds);
        g.setColour (kDark);
        g.drawRect (bounds, 2);
        g.setColour (kLight);
    }
    else
    {
        // Inactive (unmuted): light fill, dark text.
        g.setColour (kLight);
        g.fillRect (bounds);
        g.setColour (kDark);
        g.drawRect (bounds, 2);
        g.setColour (kDark);
    }

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));

    // When the cell is taller than wide (vertical stems sidebar) draw the
    // label rotated 90° counter-clockwise so it reads bottom-to-top, matching
    // the Figma deck spec.
    if (bounds.getHeight() > bounds.getWidth())
    {
        juce::Graphics::ScopedSaveState save (g);
        const float cx = static_cast<float> (bounds.getCentreX());
        const float cy = static_cast<float> (bounds.getCentreY());
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));

        // After rotation, swap the rect's width/height around the centre so
        // drawText centres correctly in the rotated coordinate system.
        juce::Rectangle<int> rotated (
            static_cast<int> (cx) - bounds.getHeight() / 2,
            static_cast<int> (cy) - bounds.getWidth()  / 2,
            bounds.getHeight(),
            bounds.getWidth());

        g.drawText (labelText, rotated, juce::Justification::centred, false);
    }
    else
    {
        g.drawText (labelText, bounds, juce::Justification::centred, false);
    }
}

// ---------------------------------------------------------------------------
void StemToggleComponent::mouseDown (const juce::MouseEvent&)
{
    if (! isReady)
        return;

    // Set all controlled properties to one new value in a single gesture
    // (PRD-0023 §1.5.1). Unmute only when every property is currently muted;
    // otherwise (none or mixed) mute the whole group.
    bool allMuted = ! propIds.empty();
    for (const auto& pid : propIds)
    {
        if (! static_cast<bool> (stemsNode.getProperty (pid, false)))
        {
            allMuted = false;
            break;
        }
    }

    const bool newValue = ! allMuted;
    for (const auto& pid : propIds)
        stemsNode.setProperty (pid, newValue, nullptr);
}

// ---------------------------------------------------------------------------
void StemToggleComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                    const juce::Identifier& property)
{
    const bool stemsChange = (changedTree == stemsNode);
    const bool deckChange  = (changedTree == deckNode);
    if (! stemsChange && ! deckChange)
        return;

    bool relevant = false;

    if (stemsChange)
    {
        relevant = (property == IDs::status);
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
    }
    else if (deckChange)
    {
        relevant = (property == IDs::sourceMode);
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
