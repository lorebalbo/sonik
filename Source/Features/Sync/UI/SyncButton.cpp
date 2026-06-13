#include "SyncButton.h"
#include "Features/Shared/Ui/SonikDraw.h"

SyncButton::SyncButton (juce::ValueTree deckState)
    : juce::Button ("SyncButton"),
      deckState_ (deckState)
{
    setClickingTogglesState (false); // ValueTree is the source of truth
    isSynced_ = static_cast<bool> (deckState_.getProperty (IDs::isSynced, false));
    deckState_.addListener (this);

    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Sync: lock this deck's tempo and phase to the master");
}

SyncButton::~SyncButton()
{
    deckState_.removeListener (this);
}

void SyncButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    sonik::ui::draw::paintLatchButton (g, getLocalBounds(), "SYNC",
                                       { .active  = isSynced_,
                                         .hover   = isMouseOver,
                                         .pressed = isButtonDown });
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
