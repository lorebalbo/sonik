#include "SyncButton.h"

SyncButton::SyncButton (juce::ValueTree deckState)
    : juce::Button ("SyncButton"),
      deckState_ (deckState)
{
    setClickingTogglesState (false); // ValueTree is the source of truth
    isSynced_ = static_cast<bool> (deckState_.getProperty (IDs::isSynced, false));
    deckState_.addListener (this);
}

SyncButton::~SyncButton()
{
    deckState_.removeListener (this);
}

void SyncButton::paintButton (juce::Graphics& g, bool /*isMouseOver*/, bool /*isButtonDown*/)
{
    const auto bounds = getLocalBounds();

    // Strict monochrome: active=black bg, inactive=white bg
    const juce::Colour fill   = isSynced_ ? juce::Colour (0xFF000000) : juce::Colour (0xFFF9F9F9);
    const juce::Colour text   = isSynced_ ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF000000);
    constexpr juce::uint32 borderArgb = 0xFF000000;

    g.setColour (fill);
    g.fillRect (bounds);

    g.setColour (juce::Colour (borderArgb));
    g.drawRect (bounds, 1);

    g.setColour (text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.drawText ("SYNC", bounds, juce::Justification::centred);
}

void SyncButton::clicked()
{
    // No-op if this deck is the current master (master cannot self-sync)
    if (static_cast<bool> (deckState_.getProperty (IDs::isMaster, false)))
        return;

    const bool newValue = ! isSynced_;
    deckState_.setProperty (IDs::isSynced, newValue, nullptr);
}

void SyncButton::valueTreePropertyChanged (juce::ValueTree& tree,
                                            const juce::Identifier& property)
{
    if (tree == deckState_ && property == IDs::isSynced)
    {
        const bool newVal = static_cast<bool> (tree[property]);
        if (newVal != isSynced_)
        {
            isSynced_ = newVal;
            juce::MessageManager::callAsync (
                [safeThis = juce::Component::SafePointer<juce::Component> (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
        }
    }
}
