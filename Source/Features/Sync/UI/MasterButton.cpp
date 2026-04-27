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

    const juce::Colour fill   = isMaster_ ? juce::Colour (0xFF000000) : juce::Colour (0xFFF9F9F9);
    const juce::Colour text   = isMaster_ ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF000000);
    constexpr juce::uint32 borderArgb = 0xFF000000;

    g.setColour (fill);
    g.fillRect (bounds);

    g.setColour (juce::Colour (borderArgb));
    g.drawRect (bounds, 1);

    g.setColour (text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
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
