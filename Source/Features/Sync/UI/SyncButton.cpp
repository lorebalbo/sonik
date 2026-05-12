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

    // DESIGN.md strict monochrome palette — matches QuantizeButtonComponent / KeyLockButton
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFF9F9F9 };

    // Active: dark fill + light text.  Inactive: light fill + dark text.
    const juce::Colour fill = isSynced_ ? kInk     : kSurface;
    const juce::Colour text = isSynced_ ? kSurface : kInk;

    g.setColour (fill);
    g.fillRect (bounds);

    // Mandatory 2px solid #2d2d2d border (DESIGN.md §5 Decks & Transport)
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Font identical to QUANTIZE / KEY buttons
    g.setColour (text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
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
