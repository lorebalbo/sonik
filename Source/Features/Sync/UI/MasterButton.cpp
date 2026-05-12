#include "MasterButton.h"

MasterButton::MasterButton (juce::ValueTree deckState,
                             MasterClockManager& manager,
                             int deckIndex)
    : juce::Button ("MasterButton"),
      deckState_ (deckState),
      manager_ (manager),
      deckIndex_ (deckIndex)
{
    setClickingTogglesState (false); // ValueTree / MasterClockManager is the source of truth
    isMaster_ = static_cast<bool> (deckState_.getProperty (IDs::isMaster, false));
    deckState_.addListener (this);
}

MasterButton::~MasterButton()
{
    deckState_.removeListener (this);
}

void MasterButton::paintButton (juce::Graphics& g, bool /*isMouseOver*/, bool /*isButtonDown*/)
{
    const auto bounds = getLocalBounds();

    // DESIGN.md strict monochrome palette — matches QuantizeButtonComponent / KeyLockButton
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFF9F9F9 };

    // Active: dark fill + light text.  Inactive: light fill + dark text.
    const juce::Colour fill = isMaster_ ? kInk     : kSurface;
    const juce::Colour text = isMaster_ ? kSurface : kInk;

    g.setColour (fill);
    g.fillRect (bounds);

    // Mandatory 2px solid #2d2d2d border (DESIGN.md §5 Decks & Transport)
    g.setColour (kInk);
    g.drawRect (bounds, 2);

    // Font identical to QUANTIZE / KEY buttons
    g.setColour (text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText ("MASTER", bounds, juce::Justification::centred);
}

void MasterButton::clicked()
{
    // No-op if this deck is already the master clock source
    if (isMaster_)
        return;

    // Delegate to MasterClockManager — it handles mutual exclusion and isSynced clearing
    manager_.setMaster (deckIndex_);
}

void MasterButton::valueTreePropertyChanged (juce::ValueTree& tree,
                                              const juce::Identifier& property)
{
    if (tree == deckState_ && property == IDs::isMaster)
    {
        const bool newVal = static_cast<bool> (tree[property]);
        if (newVal != isMaster_)
        {
            isMaster_ = newVal;
            juce::MessageManager::callAsync (
                [safeThis = juce::Component::SafePointer<juce::Component> (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
        }
    }
}
